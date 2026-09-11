#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "beacon_generator.h"
#include "config_parser.h"
#include "rgb_led.h"
#include "ble_multi_adv.h"
static const char *TAG = "probe_main";

static probe_config_t s_config;
static virtual_network_t s_networks[MAX_VIRTUAL_NETWORKS];
static uint8_t s_frame_buffer[MAX_BEACON_FRAME_LEN];
static uint32_t s_probe_responses_sent = 0;
static uint32_t s_deauth_frames_intercepted = 0;
static uint32_t s_auth_attempts_intercepted = 0;

#define CLIENT_CACHE_SIZE 64

typedef struct {
    uint8_t mac[6];
    int8_t last_rssi;
    int64_t last_seen_us;
} client_entry_t;

static client_entry_t s_client_cache[CLIENT_CACHE_SIZE];
static size_t s_client_cache_count = 0;
static uint32_t s_unique_stations_seen = 0;

static void client_cache_touch(const uint8_t *mac, int8_t rssi)
{
    if (!mac || (mac[0] & 0x01) != 0) {
        return; // Ignore broadcast or multicast addresses
    }

    int64_t now_us = esp_timer_get_time();

    for (size_t i = 0; i < s_client_cache_count; i++) {
        if (memcmp(s_client_cache[i].mac, mac, 6) == 0) {
            s_client_cache[i].last_rssi = rssi;
            s_client_cache[i].last_seen_us = now_us;
            return;
        }
    }

    s_unique_stations_seen++;

    if (s_client_cache_count < CLIENT_CACHE_SIZE) {
        memcpy(s_client_cache[s_client_cache_count].mac, mac, 6);
        s_client_cache[s_client_cache_count].last_rssi = rssi;
        s_client_cache[s_client_cache_count].last_seen_us = now_us;
        s_client_cache_count++;
    } else {
        size_t oldest_idx = 0;
        int64_t oldest_time = s_client_cache[0].last_seen_us;
        for (size_t i = 1; i < CLIENT_CACHE_SIZE; i++) {
            if (s_client_cache[i].last_seen_us < oldest_time) {
                oldest_time = s_client_cache[i].last_seen_us;
                oldest_idx = i;
            }
        }
        memcpy(s_client_cache[oldest_idx].mac, mac, 6);
        s_client_cache[oldest_idx].last_rssi = rssi;
        s_client_cache[oldest_idx].last_seen_us = now_us;
    }

    ESP_LOGI(TAG, "[NEW STATION #%lu] Discovered client %02X:%02X:%02X:%02X:%02X:%02X | RSSI: %d dBm",
             (unsigned long)s_unique_stations_seen,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             rssi);
}

static const char *wifi_deauth_reason_str(uint16_t reason)
{
    switch (reason) {
    case 1: return "Unspecified";
    case 2: return "Prev Auth Invalid";
    case 3: return "STA Leaving";
    case 4: return "Inactivity";
    case 6: return "Class 2 Non-Auth";
    case 7: return "Class 3 Non-Assoc";
    case 8: return "Disassoc STA Leaving";
    case 15: return "4-Way Handshake Timeout";
    default: return "Reason Code";
    }
}

/**
 * @brief Promiscuous Management Frame Interceptor & Honeypot Analysis Callback.
 *
 * Intercepts raw IEEE 802.11 management frames directly from the RF PHY layer.
 * Performs passive reconnaissance and active interaction analysis:
 *  - Wildcard Probes (Subtype 0x40, len 0): Detects nearby client network discovery sweeps.
 *  - Directed Probes (Subtype 0x40, matching SSID): Injects synthetic Probe Responses.
 *  - Authentication & Association Requests (0x00, 0x20, 0xB0): Detects client connection attempts.
 *  - Deauthentication / Disassociation (0xC0, 0xA0): Detects wireless disruption or teardown events.
 */
static void wifi_promiscuous_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) {
        return;
    }

    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *payload = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;

    if (len < 24) {
        return;
    }

    uint8_t fc0 = payload[0];
    if ((fc0 & 0x0C) != 0x00) {
        return; // Non-management frame
    }

    uint8_t subtype = fc0 & 0xF0;

    // 1. Intercept Client Association / Reassociation (0x00, 0x20) or Authentication (0xB0)
    if (subtype == 0x00 || subtype == 0x20 || subtype == 0xB0) {
        const uint8_t *dest_bssid = &payload[4]; // Address 1 (DA / BSSID)
        const uint8_t *client_mac = &payload[10]; // Address 2 (SA)
        client_cache_touch(client_mac, pkt->rx_ctrl.rssi);
        for (size_t i = 0; i < s_config.network_count; i++) {
            if (memcmp(dest_bssid, s_networks[i].bssid, 6) == 0) {
                s_auth_attempts_intercepted++;
                if (s_config.enable_led) {
                    rgb_led_trigger_event(LED_EVENT_WIFI_AUTH_ASSOC);
                }
                static int64_t s_last_auth_log_us = 0;
                int64_t now_us = esp_timer_get_time();
                if (now_us - s_last_auth_log_us >= 50000) {
                    s_last_auth_log_us = now_us;
                    const char *type_name = (subtype == 0xB0) ? "Auth" :
                                            (subtype == 0x00) ? "Assoc" : "Reassoc";
                    ESP_LOGI(TAG, "[AUTH INTERCEPT] Client %02X:%02X:%02X:%02X:%02X:%02X sent %s to '%s' (BSSID: %02X:%02X:%02X:%02X:%02X:%02X) | RSSI: %d dBm",
                             client_mac[0], client_mac[1], client_mac[2], client_mac[3], client_mac[4], client_mac[5],
                             type_name, s_networks[i].ssid,
                             dest_bssid[0], dest_bssid[1], dest_bssid[2], dest_bssid[3], dest_bssid[4], dest_bssid[5],
                             pkt->rx_ctrl.rssi);
                }
                break;
            }
        }
        return;
    }

    // 2. Intercept Deauthentication (0xC0) or Disassociation (0xA0)
    if (subtype == 0xC0 || subtype == 0xA0) {
        const uint8_t *addr1 = &payload[4];  // Destination / Target Address
        const uint8_t *addr2 = &payload[10]; // Source / Transmitter Address
        const uint8_t *addr3 = (len >= 22) ? &payload[16] : NULL; // BSSID

        client_cache_touch(addr2, pkt->rx_ctrl.rssi);

        const char *matched_ssid = "External-BSSID";
        bool is_virtual_net = false;
        const uint8_t *bssid_ptr = addr3 ? addr3 : addr1;

        for (size_t i = 0; i < s_config.network_count; i++) {
            if (memcmp(addr1, s_networks[i].bssid, 6) == 0 ||
                memcmp(addr2, s_networks[i].bssid, 6) == 0 ||
                (addr3 && memcmp(addr3, s_networks[i].bssid, 6) == 0)) {
                matched_ssid = s_networks[i].ssid;
                bssid_ptr = s_networks[i].bssid;
                is_virtual_net = true;
                break;
            }
        }

        s_deauth_frames_intercepted++;

        if (s_config.enable_led && is_virtual_net) {
            rgb_led_trigger_event(LED_EVENT_WIFI_DEAUTH);
        }

        // Live UART telemetry alert (throttled to max 20 logs/sec to prevent console saturation during floods)
        static int64_t s_last_deauth_log_us = 0;
        int64_t now_us = esp_timer_get_time();
        if (now_us - s_last_deauth_log_us >= 50000) {
            s_last_deauth_log_us = now_us;
            uint16_t reason_code = (len >= 26) ? (payload[24] | ((uint16_t)payload[25] << 8)) : 0;
            ESP_LOGW(TAG, "[DEAUTH ALERT] %s Frame: Target=%02X:%02X:%02X:%02X:%02X:%02X | Source=%02X:%02X:%02X:%02X:%02X:%02X | BSSID=%02X:%02X:%02X:%02X:%02X:%02X (%s) | Reason=%u (%s) | RSSI=%d dBm",
                     (subtype == 0xC0) ? "Deauth" : "Disassoc",
                     addr1[0], addr1[1], addr1[2], addr1[3], addr1[4], addr1[5],
                     addr2[0], addr2[1], addr2[2], addr2[3], addr2[4], addr2[5],
                     bssid_ptr[0], bssid_ptr[1], bssid_ptr[2], bssid_ptr[3], bssid_ptr[4], bssid_ptr[5],
                     matched_ssid,
                     reason_code, wifi_deauth_reason_str(reason_code),
                     pkt->rx_ctrl.rssi);
        }
        return;
    }

    // 3. Intercept Client Active Probe Requests (Subtype: 0x40)
    if (subtype != 0x40 || len < 26) {
        return;
    }

    const uint8_t *client_mac = &payload[10]; // Source Address (SA)
    client_cache_touch(client_mac, pkt->rx_ctrl.rssi);

    uint8_t tag_num = payload[24];
    uint8_t tag_len = payload[25];

    uint8_t resp_buf[MAX_BEACON_FRAME_LEN];
    bool answered = false;

    // Case A: Wildcard Probe Request (SSID tag length == 0)
    // Client device is querying for any and all in-range access points
    if (tag_num == 0 && tag_len == 0) {
        if (s_config.enable_led) {
            rgb_led_trigger_event(LED_EVENT_WIFI_WILDCARD_SCAN);
        }
        static int64_t s_last_wildcard_log_us = 0;
        int64_t now_us = esp_timer_get_time();
        if (now_us - s_last_wildcard_log_us >= 100000) {
            s_last_wildcard_log_us = now_us;
            ESP_LOGI(TAG, "[WILDCARD SCAN] Client %02X:%02X:%02X:%02X:%02X:%02X sweeping RF space | RSSI: %d dBm | Replying with %u honeypots",
                     client_mac[0], client_mac[1], client_mac[2], client_mac[3], client_mac[4], client_mac[5],
                     pkt->rx_ctrl.rssi, (unsigned int)s_config.network_count);
        }
        for (size_t i = 0; i < s_config.network_count; i++) {
            int rlen = beacon_generator_build_probe_response(&s_networks[i], client_mac, resp_buf, sizeof(resp_buf));
            if (rlen > 0) {
                if (esp_wifi_80211_tx(WIFI_IF_AP, resp_buf, rlen, false) == ESP_OK) {
                    s_probe_responses_sent++;
                    answered = true;
                }
            }
        }
    } else if (tag_num == 0 && tag_len > 0 && tag_len <= 32 && (26 + tag_len) <= len) {
        // Case B: Directed Probe Request
        // Client device is explicitly hunting for a specific known SSID
        for (size_t i = 0; i < s_config.network_count; i++) {
            if (strlen(s_networks[i].ssid) == tag_len &&
                memcmp(s_networks[i].ssid, &payload[26], tag_len) == 0) {
                int rlen = beacon_generator_build_probe_response(&s_networks[i], client_mac, resp_buf, sizeof(resp_buf));
                if (rlen > 0) {
                    if (esp_wifi_80211_tx(WIFI_IF_AP, resp_buf, rlen, false) == ESP_OK) {
                        s_probe_responses_sent++;
                        answered = true;
                        static int64_t s_last_probe_log_us = 0;
                        int64_t now_us = esp_timer_get_time();
                        if (now_us - s_last_probe_log_us >= 100000) {
                            s_last_probe_log_us = now_us;
                            ESP_LOGI(TAG, "[PROBE HIT] Client %02X:%02X:%02X:%02X:%02X:%02X queried '%s' -> Injected response | RSSI: %d dBm",
                                     client_mac[0], client_mac[1], client_mac[2], client_mac[3], client_mac[4], client_mac[5],
                                     s_networks[i].ssid, pkt->rx_ctrl.rssi);
                        }
                    }
                }
                break;
            }
        }

        // Case C: Client is probing for an external/foreign SSID (Preferred Network List leak)
        if (!answered) {
            char foreign_ssid[33] = {0};
            memcpy(foreign_ssid, &payload[26], tag_len);
            foreign_ssid[tag_len] = '\0';
            static int64_t s_last_sniff_log_us = 0;
            int64_t now_sniff_us = esp_timer_get_time();
            if (now_sniff_us - s_last_sniff_log_us >= 100000) {
                s_last_sniff_log_us = now_sniff_us;
                ESP_LOGI(TAG, "[PROBE SNIFF] Client %02X:%02X:%02X:%02X:%02X:%02X probing foreign SSID '%s' | RSSI: %d dBm",
                         client_mac[0], client_mac[1], client_mac[2], client_mac[3], client_mac[4], client_mac[5],
                         foreign_ssid, pkt->rx_ctrl.rssi);
            }
        }
    }

    if (answered && s_config.enable_led) {
        // Trigger visual telemetry event when an active client query is satisfied
        rgb_led_trigger_event(LED_EVENT_PROBE_RESPONSE);
    }
}

/**
 * @brief Initializes the ESP32-C6 Wi-Fi subsystem in SoftAP mode with promiscuous packet RX.
 */
static void wifi_init_probe(void)
{
    ESP_LOGI(TAG, "Initializing network interface and default event loop...");
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    // Internal SoftAP configuration (hidden, allows low-level raw 802.11 transmission)
    wifi_config_t ap_config = {
        .ap = {
            .ssid = "PhantomProbe-C6-Internal",
            .ssid_len = strlen("PhantomProbe-C6-Internal"),
            .channel = s_config.channel,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
            .ssid_hidden = 1,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(s_config.channel, WIFI_SECOND_CHAN_NONE));

    if (s_config.enable_probe_responder) {
        wifi_promiscuous_filter_t filter = {
            .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT
        };
        ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
        ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_promiscuous_rx_cb));
        ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
        ESP_LOGI(TAG, "Promiscuous 802.11 management frame analyzer & probe responder: ACTIVE");
    } else {
        ESP_LOGI(TAG, "Promiscuous 802.11 management frame analyzer: DISABLED");
    }

    ESP_LOGI(TAG, "RF PHY transmitter configured on 2.4 GHz Channel %u with raw 802.11 TX injection.", s_config.channel);
}

/**
 * @brief Continuous task responsible for synthesizing and transmitting synthetic 802.11 beacon frames.
 */
static void beacon_broadcast_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting Wi-Fi beacon injection loop (%u virtual BSSIDs, cycle=%lu ms, gap=%lu ms)...",
             (unsigned int)s_config.network_count,
             (unsigned long)s_config.burst_interval_ms,
             (unsigned long)s_config.burst_gap_ms);

    TickType_t last_stats_time = xTaskGetTickCount();
    uint32_t total_frames_sent = 0;
    uint32_t burst_loop_counter = 0;

    while (1) {
        // Trigger subtle telemetry blip periodically (~every 1.5s) to indicate active RF beaconing
        if (s_config.enable_led && (++burst_loop_counter % 25 == 0)) {
            rgb_led_trigger_event(LED_EVENT_BEACON_BURST);
        }

        // Transmit all virtual BSSID beacons in rapid succession
        for (size_t i = 0; i < s_config.network_count; i++) {
            int len = beacon_generator_build_frame(&s_networks[i], s_frame_buffer, sizeof(s_frame_buffer));
            if (len > 0) {
                esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, s_frame_buffer, len, false);
                if (err == ESP_OK) {
                    total_frames_sent++;
                } else {
                    ESP_LOGD(TAG, "Tx injection error on %s: %s", s_networks[i].ssid, esp_err_to_name(err));
                }
            }
            if (s_config.burst_gap_ms > 0) {
                vTaskDelay(pdMS_TO_TICKS(s_config.burst_gap_ms));
            }
        }

        // Inter-burst pause
        vTaskDelay(pdMS_TO_TICKS(s_config.burst_interval_ms));

        // Periodic RF Telemetry Console Output (every 5 seconds)
        if ((xTaskGetTickCount() - last_stats_time) >= pdMS_TO_TICKS(5000)) {
            last_stats_time = xTaskGetTickCount();
            int64_t uptime_sec = esp_timer_get_time() / 1000000;
            uint32_t hours = (uint32_t)(uptime_sec / 3600);
            uint32_t minutes = (uint32_t)((uptime_sec % 3600) / 60);
            uint32_t seconds = (uint32_t)(uptime_sec % 60);

            ESP_LOGI(TAG, "=== RF Probe Telemetry [Uptime: %02lu:%02lu:%02lu | Stations: %lu | Frames: %lu | Probe Resp: %lu | Deauths: %lu | Auths: %lu | Free Heap: %lu B] ===",
                     (unsigned long)hours, (unsigned long)minutes, (unsigned long)seconds,
                     (unsigned long)s_unique_stations_seen,
                     (unsigned long)total_frames_sent,
                     (unsigned long)s_probe_responses_sent,
                     (unsigned long)s_deauth_frames_intercepted,
                     (unsigned long)s_auth_attempts_intercepted,
                     (unsigned long)esp_get_free_heap_size());
            for (size_t i = 0; i < s_config.network_count; i++) {
                ESP_LOGI(TAG, "  BSSID: %02X:%02X:%02X:%02X:%02X:%02X | CH: %u | Sent: %-6lu | SSID: %s",
                         s_networks[i].bssid[0], s_networks[i].bssid[1], s_networks[i].bssid[2],
                         s_networks[i].bssid[3], s_networks[i].bssid[4], s_networks[i].bssid[5],
                         s_networks[i].channel,
                         (unsigned long)s_networks[i].beacons_sent,
                         s_networks[i].ssid);
            }
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "============================================================");
    ESP_LOGI(TAG, " PhantomProbe-C6: Wireless RF Security & Telemetry Probe    ");
    ESP_LOGI(TAG, " Target Architecture: ESP32-C6 RISC-V 160MHz (Wi-Fi 6 / BLE 5) ");
    ESP_LOGI(TAG, " Mode: Multi-BSSID Honeypot Simulation & BLE Reconnaissance ");
    ESP_LOGI(TAG, "============================================================");

    // Initialize Non-Volatile Storage (NVS)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 1. Load configuration from declarative manifest (ssid_list.conf)
    config_parser_load(&s_config);

    // 2. Initialize onboard WS2812 RGB LED (GPIO8) visual telemetry engine
    if (s_config.enable_led) {
        rgb_led_init(s_config.led_brightness);
    }

    // 3. Initialize Wi-Fi Subsystem & Multi-BSSID Frame Injection Engine
    wifi_init_probe();
    beacon_generator_init_networks(s_networks, s_config.ssids, s_config.network_count, s_config.channel);

    xTaskCreatePinnedToCore(
        beacon_broadcast_task,
        "probe_wifi_tx",
        4096,
        NULL,
        5,
        NULL,
        0
    );

    // 4. Initialize Multi-Identity Bluetooth Low Energy Peripheral Reconnaissance Engine
    if (s_config.enable_ble && s_config.ble_name_count > 0) {
        ble_config_t ble_cfg;
        ble_cfg.name_count = s_config.ble_name_count;
        ble_cfg.rotation_interval_ms = s_config.ble_adv_interval_ms;
        ble_cfg.accessory_type = s_config.ble_accessory_type;
        for (size_t i = 0; i < s_config.ble_name_count; i++) {
            strncpy(ble_cfg.names[i], s_config.ble_names[i], MAX_BLE_NAME_LEN);
            ble_cfg.names[i][MAX_BLE_NAME_LEN] = '\0';
        }
        ble_multi_adv_init(&ble_cfg);
    }
}

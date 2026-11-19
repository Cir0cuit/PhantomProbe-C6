#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "beacon_generator.h"
#include "config_parser.h"
#include "rgb_led.h"
#include "ble_multi_adv.h"
static const char *TAG = "probe_main";

#define MAC_FMT "%02X:%02X:%02X:%02X:%02X:%02X"
#define MAC_ARG(m) (m)[0], (m)[1], (m)[2], (m)[3], (m)[4], (m)[5]

#define TELEMETRY_PERIOD_MS 5000
#define STATION_ACTIVE_WINDOW_MS 300000
// One log line of about 120 bytes needs 10 ms on the wire at 115200 baud. Sleeping that long
// after each line lets the UART FIFO drain instead of busy-waiting on it.
#define LOG_PACING_MS 10

// Probe responder limits: a wildcard probe costs one response per virtual network.
#define RESP_BUCKET_CAPACITY (2 * MAX_VIRTUAL_NETWORKS)
#define RESP_BUCKET_REFILL_PER_S 64
#define WILDCARD_CLIENT_GAP_MS 250
#define SNIFF_REPEAT_WINDOW_MS 10000

static probe_config_t s_config;
static virtual_network_t s_networks[MAX_VIRTUAL_NETWORKS];
static uint8_t s_frame_buffer[MAX_BEACON_FRAME_LEN];
static uint32_t s_beacon_period_ms;

// Telemetry counters. Each has a single writer task; 32-bit loads are atomic on RV32.
static uint32_t s_beacons_queued = 0;
static uint32_t s_beacon_tx_fail = 0;
static uint32_t s_probe_responses_sent = 0;
static uint32_t s_probe_resp_tx_fail = 0;
static uint32_t s_probe_resp_limited = 0;
static uint32_t s_deauth_frames_intercepted = 0;
static uint32_t s_deauth_frames_honeypot = 0;
static uint32_t s_auth_attempts_intercepted = 0;
static uint32_t s_log_lines_skipped = 0;

#define CLIENT_CACHE_SIZE 64

typedef struct {
    uint8_t mac[6];
    uint32_t last_seen_ms;
    uint32_t last_resp_ms;
} client_entry_t;

static client_entry_t s_client_cache[CLIENT_CACHE_SIZE];
static size_t s_client_cache_count = 0;
static uint32_t s_station_macs_seen = 0;

// ---------------------------------------------------------------------------
// Deferred logging: the promiscuous callback runs in the Wi-Fi task, where a
// console write would delay frame handling. It only queues events; the
// telemetry task formats and prints them.
// ---------------------------------------------------------------------------

typedef enum {
    EVT_NEW_STATION,
    EVT_WILDCARD_SCAN,
    EVT_PROBE_HIT,
    EVT_PROBE_SNIFF,
    EVT_AUTH,
    EVT_DEAUTH,
} probe_evt_kind_t;

#define EVT_F_ANSWERED     0x01
#define EVT_F_RATE_LIMITED 0x02
#define EVT_F_REASON_VALID 0x04

typedef struct {
    uint8_t kind;
    uint8_t subtype;
    uint8_t flags;
    int8_t rssi;
    int8_t net_idx;
    uint8_t ssid_len;
    uint16_t reason;
    uint32_t count;
    uint8_t addr1[6];
    uint8_t addr2[6];
    uint8_t addr3[6];
    uint8_t ssid[MAX_SSID_LEN];
} probe_evt_t;

typedef enum {
    GATE_NEW_STATION,
    GATE_WILDCARD,
    GATE_PROBE_HIT,
    GATE_PROBE_SNIFF,
    GATE_AUTH,
    GATE_DEAUTH_HONEYPOT,
    GATE_DEAUTH_AMBIENT,
    GATE_COUNT
} log_gate_t;

static const uint16_t s_gate_gap_ms[GATE_COUNT] = { 100, 100, 100, 100, 50, 50, 50 };
// Lines that may be logged back to back. A client probes for its saved networks in one burst.
static const uint8_t s_gate_burst[GATE_COUNT] = { 1, 1, 1, 8, 1, 1, 1 };
static uint8_t s_gate_credit[GATE_COUNT];
static uint32_t s_gate_last_ms[GATE_COUNT];
static QueueHandle_t s_evt_queue = NULL;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Per-class rate limit for log lines, so one noisy frame type cannot hide the others.
// One line is earned per gap, and up to s_gate_burst lines can be saved up.
static bool log_gate(log_gate_t gate, uint32_t now)
{
    uint32_t earned = (now - s_gate_last_ms[gate]) / s_gate_gap_ms[gate];
    if (earned > 0) {
        if (s_gate_credit[gate] + earned >= s_gate_burst[gate]) {
            s_gate_credit[gate] = s_gate_burst[gate];
            s_gate_last_ms[gate] = now;
        } else {
            s_gate_credit[gate] += (uint8_t)earned;
            s_gate_last_ms[gate] += earned * s_gate_gap_ms[gate];
        }
    }
    if (s_gate_credit[gate] == 0) {
        s_log_lines_skipped++;
        return false;
    }
    s_gate_credit[gate]--;
    return true;
}

static bool post_event(const probe_evt_t *evt)
{
    if (!s_evt_queue || xQueueSend(s_evt_queue, evt, 0) != pdTRUE) {
        s_log_lines_skipped++;
        return false;
    }
    return true;
}

static int find_network_by_bssid(const uint8_t *addr)
{
    for (size_t i = 0; i < s_config.network_count; i++) {
        if (memcmp(addr, s_networks[i].bssid, 6) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool is_broadcast(const uint8_t *addr)
{
    static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    return memcmp(addr, bcast, 6) == 0;
}

static client_entry_t *client_cache_touch(const uint8_t *mac, int8_t rssi, uint32_t now)
{
    for (size_t i = 0; i < s_client_cache_count; i++) {
        if (memcmp(s_client_cache[i].mac, mac, 6) == 0) {
            s_client_cache[i].last_seen_ms = now;
            return &s_client_cache[i];
        }
    }

    client_entry_t *entry;
    if (s_client_cache_count < CLIENT_CACHE_SIZE) {
        entry = &s_client_cache[s_client_cache_count];
    } else {
        // Evict the least recently seen entry
        entry = &s_client_cache[0];
        for (size_t i = 1; i < CLIENT_CACHE_SIZE; i++) {
            if (now - s_client_cache[i].last_seen_ms > now - entry->last_seen_ms) {
                entry = &s_client_cache[i];
            }
        }
    }
    memcpy(entry->mac, mac, 6);
    entry->last_seen_ms = now;
    entry->last_resp_ms = now - WILDCARD_CLIENT_GAP_MS;
    if (s_client_cache_count < CLIENT_CACHE_SIZE) {
        s_client_cache_count++;
    }
    s_station_macs_seen++;

    if (log_gate(GATE_NEW_STATION, now)) {
        probe_evt_t evt = { .kind = EVT_NEW_STATION, .rssi = rssi, .count = s_station_macs_seen };
        memcpy(evt.addr2, mac, 6);
        post_event(&evt);
    }
    return entry;
}

// Global budget for probe responses, so a probe flood cannot crowd the beacons out of the TX queue.
static bool resp_bucket_take(uint32_t now, uint32_t tokens)
{
    static uint32_t s_milli_tokens = RESP_BUCKET_CAPACITY * 1000;
    static uint32_t s_last_ms = 0;

    uint64_t refilled = (uint64_t)s_milli_tokens + (uint64_t)(now - s_last_ms) * RESP_BUCKET_REFILL_PER_S;
    s_last_ms = now;
    s_milli_tokens = (refilled > RESP_BUCKET_CAPACITY * 1000) ? RESP_BUCKET_CAPACITY * 1000 : (uint32_t)refilled;

    if (s_milli_tokens < tokens * 1000) {
        return false;
    }
    s_milli_tokens -= tokens * 1000;
    return true;
}

static uint32_t send_probe_response(size_t net_idx, const uint8_t *client_mac)
{
    uint8_t resp_buf[MAX_BEACON_FRAME_LEN];
    int rlen = beacon_generator_build_probe_response(&s_networks[net_idx], client_mac, resp_buf, sizeof(resp_buf));
    if (rlen > 0 && esp_wifi_80211_tx(WIFI_IF_AP, resp_buf, rlen, false) == ESP_OK) {
        s_probe_responses_sent++;
        return 1;
    }
    s_probe_resp_tx_fail++;
    return 0;
}

// A probe request is meant for a virtual AP when both its destination and its BSSID field
// are either broadcast or that AP's BSSID.
static bool probe_addressed_to(const uint8_t *addr1, const uint8_t *addr3, const virtual_network_t *net)
{
    return (is_broadcast(addr1) || memcmp(addr1, net->bssid, 6) == 0) &&
           (is_broadcast(addr3) || memcmp(addr3, net->bssid, 6) == 0);
}

// Remembers recently logged (client, SSID) pairs so a client repeating the same directed probe is
// logged once. Returns NULL for a repeat or when the log gate is closed, otherwise the table slot
// that now holds the pair.
static uint32_t *sniff_remember(const uint8_t *mac, const uint8_t *ssid, uint8_t ssid_len, uint32_t now)
{
    static struct {
        uint32_t hash;
        uint32_t seen_ms;
    } s_recent[16];

    uint32_t hash = 2166136261u; // FNV-1a
    for (int i = 0; i < 6; i++) hash = (hash ^ mac[i]) * 16777619u;
    for (int i = 0; i < ssid_len; i++) hash = (hash ^ ssid[i]) * 16777619u;
    if (hash == 0) hash = 1;

    size_t oldest = 0;
    for (size_t i = 0; i < sizeof(s_recent) / sizeof(s_recent[0]); i++) {
        if (s_recent[i].hash == hash && now - s_recent[i].seen_ms < SNIFF_REPEAT_WINDOW_MS) {
            return NULL;
        }
        if (now - s_recent[i].seen_ms > now - s_recent[oldest].seen_ms) {
            oldest = i;
        }
    }
    if (!log_gate(GATE_PROBE_SNIFF, now)) {
        return NULL;
    }
    s_recent[oldest].hash = hash;
    s_recent[oldest].seen_ms = now;
    return &s_recent[oldest].hash;
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

// Association / Reassociation Request (0x00, 0x20) or Authentication (0xB0)
static void handle_auth_assoc(const uint8_t *payload, uint8_t subtype, int8_t rssi, uint32_t now)
{
    const uint8_t *dest_bssid = &payload[4];   // Address 1 (DA / BSSID)
    const uint8_t *client_mac = &payload[10];  // Address 2 (SA)
    const uint8_t *addr3 = &payload[16];

    // Authentication frames also travel from the AP to the client; an AP is not a station.
    if (memcmp(client_mac, addr3, 6) == 0) {
        return;
    }
    client_cache_touch(client_mac, rssi, now);

    int net_idx = find_network_by_bssid(dest_bssid);
    if (net_idx < 0) {
        return;
    }
    s_auth_attempts_intercepted++;
    rgb_led_trigger_event(LED_EVENT_WIFI_AUTH_ASSOC);

    if (log_gate(GATE_AUTH, now)) {
        probe_evt_t evt = { .kind = EVT_AUTH, .subtype = subtype, .rssi = rssi, .net_idx = (int8_t)net_idx };
        memcpy(evt.addr1, dest_bssid, 6);
        memcpy(evt.addr2, client_mac, 6);
        post_event(&evt);
    }
}

// Deauthentication (0xC0) or Disassociation (0xA0)
static void handle_deauth(const uint8_t *payload, int len, int body, uint8_t subtype, int8_t rssi, uint32_t now)
{
    const uint8_t *addr1 = &payload[4];  // Destination / Target Address
    const uint8_t *addr2 = &payload[10]; // Source / Transmitter Address
    const uint8_t *addr3 = &payload[16]; // BSSID

    int net_idx = find_network_by_bssid(addr2);
    bool from_ap = (net_idx >= 0) || memcmp(addr2, addr3, 6) == 0;
    if (net_idx < 0) net_idx = find_network_by_bssid(addr1);
    if (net_idx < 0) net_idx = find_network_by_bssid(addr3);

    if (!from_ap) {
        client_cache_touch(addr2, rssi, now);
    }

    s_deauth_frames_intercepted++;
    if (net_idx >= 0) {
        s_deauth_frames_honeypot++;
        rgb_led_trigger_event(LED_EVENT_WIFI_DEAUTH);
    }

    if (log_gate(net_idx >= 0 ? GATE_DEAUTH_HONEYPOT : GATE_DEAUTH_AMBIENT, now)) {
        probe_evt_t evt = { .kind = EVT_DEAUTH, .subtype = subtype, .rssi = rssi, .net_idx = (int8_t)net_idx };
        memcpy(evt.addr1, addr1, 6);
        memcpy(evt.addr2, addr2, 6);
        memcpy(evt.addr3, addr3, 6);
        // With management frame protection the body is encrypted and holds no readable reason code.
        if (!(payload[1] & 0x40) && len >= body + 2) {
            evt.reason = (uint16_t)(payload[body] | ((uint16_t)payload[body + 1] << 8));
            evt.flags |= EVT_F_REASON_VALID;
        }
        post_event(&evt);
    }
}

// Probe Request (0x40)
static void handle_probe_request(const uint8_t *payload, int len, int body, int8_t rssi, uint32_t now)
{
    // The SSID element is always the first element of a probe request
    if ((payload[1] & 0x40) || len < body + 2 || payload[body] != 0) {
        return;
    }
    uint8_t ssid_len = payload[body + 1];
    if (ssid_len > MAX_SSID_LEN || body + 2 + ssid_len > len) {
        return;
    }
    const uint8_t *ssid = &payload[body + 2];
    const uint8_t *addr1 = &payload[4];
    const uint8_t *client_mac = &payload[10]; // Source Address (SA)
    const uint8_t *addr3 = &payload[16];

    client_entry_t *client = client_cache_touch(client_mac, rssi, now);

    // Case A: Wildcard Probe Request (SSID tag length == 0)
    // Client device is querying for any and all in-range access points
    if (ssid_len == 0) {
        rgb_led_trigger_event(LED_EVENT_WIFI_WILDCARD_SCAN);

        probe_evt_t evt = { .kind = EVT_WILDCARD_SCAN, .rssi = rssi };
        uint32_t wanted = 0;
        for (size_t i = 0; i < s_config.network_count; i++) {
            if (probe_addressed_to(addr1, addr3, &s_networks[i])) wanted++;
        }
        if (s_config.enable_probe_responder && wanted > 0) {
            if (now - client->last_resp_ms < WILDCARD_CLIENT_GAP_MS || !resp_bucket_take(now, wanted)) {
                s_probe_resp_limited++;
                evt.flags |= EVT_F_RATE_LIMITED;
            } else {
                client->last_resp_ms = now;
                for (size_t i = 0; i < s_config.network_count; i++) {
                    if (probe_addressed_to(addr1, addr3, &s_networks[i])) {
                        evt.count += send_probe_response(i, client_mac);
                    }
                }
            }
        }
        if (log_gate(GATE_WILDCARD, now)) {
            memcpy(evt.addr2, client_mac, 6);
            post_event(&evt);
        }
        return;
    }

    // Case B: Directed Probe Request
    // Client device is explicitly hunting for a specific known SSID
    for (size_t i = 0; i < s_config.network_count; i++) {
        if (strlen(s_networks[i].ssid) != ssid_len || memcmp(s_networks[i].ssid, ssid, ssid_len) != 0) {
            continue;
        }
        probe_evt_t evt = { .kind = EVT_PROBE_HIT, .rssi = rssi, .net_idx = (int8_t)i };
        if (s_config.enable_probe_responder && probe_addressed_to(addr1, addr3, &s_networks[i])) {
            if (!resp_bucket_take(now, 1)) {
                s_probe_resp_limited++;
                evt.flags |= EVT_F_RATE_LIMITED;
            } else if (send_probe_response(i, client_mac)) {
                evt.flags |= EVT_F_ANSWERED;
                rgb_led_trigger_event(LED_EVENT_PROBE_RESPONSE);
            }
        }
        if (log_gate(GATE_PROBE_HIT, now)) {
            memcpy(evt.addr2, client_mac, 6);
            post_event(&evt);
        }
        return;
    }

    // Case C: Client is probing for an external/foreign SSID (Preferred Network List leak)
    uint32_t *slot = sniff_remember(client_mac, ssid, ssid_len, now);
    if (!slot) {
        return;
    }
    probe_evt_t evt = { .kind = EVT_PROBE_SNIFF, .rssi = rssi, .ssid_len = ssid_len };
    memcpy(evt.addr2, client_mac, 6);
    memcpy(evt.ssid, ssid, ssid_len);
    if (!post_event(&evt)) {
        *slot = 0; // not logged this time, so let the next occurrence through
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
 *
 * Runs in the Wi-Fi task: no console output here, only counters, TX requests and queued events.
 */
static void wifi_promiscuous_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) {
        return;
    }

    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    if (pkt->rx_ctrl.rx_state != 0) {
        return; // Reception error
    }
    const uint8_t *payload = pkt->payload;
    int len = (int)pkt->rx_ctrl.sig_len - 4; // sig_len includes the 4-byte FCS

    if (len < 24) {
        return;
    }

    uint8_t fc0 = payload[0];
    if ((fc0 & 0x0C) != 0x00) {
        return; // Non-management frame
    }
    if (payload[10] & 0x01) {
        return; // A group address is never a valid transmitter
    }

    uint8_t subtype = fc0 & 0xF0;
    int body = 24 + ((payload[1] & 0x80) ? 4 : 0); // HT Control field present
    int8_t rssi = pkt->rx_ctrl.rssi;
    uint32_t now = now_ms();

    if (subtype == 0x00 || subtype == 0x20 || subtype == 0xB0) {
        handle_auth_assoc(payload, subtype, rssi, now);
    } else if (subtype == 0xC0 || subtype == 0xA0) {
        handle_deauth(payload, len, body, subtype, rssi, now);
    } else if (subtype == 0x40) {
        handle_probe_request(payload, len, body, rssi, now);
    }
}

// SSIDs received over the air are untrusted bytes. Everything outside printable ASCII is
// written as \xNN so a crafted SSID cannot forge log lines or drive the terminal.
static const char *printable_ssid(const uint8_t *ssid, uint8_t ssid_len)
{
    static char out[4 * MAX_SSID_LEN + 1];
    size_t pos = 0;
    for (uint8_t i = 0; i < ssid_len && i < MAX_SSID_LEN; i++) {
        uint8_t c = ssid[i];
        if (c >= 0x20 && c <= 0x7E && c != '\'' && c != '\\') {
            out[pos++] = (char)c;
        } else {
            pos += (size_t)snprintf(&out[pos], 5, "\\x%02X", c);
        }
    }
    out[pos] = '\0';
    return out;
}

static void print_event(const probe_evt_t *evt)
{
    switch (evt->kind) {
    case EVT_NEW_STATION:
        ESP_LOGI(TAG, "[NEW STATION #%lu] Discovered client " MAC_FMT " | RSSI: %d dBm",
                 (unsigned long)evt->count, MAC_ARG(evt->addr2), evt->rssi);
        break;

    case EVT_WILDCARD_SCAN:
        if (evt->count > 0) {
            ESP_LOGI(TAG, "[WILDCARD SCAN] Client " MAC_FMT " sweeping RF space | RSSI: %d dBm | Replying with %lu honeypots",
                     MAC_ARG(evt->addr2), evt->rssi, (unsigned long)evt->count);
        } else {
            ESP_LOGI(TAG, "[WILDCARD SCAN] Client " MAC_FMT " sweeping RF space | RSSI: %d dBm | Not answered (%s)",
                     MAC_ARG(evt->addr2), evt->rssi,
                     (evt->flags & EVT_F_RATE_LIMITED) ? "rate limited" :
                     !s_config.enable_probe_responder ? "responder off" : "addressed to another AP");
        }
        break;

    case EVT_PROBE_HIT:
        ESP_LOGI(TAG, "[PROBE HIT] Client " MAC_FMT " queried '%s' -> %s | RSSI: %d dBm",
                 MAC_ARG(evt->addr2), s_networks[evt->net_idx].ssid,
                 (evt->flags & EVT_F_ANSWERED) ? "Injected response" :
                 !s_config.enable_probe_responder ? "Not answered (responder off)" :
                 (evt->flags & EVT_F_RATE_LIMITED) ? "Not answered (rate limited)" : "Not answered",
                 evt->rssi);
        break;

    case EVT_PROBE_SNIFF:
        ESP_LOGI(TAG, "[PROBE SNIFF] Client " MAC_FMT " probing foreign SSID '%s' | RSSI: %d dBm",
                 MAC_ARG(evt->addr2), printable_ssid(evt->ssid, evt->ssid_len), evt->rssi);
        break;

    case EVT_AUTH:
        ESP_LOGI(TAG, "[AUTH INTERCEPT] Client " MAC_FMT " sent %s to '%s' (BSSID: " MAC_FMT ") | RSSI: %d dBm",
                 MAC_ARG(evt->addr2),
                 (evt->subtype == 0xB0) ? "Auth" : (evt->subtype == 0x00) ? "Assoc" : "Reassoc",
                 s_networks[evt->net_idx].ssid, MAC_ARG(evt->addr1), evt->rssi);
        break;

    case EVT_DEAUTH: {
        const uint8_t *bssid = (evt->net_idx >= 0) ? s_networks[evt->net_idx].bssid : evt->addr3;
        char reason[48];
        if (evt->flags & EVT_F_REASON_VALID) {
            snprintf(reason, sizeof(reason), "%u (%s)", evt->reason, wifi_deauth_reason_str(evt->reason));
        } else {
            snprintf(reason, sizeof(reason), "n/a (protected or truncated frame)");
        }
        ESP_LOGW(TAG, "[DEAUTH ALERT] %s Frame: Target=" MAC_FMT " | Source=" MAC_FMT " | BSSID=" MAC_FMT " (%s) | Reason=%s | RSSI: %d dBm",
                 (evt->subtype == 0xC0) ? "Deauth" : "Disassoc",
                 MAC_ARG(evt->addr1), MAC_ARG(evt->addr2), MAC_ARG(bssid),
                 (evt->net_idx >= 0) ? s_networks[evt->net_idx].ssid : "External-BSSID",
                 reason, evt->rssi);
        break;
    }

    default:
        break;
    }
}

static void print_telemetry(void)
{
    int64_t uptime_sec = esp_timer_get_time() / 1000000;
    uint32_t hours = (uint32_t)(uptime_sec / 3600);
    uint32_t minutes = (uint32_t)((uptime_sec % 3600) / 60);
    uint32_t seconds = (uint32_t)(uptime_sec % 60);

    uint32_t now = now_ms();
    uint32_t active = 0;
    size_t cached = s_client_cache_count;
    for (size_t i = 0; i < cached; i++) {
        if (now - s_client_cache[i].last_seen_ms + 1000u < STATION_ACTIVE_WINDOW_MS + 1000u) active++;
    }

    ESP_LOGI(TAG, "=== RF Probe Telemetry [Uptime: %02lu:%02lu:%02lu | Stations: %lu active, %lu MACs seen | Frames: %lu | Probe Resp: %lu | Deauths: %lu (%lu honeypot) | Auths: %lu | Free Heap: %lu B] ===",
             (unsigned long)hours, (unsigned long)minutes, (unsigned long)seconds,
             (unsigned long)active,
             (unsigned long)s_station_macs_seen,
             (unsigned long)s_beacons_queued,
             (unsigned long)s_probe_responses_sent,
             (unsigned long)s_deauth_frames_intercepted,
             (unsigned long)s_deauth_frames_honeypot,
             (unsigned long)s_auth_attempts_intercepted,
             (unsigned long)esp_get_free_heap_size());
    vTaskDelay(pdMS_TO_TICKS(2 * LOG_PACING_MS));

    // Shown only when one of the counters moved since the last summary
    static uint32_t s_limits_reported = 0;
    uint32_t limits_total = s_beacon_tx_fail + s_probe_resp_tx_fail + s_probe_resp_limited + s_log_lines_skipped;
    if (limits_total != s_limits_reported) {
        s_limits_reported = limits_total;
        ESP_LOGI(TAG, "  Limits: %lu beacons and %lu probe responses refused by the TX queue | %lu probes not answered (rate limit) | %lu log lines skipped",
                 (unsigned long)s_beacon_tx_fail,
                 (unsigned long)s_probe_resp_tx_fail,
                 (unsigned long)s_probe_resp_limited,
                 (unsigned long)s_log_lines_skipped);
        vTaskDelay(pdMS_TO_TICKS(2 * LOG_PACING_MS));
    }

    for (size_t i = 0; i < s_config.network_count; i++) {
        ESP_LOGI(TAG, "  BSSID: " MAC_FMT " | CH: %u | Sent: %-6lu | SSID: %s",
                 MAC_ARG(s_networks[i].bssid),
                 s_networks[i].channel,
                 (unsigned long)s_networks[i].beacons_sent,
                 s_networks[i].ssid);
        vTaskDelay(pdMS_TO_TICKS(LOG_PACING_MS));
    }
}

/**
 * @brief Low-priority task that owns all Wi-Fi console output: queued sniffer events
 *        and the periodic RF telemetry summary.
 */
static void telemetry_task(void *pvParameters)
{
    TickType_t next_stats = xTaskGetTickCount() + pdMS_TO_TICKS(TELEMETRY_PERIOD_MS);

    while (1) {
        int32_t wait = (int32_t)(next_stats - xTaskGetTickCount());
        probe_evt_t evt;
        if (xQueueReceive(s_evt_queue, &evt, wait > 0 ? (TickType_t)wait : 0) == pdTRUE) {
            print_event(&evt);
            vTaskDelay(pdMS_TO_TICKS(LOG_PACING_MS));
        }
        if ((int32_t)(xTaskGetTickCount() - next_stats) >= 0) {
            print_telemetry();
            next_stats += pdMS_TO_TICKS(TELEMETRY_PERIOD_MS);
            if ((int32_t)(xTaskGetTickCount() - next_stats) >= 0) {
                next_stats = xTaskGetTickCount() + pdMS_TO_TICKS(TELEMETRY_PERIOD_MS); // fell behind
            }
        }
    }
}

/**
 * @brief Brings up the Wi-Fi driver and settles the regulatory domain and channel,
 *        without starting the radio.
 */
static void wifi_init_radio(void)
{
    ESP_LOGI(TAG, "Initializing Wi-Fi driver and default event loop...");
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    if (s_config.country[0] != '\0') {
        esp_err_t err = esp_wifi_set_country_code(s_config.country, false);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Country code '%s' rejected (%s); keeping the driver default", s_config.country, esp_err_to_name(err));
        }
    }

    // A channel outside the regulatory domain makes esp_wifi_set_config fail, so settle it first.
    wifi_country_t country;
    if (esp_wifi_get_country(&country) == ESP_OK) {
        uint8_t last = (uint8_t)(country.schan + country.nchan - 1);
        if (s_config.channel < country.schan || s_config.channel > last) {
            ESP_LOGE(TAG, "Channel %u is not allowed in regulatory domain '%.2s' (%u-%u); using channel %u. Set 'country' in [wifi] to unlock 12-13.",
                     s_config.channel, country.cc, country.schan, last, country.schan);
            s_config.channel = country.schan;
        }
    }
}

/**
 * @brief Starts the radio with promiscuous management frame RX.
 *
 * Raw 802.11 TX needs a started interface, so a carrier SoftAP is brought up. It is hidden,
 * has no IP stack attached and uses a random WPA2 passphrase, so nothing can join it.
 */
static void wifi_start_probe(void)
{
    wifi_config_t ap_config = {
        .ap = {
            .ssid = "PhantomProbe-C6-Internal",
            .ssid_len = strlen("PhantomProbe-C6-Internal"),
            .channel = s_config.channel,
            .max_connection = 1,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .ssid_hidden = 1,
        },
    };
    snprintf((char *)ap_config.ap.password, sizeof(ap_config.ap.password), "%08lx%08lx%08lx%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random(),
             (unsigned long)esp_random(), (unsigned long)esp_random());

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(s_config.channel, WIFI_SECOND_CHAN_NONE));

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_promiscuous_rx_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_LOGI(TAG, "Promiscuous 802.11 management frame analyzer: ACTIVE | Probe responder: %s",
             s_config.enable_probe_responder ? "ACTIVE" : "DISABLED");

    ESP_LOGI(TAG, "RF PHY transmitter configured on 2.4 GHz Channel %u with raw 802.11 TX injection.", s_config.channel);
}

/**
 * @brief Continuous task responsible for synthesizing and transmitting synthetic 802.11 beacon frames.
 */
static void beacon_broadcast_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting Wi-Fi beacon injection loop (%u virtual BSSIDs, period=%lu ms, pause=%lu ms, gap=%lu ms)...",
             (unsigned int)s_config.network_count,
             (unsigned long)s_beacon_period_ms,
             (unsigned long)s_config.burst_interval_ms,
             (unsigned long)s_config.burst_gap_ms);

    // Subtle telemetry blip roughly every 1.5 s to indicate active RF beaconing
    const uint32_t led_every = (1500 / s_beacon_period_ms) ? (1500 / s_beacon_period_ms) : 1;
    uint32_t burst_loop_counter = 0;
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        if (++burst_loop_counter % led_every == 0) {
            rgb_led_trigger_event(LED_EVENT_BEACON_BURST);
        }

        // Transmit all virtual BSSID beacons in rapid succession
        for (size_t i = 0; i < s_config.network_count; i++) {
            int len = beacon_generator_build_frame(&s_networks[i], s_frame_buffer, sizeof(s_frame_buffer));
            if (len > 0 && esp_wifi_80211_tx(WIFI_IF_AP, s_frame_buffer, len, false) == ESP_OK) {
                s_networks[i].beacons_sent++;
                s_beacons_queued++;
            } else {
                s_beacon_tx_fail++;
            }
            if (s_config.burst_gap_ms > 0 && i + 1 < s_config.network_count) {
                vTaskDelay(pdMS_TO_TICKS(s_config.burst_gap_ms));
            }
        }

        // Sleep out the rest of the period, so the rate matches the advertised beacon interval
        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(s_beacon_period_ms));
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

    // 2. Initialize onboard WS2812 RGB LED (GPIO8) visual telemetry engine.
    //    Also runs when the LED is disabled, to switch off a LED left lit by an earlier firmware.
    rgb_led_init(s_config.led_brightness, s_config.enable_led);

    // 3. Initialize Wi-Fi Subsystem & Multi-BSSID Frame Injection Engine.
    //    The virtual networks must exist before the sniffer callback can run.
    wifi_init_radio();

    s_beacon_period_ms = (uint32_t)s_config.network_count * s_config.burst_gap_ms + s_config.burst_interval_ms;
    // A beacon takes about 1.5 ms of airtime at the 1 Mbps raw TX rate
    uint32_t min_period_ms = ((uint32_t)s_config.network_count * 3 + 1) / 2;
    if (s_beacon_period_ms < min_period_ms) {
        ESP_LOGW(TAG, "Beacon period raised from %lu to %lu ms: %u beacons do not fit in less airtime",
                 (unsigned long)s_beacon_period_ms, (unsigned long)min_period_ms, (unsigned int)s_config.network_count);
        s_beacon_period_ms = min_period_ms;
    }
    uint32_t interval_tu = (s_beacon_period_ms * 1000 + 512) / 1024;
    if (interval_tu < 1) interval_tu = 1;
    beacon_generator_init_networks(s_networks, s_config.ssids, s_config.network_count, s_config.channel, (uint16_t)interval_tu);

    s_evt_queue = xQueueCreate(24, sizeof(probe_evt_t));
    if (!s_evt_queue || xTaskCreate(telemetry_task, "probe_log", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start the telemetry task; sniffer events will not be printed");
    }

    wifi_start_probe();

    xTaskCreate(
        beacon_broadcast_task,
        "probe_wifi_tx",
        4096,
        NULL,
        5,
        NULL
    );

    // 4. Initialize Multi-Identity Bluetooth Low Energy Peripheral Reconnaissance Engine
    if (s_config.enable_ble && s_config.ble_name_count > 0) {
        static ble_config_t ble_cfg;
        ble_cfg.name_count = s_config.ble_name_count;
        ble_cfg.rotation_interval_ms = s_config.ble_adv_interval_ms;
        ble_cfg.max_connection_ms = s_config.ble_max_connection_s * 1000;
        ble_cfg.accessory_type = s_config.ble_accessory_type;
        for (size_t i = 0; i < s_config.ble_name_count; i++) {
            strncpy(ble_cfg.names[i], s_config.ble_names[i], MAX_BLE_NAME_LEN);
            ble_cfg.names[i][MAX_BLE_NAME_LEN] = '\0';
        }
        ble_multi_adv_init(&ble_cfg);
    }
}

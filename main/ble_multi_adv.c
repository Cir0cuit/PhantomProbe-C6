#include "ble_multi_adv.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "os/os_mbuf.h"
#include "esp_timer.h"
#include "rgb_led.h"

static const char *TAG = "probe_ble";

static ble_config_t s_ble_cfg;
static uint8_t s_base_mac[6];
static bool s_is_connected = false;

/**
 * @brief Synthetic GATT characteristic access handler.
 *        Returns empty responses to client reads and triggers optical/UART telemetry.
 */
static int gatt_svr_chr_access_dummy(uint16_t conn_handle, uint16_t attr_handle,
                                     struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t zero = 0;
        os_mbuf_append(ctxt->om, &zero, sizeof(zero));
        rgb_led_trigger_event(LED_EVENT_BLE_GATT_READ);
    }

    static int64_t s_last_gatt_log_us = 0;
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_last_gatt_log_us >= 100000) { // Max 10/sec
        s_last_gatt_log_us = now_us;
        uint16_t uuid16 = 0;
        if (ctxt->chr && ctxt->chr->uuid && ctxt->chr->uuid->type == BLE_UUID_TYPE_16) {
            uuid16 = BLE_UUID16(ctxt->chr->uuid)->value;
        }
        ESP_LOGI(TAG, "[BLE GATT %s] Client handle=%u attr_handle=%u (UUID=0x%04X)",
                 (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) ? "WRITE" : "READ",
                 conn_handle, attr_handle, uuid16);
    }
    return 0;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        // Device Information Service (0x180A)
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180A),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(0x2A29), // Manufacturer Name
                .flags = BLE_GATT_CHR_F_READ,
                .access_cb = gatt_svr_chr_access_dummy,
            },
            {
                .uuid = BLE_UUID16_DECLARE(0x2A24), // Model Number
                .flags = BLE_GATT_CHR_F_READ,
                .access_cb = gatt_svr_chr_access_dummy,
            },
            { 0 }
        }
    },
    {
        // Human Interface Device (HID) Service (0x1812)
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(0x2A4E), // Protocol Mode
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .access_cb = gatt_svr_chr_access_dummy,
            },
            {
                .uuid = BLE_UUID16_DECLARE(0x2A4A), // HID Information
                .flags = BLE_GATT_CHR_F_READ,
                .access_cb = gatt_svr_chr_access_dummy,
            },
            { 0 }
        }
    },
    {
        // Heart Rate Service (0x180D)
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180D),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(0x2A37), // Heart Rate Measurement
                .flags = BLE_GATT_CHR_F_READ,
                .access_cb = gatt_svr_chr_access_dummy,
            },
            { 0 }
        }
    },
    {
        // Health Thermometer (0x1809)
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1809),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(0x2A1C), // Temperature Measurement
                .flags = BLE_GATT_CHR_F_READ,
                .access_cb = gatt_svr_chr_access_dummy,
            },
            { 0 }
        }
    },
    {
        // Current Time Service (0x1805)
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1805),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(0x2A2B), // Current Time
                .flags = BLE_GATT_CHR_F_READ,
                .access_cb = gatt_svr_chr_access_dummy,
            },
            { 0 }
        }
    },
    {
        // Cycling Speed and Cadence (0x1816)
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1816),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(0x2A5B), // CSC Measurement
                .flags = BLE_GATT_CHR_F_READ,
                .access_cb = gatt_svr_chr_access_dummy,
            },
            { 0 }
        }
    },
    { 0 }
};

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        struct ble_gap_conn_desc desc;
        int rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
        if (rc == 0) {
            ESP_LOGI(TAG, "[BLE CONNECT] Client %02X:%02X:%02X:%02X:%02X:%02X paired on handle %u (itvl=%u, latency=%u, timeout=%u)",
                     desc.peer_id_addr.val[5], desc.peer_id_addr.val[4], desc.peer_id_addr.val[3],
                     desc.peer_id_addr.val[2], desc.peer_id_addr.val[1], desc.peer_id_addr.val[0],
                     event->connect.conn_handle, desc.conn_itvl, desc.conn_latency, desc.supervision_timeout);
        } else {
            ESP_LOGI(TAG, "[BLE CONNECT] Client connected on handle %u (status=%d)",
                     event->connect.conn_handle, event->connect.status);
        }
        s_is_connected = (event->connect.status == 0);
        if (s_is_connected) {
            rgb_led_trigger_event(LED_EVENT_BLE_CONNECT);
            rgb_led_set_connected(true);
        }
        break;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "[BLE DISCONNECT] Client disconnected from handle %u (reason=%d)",
                 event->disconnect.conn.conn_handle, event->disconnect.reason);
        s_is_connected = false;
        rgb_led_trigger_event(LED_EVENT_BLE_DISCONNECT);
        rgb_led_set_connected(false);
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "[BLE MTU] Client on handle %u negotiated MTU: %u bytes",
                 event->mtu.conn_handle, event->mtu.value);
        break;

    default:
        break;
    }
    return 0;
}

static void get_accessory_profile(uint8_t mode, size_t index, uint16_t *out_svc_uuid, uint16_t *out_appearance, const char **out_type_name)
{
    uint8_t effective_type = mode;
    if (effective_type == 12) {
        // Auto-Distribute showcase mode: cycle through accessories across identities!
        effective_type = (uint8_t)(1 + (index % 11));
    }

    switch (effective_type) {
    case 1:
        *out_svc_uuid = 0x1812;
        *out_appearance = 0x03C1;
        *out_type_name = "Keyboard";
        break;
    case 2:
        *out_svc_uuid = 0x1812;
        *out_appearance = 0x03C2;
        *out_type_name = "Mouse";
        break;
    case 3:
        *out_svc_uuid = 0x1812;
        *out_appearance = 0x03C4;
        *out_type_name = "Gamepad";
        break;
    case 4:
        *out_svc_uuid = 0x1812;
        *out_appearance = 0x03C3;
        *out_type_name = "Joystick";
        break;
    case 5:
        *out_svc_uuid = 0x1812;
        *out_appearance = 0x03C8;
        *out_type_name = "Remote Control";
        break;
    case 6:
        *out_svc_uuid = 0x1805;
        *out_appearance = 0x00C2;
        *out_type_name = "Smartwatch";
        break;
    case 7:
        *out_svc_uuid = 0x180D;
        *out_appearance = 0x0341;
        *out_type_name = "Heart Rate Monitor";
        break;
    case 8:
        *out_svc_uuid = 0x1809;
        *out_appearance = 0x0300;
        *out_type_name = "Thermometer";
        break;
    case 9:
        *out_svc_uuid = 0x1816;
        *out_appearance = 0x0484;
        *out_type_name = "Cycling Sensor";
        break;
    case 10:
        *out_svc_uuid = 0x1812;
        *out_appearance = 0x03C6;
        *out_type_name = "Digital Stylus";
        break;
    case 11:
        *out_svc_uuid = 0x1812;
        *out_appearance = 0x03C5;
        *out_type_name = "Barcode Scanner";
        break;
    default:
        *out_svc_uuid = 0x180A;
        *out_appearance = 0x0000;
        *out_type_name = "Generic";
        break;
    }
}

static void ble_rotator_task(void *param)
{
    const char *mode_str = (s_ble_cfg.accessory_type == 12) ? "Auto-Distribute Showcase" :
                           (s_ble_cfg.accessory_type == 1)  ? "Keyboard" :
                           (s_ble_cfg.accessory_type == 2)  ? "Mouse" :
                           (s_ble_cfg.accessory_type == 3)  ? "Gamepad" :
                           (s_ble_cfg.accessory_type == 4)  ? "Joystick" :
                           (s_ble_cfg.accessory_type == 5)  ? "Remote Control" :
                           (s_ble_cfg.accessory_type == 6)  ? "Smartwatch" :
                           (s_ble_cfg.accessory_type == 7)  ? "Heart Rate Monitor" :
                           (s_ble_cfg.accessory_type == 8)  ? "Thermometer" :
                           (s_ble_cfg.accessory_type == 9)  ? "Cycling Sensor" :
                           (s_ble_cfg.accessory_type == 10) ? "Digital Stylus" :
                           (s_ble_cfg.accessory_type == 11) ? "Barcode Scanner" : "Generic";

    ESP_LOGI(TAG, "Starting BLE rotator task (%u names, dwell=%lu ms, Mode=%s)...",
             (unsigned int)s_ble_cfg.name_count,
             (unsigned long)s_ble_cfg.rotation_interval_ms,
             mode_str);

    size_t cur_idx = 0;

    while (1) {
        if (!s_is_connected && s_ble_cfg.name_count > 0) {
            // 1. Stop active advertising and wait until controller has fully stopped
            if (ble_gap_adv_active()) {
                int rc = ble_gap_adv_stop();
                if (rc != 0 && rc != BLE_HS_EALREADY) {
                    ESP_LOGW(TAG, "ble_gap_adv_stop: %d", rc);
                }
                int wait_count = 0;
                while (ble_gap_adv_active() && wait_count < 30) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    wait_count++;
                }
            }

            const char *current_name = s_ble_cfg.names[cur_idx];

            // 2. Set GAP Device Name
            ble_svc_gap_device_name_set(current_name);

            // 3. Generate a unique Random Static Address for this virtual Bluetooth device
            // Per BLE Core Spec, the 2 most significant bits of byte 5 must be 11 (0b11xxxxxx)
            uint8_t rnd_addr[6];
            memcpy(rnd_addr, s_base_mac, 6);
            rnd_addr[0] ^= ((uint8_t)(cur_idx * 0x1F + 0x0D));
            rnd_addr[1] ^= ((uint8_t)(cur_idx * 0x0B + 0x03));
            rnd_addr[5] = (rnd_addr[5] & 0x3F) | 0xC0;

            int rc = ble_hs_id_set_rnd(rnd_addr);
            if (rc != 0) {
                ESP_LOGW(TAG, "ble_hs_id_set_rnd failed for [%zu]: %d", cur_idx, rc);
            }

            // 4. Set Advertising Fields
            struct ble_hs_adv_fields fields;
            memset(&fields, 0, sizeof(fields));
            fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

            // Resolve accessory profile (UUID, Appearance, Name)
            uint16_t svc_uuid_val = 0x180A;
            uint16_t appearance_val = 0x0000;
            const char *current_type_name = "Generic";
            get_accessory_profile(s_ble_cfg.accessory_type, cur_idx, &svc_uuid_val, &appearance_val, &current_type_name);

            ble_uuid16_t svc_uuid;
            svc_uuid.u.type = BLE_UUID_TYPE_16;
            svc_uuid.value = svc_uuid_val;

            fields.uuids16 = &svc_uuid;
            fields.num_uuids16 = 1;
            fields.uuids16_is_complete = 1;

            if (appearance_val != 0) {
                fields.appearance = appearance_val;
                fields.appearance_is_present = 1;
            }

            // Truncate name in primary ad packet if needed to stay strictly under 31-byte limit
            size_t name_len = strlen(current_name);
            if (name_len <= 18) {
                fields.name = (uint8_t *)current_name;
                fields.name_len = (uint8_t)name_len;
                fields.name_is_complete = 1;
            } else {
                fields.name = (uint8_t *)current_name;
                fields.name_len = 18;
                fields.name_is_complete = 0; // Shortened Local Name
            }

            rc = ble_gap_adv_set_fields(&fields);
            if (rc != 0) {
                ESP_LOGW(TAG, "ble_gap_adv_set_fields failed: %d", rc);
            }

            // 5. Set Scan Response Fields (FULL complete device name up to 29 chars)
            struct ble_hs_adv_fields rsp_fields;
            memset(&rsp_fields, 0, sizeof(rsp_fields));
            rsp_fields.name = (uint8_t *)current_name;
            rsp_fields.name_len = (uint8_t)name_len;
            rsp_fields.name_is_complete = 1;

            rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
            if (rc != 0) {
                ESP_LOGW(TAG, "ble_gap_adv_rsp_set_fields failed: %d", rc);
            }

            // 6. Start Advertising as an undirected connectable/pairable peripheral
            struct ble_gap_adv_params adv_params;
            memset(&adv_params, 0, sizeof(adv_params));
            adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
            adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
            adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(40); // 40ms fast advertising
            adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(60); // 60ms

            rc = ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL, BLE_HS_FOREVER,
                                   &adv_params, ble_gap_event_cb, NULL);
            if (rc == 0) {
                ESP_LOGI(TAG, "Broadcasting BLE [%zu/%zu] as %s: '%s' (MAC: %02X:%02X:%02X:%02X:%02X:%02X)",
                         cur_idx + 1, s_ble_cfg.name_count, current_type_name, current_name,
                         rnd_addr[5], rnd_addr[4], rnd_addr[3], rnd_addr[2], rnd_addr[1], rnd_addr[0]);
                rgb_led_trigger_event(LED_EVENT_BLE_ADV);
            } else {
                ESP_LOGE(TAG, "ble_gap_adv_start failed for '%s': %d", current_name, rc);
            }

            cur_idx = (cur_idx + 1) % s_ble_cfg.name_count;
        }

        vTaskDelay(pdMS_TO_TICKS(s_ble_cfg.rotation_interval_ms));
    }
}

static void ble_on_sync(void)
{
    ESP_LOGI(TAG, "NimBLE host synced. Launching multi-device advertising...");
    xTaskCreate(ble_rotator_task, "ble_rotator", 4096, NULL, 3, NULL);
}

static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task started.");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

bool ble_multi_adv_init(const ble_config_t *config)
{
    if (!config || config->name_count == 0) {
        ESP_LOGW(TAG, "BLE advertising disabled or 0 names configured.");
        return false;
    }

    memcpy(&s_ble_cfg, config, sizeof(ble_config_t));
    if (s_ble_cfg.rotation_interval_ms < 1000) {
        s_ble_cfg.rotation_interval_ms = 2000;
    }

    // Read base MAC to derive consistent BLE Random Static Addresses
    if (esp_read_mac(s_base_mac, ESP_MAC_BT) != ESP_OK) {
        esp_read_mac(s_base_mac, ESP_MAC_WIFI_STA);
    }

    ESP_LOGI(TAG, "Initializing NimBLE for %u Bluetooth identities (Mode=%u)...",
             (unsigned int)s_ble_cfg.name_count, s_ble_cfg.accessory_type);
    for (size_t i = 0; i < s_ble_cfg.name_count; i++) {
        uint16_t dummy_u = 0, dummy_a = 0;
        const char *tname = "";
        get_accessory_profile(s_ble_cfg.accessory_type, i, &dummy_u, &dummy_a, &tname);
        ESP_LOGI(TAG, "  BLE Identity [%u]: '%s' -> %s", (unsigned int)(i + 1), s_ble_cfg.names[i], tname);
    }

    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NimBLE port: %s", esp_err_to_name(ret));
        return false;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(s_gatt_svcs);
    ble_gatts_add_svcs(s_gatt_svcs);

    ble_hs_cfg.sync_cb = ble_on_sync;

    nimble_port_freertos_init(ble_host_task);
    return true;
}

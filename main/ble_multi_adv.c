#include "ble_multi_adv.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/ble_hs_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "os/os_mbuf.h"
#include "esp_timer.h"
#include "rgb_led.h"

static const char *TAG = "probe_ble";

#define BLE_MAX_LINKS CONFIG_BT_NIMBLE_MAX_CONNECTIONS
#define BLE_MIN_ROTATION_MS 1000

static ble_config_t s_ble_cfg;
static uint8_t s_base_mac[6];
static TaskHandle_t s_rotator_task = NULL;

// NimBLE keeps a pointer to the advertised UUID list, so it must outlive the rotator's stack frame.
static ble_uuid16_t s_adv_svc_uuid = BLE_UUID16_INIT(0x180A);

// True from a successful ble_gap_adv_start until a GAP event reports that advertising ended.
static volatile bool s_adv_owned = false;

static struct {
    volatile bool used;
    uint16_t handle;
    TickType_t since;
} s_links[BLE_MAX_LINKS];

static size_t link_count(void)
{
    size_t n = 0;
    for (size_t i = 0; i < BLE_MAX_LINKS; i++) {
        if (s_links[i].used) n++;
    }
    return n;
}

static void link_add(uint16_t handle)
{
    for (size_t i = 0; i < BLE_MAX_LINKS; i++) {
        if (!s_links[i].used) {
            s_links[i].handle = handle;
            s_links[i].since = xTaskGetTickCount();
            s_links[i].used = true;
            return;
        }
    }
}

static void link_remove(uint16_t handle)
{
    for (size_t i = 0; i < BLE_MAX_LINKS; i++) {
        if (s_links[i].used && s_links[i].handle == handle) {
            s_links[i].used = false;
        }
    }
}

/**
 * @brief Synthetic GATT characteristic access handler.
 *        Answers reads with a well-formed placeholder value for each characteristic
 *        and triggers optical/UART telemetry.
 */
static int gatt_svr_chr_access_dummy(uint16_t conn_handle, uint16_t attr_handle,
                                     struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint16_t uuid16 = 0;
    if (ctxt->chr && ctxt->chr->uuid && ctxt->chr->uuid->type == BLE_UUID_TYPE_16) {
        uuid16 = BLE_UUID16(ctxt->chr->uuid)->value;
    }

    int rc = 0;
    uint8_t written[8];
    uint16_t written_len = 0;
    uint16_t written_total = 0;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        static const uint8_t protocol_mode[] = { 0x01 };                    // Report Protocol
        static const uint8_t hid_info[] = { 0x11, 0x01, 0x00, 0x02 };       // HID 1.11, no country, normally connectable
        static const uint8_t current_time[] = { 0xEA, 0x07, 1, 1, 0, 0, 0, 4, 0, 0 }; // 2026-01-01 00:00:00, Thursday
        static const uint8_t heart_rate[] = { 0x00, 72 };                   // 8-bit value, 72 bpm
        static const uint8_t temperature[] = { 0x00, 0x6E, 0x01, 0x00, 0xFF }; // 36.6 C
        static const uint8_t csc[] = { 0x00 };                              // no wheel or crank data
        static const uint8_t zero[] = { 0x00 };

        const void *value = zero;
        uint16_t value_len = sizeof(zero);
        switch (uuid16) {
        case 0x2A29: value = "Generic"; value_len = 7; break;               // Manufacturer Name
        case 0x2A24: value = "BLE Peripheral"; value_len = 14; break;       // Model Number
        case 0x2A4E: value = protocol_mode; value_len = sizeof(protocol_mode); break;
        case 0x2A4A: value = hid_info; value_len = sizeof(hid_info); break;
        case 0x2A2B: value = current_time; value_len = sizeof(current_time); break;
        case 0x2A37: value = heart_rate; value_len = sizeof(heart_rate); break;
        case 0x2A1C: value = temperature; value_len = sizeof(temperature); break;
        case 0x2A5B: value = csc; value_len = sizeof(csc); break;
        default: break;
        }
        if (os_mbuf_append(ctxt->om, value, value_len) != 0) {
            rc = BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        rgb_led_trigger_event(LED_EVENT_BLE_GATT_READ);
    } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        written_total = OS_MBUF_PKTLEN(ctxt->om);
        ble_hs_mbuf_to_flat(ctxt->om, written, sizeof(written), &written_len);
    }

    static int64_t s_last_gatt_log_us = 0;
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_last_gatt_log_us >= 100000) { // Max 10/sec
        s_last_gatt_log_us = now_us;
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            char hex[2 * sizeof(written) + 1] = "";
            for (uint16_t i = 0; i < written_len; i++) {
                snprintf(&hex[2 * i], 3, "%02X", written[i]);
            }
            ESP_LOGI(TAG, "[BLE GATT WRITE] Client handle=%u attr_handle=%u (UUID=0x%04X) len=%u data=%s%s",
                     conn_handle, attr_handle, uuid16, written_total, hex,
                     written_total > written_len ? "..." : "");
        } else {
            ESP_LOGI(TAG, "[BLE GATT READ] Client handle=%u attr_handle=%u (UUID=0x%04X)",
                     conn_handle, attr_handle, uuid16);
        }
    }
    return rc;
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
        // Advertising stops when a central connects, whether or not the link then comes up
        s_adv_owned = false;
        struct ble_gap_conn_desc desc;
        bool found = (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0);
        // The host can report a non-zero status and still keep the link; only a missing
        // connection is a failed attempt.
        if (event->connect.status != 0 && (event->connect.status == BLE_HS_EAGAIN || !found)) {
            ESP_LOGI(TAG, "[BLE CONNECT] Connection attempt failed (status=%d)", event->connect.status);
            break;
        }
        link_add(event->connect.conn_handle);

        if (found) {
            ESP_LOGI(TAG, "[BLE CONNECT] Client %02X:%02X:%02X:%02X:%02X:%02X (%s address) connected on handle %u (itvl=%u, latency=%u, timeout=%u)",
                     desc.peer_id_addr.val[5], desc.peer_id_addr.val[4], desc.peer_id_addr.val[3],
                     desc.peer_id_addr.val[2], desc.peer_id_addr.val[1], desc.peer_id_addr.val[0],
                     desc.peer_id_addr.type == BLE_ADDR_PUBLIC ? "public" : "random",
                     event->connect.conn_handle, desc.conn_itvl, desc.conn_latency, desc.supervision_timeout);
        } else {
            ESP_LOGI(TAG, "[BLE CONNECT] Client connected on handle %u", event->connect.conn_handle);
        }
        rgb_led_trigger_event(LED_EVENT_BLE_CONNECT);
        rgb_led_set_connected(true);
        break;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "[BLE DISCONNECT] Client disconnected from handle %u (reason=%d)",
                 event->disconnect.conn.conn_handle, event->disconnect.reason);
        link_remove(event->disconnect.conn.conn_handle);
        rgb_led_trigger_event(LED_EVENT_BLE_DISCONNECT);
        rgb_led_set_connected(link_count() > 0);
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_adv_owned = false;
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "[BLE MTU] Client on handle %u negotiated MTU: %u bytes",
                 event->mtu.conn_handle, event->mtu.value);
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "[BLE PAIRING] Client on handle %u finished pairing/encryption (status=%d)",
                 event->enc_change.conn_handle, event->enc_change.status);
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

    // Appearance values from the Bluetooth SIG Assigned Numbers
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
        *out_appearance = 0x0180;
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
        *out_appearance = 0x0485;
        *out_type_name = "Cycling Sensor";
        break;
    case 10:
        *out_svc_uuid = 0x1812;
        *out_appearance = 0x03C7;
        *out_type_name = "Digital Pen";
        break;
    case 11:
        *out_svc_uuid = 0x1812;
        *out_appearance = 0x03C8;
        *out_type_name = "Barcode Scanner";
        break;
    default:
        *out_svc_uuid = 0x180A;
        *out_appearance = 0x0000;
        *out_type_name = "Generic";
        break;
    }
}

// Length of the longest prefix of s that is at most max bytes and does not split a UTF-8 sequence.
static size_t utf8_prefix_len(const char *s, size_t max)
{
    size_t n = strlen(s);
    if (n <= max) return n;
    n = max;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    return n;
}

// Starts connectable advertising with the name, address and payload currently set.
static int start_advertising(void)
{
    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(40); // 40ms fast advertising
    adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(60); // 60ms

    int rc = ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL, BLE_HS_FOREVER,
                               &adv_params, ble_gap_event_cb, NULL);
    s_adv_owned = (rc == 0);
    return rc;
}

/**
 * @brief Switches the advertiser to identity idx: device name, appearance, random static
 *        address, advertising data and scan response. Advertising must be stopped.
 */
static void advertise_identity(size_t idx)
{
    const char *name = s_ble_cfg.names[idx];
    size_t name_len = strlen(name);

    uint16_t svc_uuid_val = 0x180A;
    uint16_t appearance_val = 0x0000;
    const char *type_name = "Generic";
    get_accessory_profile(s_ble_cfg.accessory_type, idx, &svc_uuid_val, &appearance_val, &type_name);

    // 1. GAP service values a connected client reads back
    int rc = ble_svc_gap_device_name_set(name);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_svc_gap_device_name_set('%s') failed: %d", name, rc);
    }
    ble_svc_gap_device_appearance_set(appearance_val);

    // 2. Generate a unique Random Static Address for this virtual Bluetooth device
    // Per BLE Core Spec, the 2 most significant bits of byte 5 must be 11 (0b11xxxxxx)
    uint8_t rnd_addr[6];
    memcpy(rnd_addr, s_base_mac, 6);
    rnd_addr[0] ^= ((uint8_t)(idx * 0x1F + 0x0D));
    rnd_addr[1] ^= ((uint8_t)(idx * 0x0B + 0x03));
    rnd_addr[5] = (rnd_addr[5] & 0x3F) | 0xC0;

    rc = ble_hs_id_set_rnd(rnd_addr);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_hs_id_set_rnd failed for [%u]: %d", (unsigned int)idx, rc);
        return;
    }

    // 3. Advertising data: flags (3 bytes) + one 16-bit service UUID (4) + appearance (4) + name
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    s_adv_svc_uuid.value = svc_uuid_val;
    fields.uuids16 = &s_adv_svc_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 0; // the GATT server exposes more services than this one

    size_t name_room = BLE_HS_ADV_MAX_SZ - 3 - 4 - 2;
    if (appearance_val != 0) {
        fields.appearance = appearance_val;
        fields.appearance_is_present = 1;
        name_room -= 4;
    }

    // Shorten the name in the primary packet if needed; the scan response carries it in full
    fields.name = (const uint8_t *)name;
    fields.name_len = (uint8_t)utf8_prefix_len(name, name_room);
    fields.name_is_complete = (fields.name_len == name_len);

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_adv_set_fields failed for '%s': %d", name, rc);
        return;
    }

    // 4. Scan response: complete device name (MAX_BLE_NAME_LEN fits one packet)
    struct ble_hs_adv_fields rsp_fields;
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.name = (const uint8_t *)name;
    rsp_fields.name_len = (uint8_t)name_len;
    rsp_fields.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_adv_rsp_set_fields failed for '%s': %d", name, rc);
        return;
    }

    // 5. Start Advertising as an undirected connectable peripheral
    rc = start_advertising();
    if (rc == 0) {
        ESP_LOGI(TAG, "Broadcasting BLE [%u/%u] as %s: '%s' (MAC: %02X:%02X:%02X:%02X:%02X:%02X)",
                 (unsigned int)(idx + 1), (unsigned int)s_ble_cfg.name_count, type_name, name,
                 rnd_addr[5], rnd_addr[4], rnd_addr[3], rnd_addr[2], rnd_addr[1], rnd_addr[0]);
        rgb_led_trigger_event(LED_EVENT_BLE_ADV);
    } else {
        ESP_LOGE(TAG, "ble_gap_adv_start failed for '%s': %d", name, rc);
    }
}

// A central that stays connected would freeze the identity rotation, so links are time-limited.
static void drop_overdue_links(void)
{
    if (s_ble_cfg.max_connection_ms == 0) {
        return;
    }
    TickType_t now = xTaskGetTickCount();
    for (size_t i = 0; i < BLE_MAX_LINKS; i++) {
        if (!s_links[i].used || (now - s_links[i].since) < pdMS_TO_TICKS(s_ble_cfg.max_connection_ms)) {
            continue;
        }
        uint16_t handle = s_links[i].handle;
        int rc = ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
        if (rc == 0) {
            ESP_LOGI(TAG, "[BLE DISCONNECT] Dropping client on handle %u after %lu s to resume identity rotation",
                     handle, (unsigned long)(s_ble_cfg.max_connection_ms / 1000));
        } else if (rc == BLE_HS_ENOTCONN) {
            link_remove(handle);
            rgb_led_set_connected(link_count() > 0);
        }
    }
}

static void ble_rotator_task(void *param)
{
    ESP_LOGI(TAG, "Starting BLE rotator task (%u names, dwell=%lu ms, Mode=%u)...",
             (unsigned int)s_ble_cfg.name_count,
             (unsigned long)s_ble_cfg.rotation_interval_ms,
             s_ble_cfg.accessory_type);

    size_t cur_idx = 0;
    unsigned int waiting_ticks = 0;

    while (1) {
        if (!ble_hs_synced()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        drop_overdue_links();

        bool rotate = (link_count() == 0);

        // Advertising that ended without a GAP event yet means a central is connecting right now.
        // Leave name and address alone until the connect event arrives. After two dwell periods
        // probe with the unchanged identity: the host refuses to advertise while it holds a link.
        if (rotate && s_adv_owned && !ble_gap_adv_active()) {
            rotate = false;
            if (++waiting_ticks > 2) {
                int rc = start_advertising();
                if (rc == 0) {
                    waiting_ticks = 0;
                } else if (rc != BLE_HS_ENOMEM) {
                    s_adv_owned = false;
                    rotate = true;
                }
            }
        }

        if (rotate && ble_gap_adv_active()) {
            // Any error here, BLE_HS_EALREADY included, means a central took the advertiser first
            rotate = (ble_gap_adv_stop() == 0);
            if (rotate) {
                s_adv_owned = false;
            }
        }

        if (rotate) {
            waiting_ticks = 0;
            advertise_identity(cur_idx);
            cur_idx = (cur_idx + 1) % s_ble_cfg.name_count;
        }

        vTaskDelay(pdMS_TO_TICKS(s_ble_cfg.rotation_interval_ms));
    }
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset (reason=%d); advertising resumes after re-sync", reason);
    s_adv_owned = false;
    for (size_t i = 0; i < BLE_MAX_LINKS; i++) {
        s_links[i].used = false;
    }
    rgb_led_set_connected(false);
}

static void ble_on_sync(void)
{
    // Called again after every host reset; the rotator task must exist only once
    s_adv_owned = false;
    if (s_rotator_task != NULL) {
        return;
    }
    ESP_LOGI(TAG, "NimBLE host synced. Launching multi-device advertising...");
    if (xTaskCreate(ble_rotator_task, "ble_rotator", 4096, NULL, 3, &s_rotator_task) != pdPASS) {
        s_rotator_task = NULL;
        ESP_LOGE(TAG, "Failed to create the BLE rotator task");
    }
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
    if (s_ble_cfg.rotation_interval_ms < BLE_MIN_ROTATION_MS) {
        ESP_LOGW(TAG, "Rotation interval %lu ms is below the %d ms minimum; using %d ms",
                 (unsigned long)s_ble_cfg.rotation_interval_ms, BLE_MIN_ROTATION_MS, BLE_MIN_ROTATION_MS);
        s_ble_cfg.rotation_interval_ms = BLE_MIN_ROTATION_MS;
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
    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_gatt_svcs);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to register GATT services: %d", rc);
        return false;
    }

    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;
    // No bonds are stored: a honeypot must accept the same central again and must not fill a bond table
    ble_hs_cfg.sm_bonding = 0;

    nimble_port_freertos_init(ble_host_task);
    return true;
}

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "probe_limits.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Unified configuration manifest for the PhantomProbe-C6 hardware probe.
 */
typedef struct {
    // IEEE 802.11 Wi-Fi Honeypot & Injection Parameters
    uint8_t channel;
    char country[3];                // ISO 3166-1 alpha-2 code, empty = keep the Wi-Fi driver default
    uint32_t burst_interval_ms;
    uint32_t burst_gap_ms;
    bool enable_probe_responder;
    size_t network_count;
    char ssids[MAX_VIRTUAL_NETWORKS][MAX_SSID_LEN + 1];

    // Bluetooth Low Energy Peripheral Reconnaissance Parameters
    bool enable_ble;
    uint32_t ble_adv_interval_ms;
    uint32_t ble_max_connection_s;  // 0 = never drop a connected central
    uint8_t ble_accessory_type;
    size_t ble_name_count;
    char ble_names[MAX_BLE_NAMES][MAX_BLE_NAME_LEN + 1];

    // Hardware Telemetry (WS2812 RMT LED)
    bool enable_led;
    uint8_t led_brightness;
} probe_config_t;

/**
 * @brief Parses the manifest compiled into the firmware (ssid_list.conf) and populates probe_config_t.
 *
 * @param config Pointer to probe_config_t struct to populate.
 */
void config_parser_load(probe_config_t *config);

/**
 * @brief Parses manifest text. Every field starts from its default; invalid lines are
 *        reported with ESP_LOGW and leave the default in place.
 *
 * @param text Manifest text (does not need to be NUL-terminated).
 * @param len Length of text in bytes.
 * @param config Pointer to probe_config_t struct to populate.
 */
void config_parser_parse(const char *text, size_t len, probe_config_t *config);

#ifdef __cplusplus
}
#endif

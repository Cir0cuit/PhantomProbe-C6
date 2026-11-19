#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "probe_limits.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration parameters for the multi-identity BLE peripheral reconnaissance probe.
 */
typedef struct {
    char names[MAX_BLE_NAMES][MAX_BLE_NAME_LEN + 1];
    size_t name_count;
    uint32_t rotation_interval_ms; // Dwell time per virtual BLE identity (at least 1000 ms)
    uint32_t max_connection_ms;    // Drop a connected central after this long, 0 = never
    uint8_t accessory_type;        // Emulated profile (0=Generic, 1=Keyboard, ..., 12=Auto-Distribute)
} ble_config_t;

/**
 * @brief Initializes the NimBLE Bluetooth stack, registers synthetic GATT services
 *        (Device Info, HID, Sensors), and launches the multi-identity peripheral advertiser.
 *
 * @param config Pointer to ble_config_t with identities and rotation timing.
 * @return true on successful initialization, false on failure.
 */
bool ble_multi_adv_init(const ble_config_t *config);

#ifdef __cplusplus
}
#endif

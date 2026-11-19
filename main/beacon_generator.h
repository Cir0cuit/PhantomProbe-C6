#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "probe_limits.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_BEACON_FRAME_LEN 128

/**
 * @brief Represents a synthetic virtual 802.11 access point identity.
 */
typedef struct {
    char ssid[MAX_SSID_LEN + 1];  // Target network SSID (up to 32 bytes + null)
    uint8_t bssid[6];             // Locally administered virtual MAC address
    uint8_t channel;              // Operating 2.4 GHz RF channel
    uint16_t beacon_interval_tu;  // Advertised beacon interval in time units (1 TU = 1024 us)
    uint16_t seq_num;             // IEEE 802.11 frame sequence counter
    uint32_t beacons_sent;        // Beacons accepted by the Wi-Fi driver (maintained by the caller)
} virtual_network_t;

/**
 * @brief Initializes virtual networks with configured SSIDs and derives unique,
 *        compliant locally administered BSSIDs (clearing multicast bit, setting U/L bit).
 *
 * @param networks Array of virtual_network_t structs.
 * @param ssids Array of SSID strings from the deployment manifest.
 * @param count Number of virtual networks to instantiate.
 * @param channel Target Wi-Fi channel (1-13).
 * @param beacon_interval_tu Beacon interval to advertise; must match the real transmit period.
 */
void beacon_generator_init_networks(virtual_network_t *networks, const char ssids[][MAX_SSID_LEN + 1], size_t count,
                                    uint8_t channel, uint16_t beacon_interval_tu);

/**
 * @brief Synthesizes a raw IEEE 802.11 Beacon management frame for low-level packet injection.
 *        Constructs MAC header, fixed parameters (timestamp, interval, capability),
 *        and tagged Information Elements (SSID, Supported Rates, DS Parameter, TIM, ERP, Extended Rates).
 *
 * @param net Pointer to the virtual network target.
 * @param buffer Output buffer where the raw 802.11 frame will be assembled.
 * @param max_len Length of the output buffer; at least MAX_BEACON_FRAME_LEN.
 * @return int Number of bytes written to the buffer, or -1 on error.
 */
int beacon_generator_build_frame(virtual_network_t *net, uint8_t *buffer, size_t max_len);

/**
 * @brief Synthesizes a raw IEEE 802.11 Probe Response management frame in answer
 *        to client active discovery probes (wildcard or targeted).
 *
 * @param net Pointer to the virtual network target.
 * @param client_mac Hardware MAC address of the interrogating client station.
 * @param buffer Output buffer where the raw 802.11 frame will be assembled.
 * @param max_len Length of the output buffer; at least MAX_BEACON_FRAME_LEN.
 * @return int Number of bytes written to the buffer, or -1 on error.
 */
int beacon_generator_build_probe_response(virtual_network_t *net, const uint8_t *client_mac, uint8_t *buffer, size_t max_len);

#ifdef __cplusplus
}
#endif

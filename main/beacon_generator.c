#include "beacon_generator.h"
#include <string.h>
#include <stdio.h>
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_log.h"

static const char *TAG = "probe_beacon";

void beacon_generator_init_networks(virtual_network_t *networks, const char ssids[][33], size_t count, uint8_t channel)
{
    uint8_t base_mac[6] = {0};
    esp_err_t err = esp_read_mac(base_mac, ESP_MAC_WIFI_SOFTAP);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read hardware MAC; using research fallback template");
        base_mac[0] = 0x24;
        base_mac[1] = 0xEC;
        base_mac[2] = 0x4A;
        base_mac[3] = 0x01;
        base_mac[4] = 0x02;
        base_mac[5] = 0x03;
    }

    for (size_t i = 0; i < count; i++) {
        strncpy(networks[i].ssid, ssids[i], sizeof(networks[i].ssid) - 1);
        networks[i].ssid[sizeof(networks[i].ssid) - 1] = '\0';

        // Derive virtual BSSID:
        // Set Locally Administered bit (bit 1 = 1) and clear Multicast bit (bit 0 = 0)
        // This ensures fully compliant IEEE 802.11 unicast MACs that will not conflict with registered OUIs.
        memcpy(networks[i].bssid, base_mac, 6);
        networks[i].bssid[0] = (base_mac[0] | 0x02) & 0xFE;
        // Deterministically hash the final octet to provide distinct virtual BSSIDs
        networks[i].bssid[5] = (uint8_t)(base_mac[5] ^ ((i + 1) * 0x13));

        networks[i].channel = channel;
        networks[i].seq_num = 0;
        networks[i].beacons_sent = 0;

        ESP_LOGI(TAG, "Virtual AP [%u]: SSID='%s' -> BSSID=%02X:%02X:%02X:%02X:%02X:%02X (CH %u)",
                 (unsigned int)(i + 1),
                 networks[i].ssid,
                 networks[i].bssid[0], networks[i].bssid[1], networks[i].bssid[2],
                 networks[i].bssid[3], networks[i].bssid[4], networks[i].bssid[5],
                 networks[i].channel);
    }
}

int beacon_generator_build_frame(virtual_network_t *net, uint8_t *buffer, size_t max_len)
{
    if (!net || !buffer || max_len < 128) {
        return -1;
    }

    size_t idx = 0;

    // --- 802.11 MAC Header (24 bytes) ---
    // Frame Control: Management (00), Subtype: Beacon (1000) -> 0x8000
    buffer[idx++] = 0x80;
    buffer[idx++] = 0x00;

    // Duration: 0
    buffer[idx++] = 0x00;
    buffer[idx++] = 0x00;

    // Destination Address: Broadcast FF:FF:FF:FF:FF:FF
    memset(&buffer[idx], 0xFF, 6);
    idx += 6;

    // Source Address (SA): Virtual BSSID
    memcpy(&buffer[idx], net->bssid, 6);
    idx += 6;

    // BSSID: Virtual BSSID
    memcpy(&buffer[idx], net->bssid, 6);
    idx += 6;

    // Sequence Control: (seq_num << 4) & 0xFFF0 (Fragment 0)
    uint16_t seq_ctrl = (net->seq_num << 4) & 0xFFF0;
    buffer[idx++] = (uint8_t)(seq_ctrl & 0xFF);
    buffer[idx++] = (uint8_t)((seq_ctrl >> 8) & 0xFF);
    net->seq_num = (net->seq_num + 1) & 0x0FFF;

    // --- 802.11 Management Frame Body ---

    // 1. Timestamp (8 bytes, in microseconds)
    uint64_t ts = esp_timer_get_time();
    for (int i = 0; i < 8; i++) {
        buffer[idx++] = (uint8_t)((ts >> (i * 8)) & 0xFF);
    }

    // 2. Beacon Interval: 100 Time Units (102.4 ms) -> 0x0064
    buffer[idx++] = 0x64;
    buffer[idx++] = 0x00;

    // 3. Capability Information: ESS (0x0001) | Short Preamble (0x0020) | Short Slot Time (0x0400) = 0x0421
    buffer[idx++] = 0x21;
    buffer[idx++] = 0x04;

    // --- Tagged Parameters (Information Elements) ---

    // Tag 0: SSID Parameter Set
    size_t ssid_len = strlen(net->ssid);
    if (ssid_len > 32) ssid_len = 32;
    buffer[idx++] = 0x00;                // Tag Number
    buffer[idx++] = (uint8_t)ssid_len;   // Tag Length
    memcpy(&buffer[idx], net->ssid, ssid_len);
    idx += ssid_len;

    // Tag 1: Supported Rates (1, 2, 5.5, 11, 6, 9, 12, 18 Mbps)
    buffer[idx++] = 0x01; // Tag Number
    buffer[idx++] = 0x08; // Length
    buffer[idx++] = 0x82; // 1(B) Mbps
    buffer[idx++] = 0x84; // 2(B) Mbps
    buffer[idx++] = 0x8B; // 5.5(B) Mbps
    buffer[idx++] = 0x96; // 11(B) Mbps
    buffer[idx++] = 0x0C; // 6 Mbps
    buffer[idx++] = 0x12; // 9 Mbps
    buffer[idx++] = 0x18; // 12 Mbps
    buffer[idx++] = 0x24; // 18 Mbps

    // Tag 3: DS Parameter Set (Current Channel)
    buffer[idx++] = 0x03; // Tag Number
    buffer[idx++] = 0x01; // Length
    buffer[idx++] = net->channel;

    // Tag 5: Traffic Indication Map (TIM)
    buffer[idx++] = 0x05; // Tag Number
    buffer[idx++] = 0x04; // Length
    buffer[idx++] = 0x00; // DTIM Count: 0
    buffer[idx++] = 0x01; // DTIM Period: 1
    buffer[idx++] = 0x00; // Bitmap Control: 0
    buffer[idx++] = 0x00; // Partial Virtual Bitmap: 0

    // Tag 50 (0x32): Extended Supported Rates (24, 36, 48, 54 Mbps)
    buffer[idx++] = 0x32; // Tag Number
    buffer[idx++] = 0x04; // Length
    buffer[idx++] = 0x30; // 24 Mbps
    buffer[idx++] = 0x48; // 36 Mbps
    buffer[idx++] = 0x60; // 48 Mbps
    buffer[idx++] = 0x6C; // 54 Mbps

    net->beacons_sent++;
    return (int)idx;
}

int beacon_generator_build_probe_response(virtual_network_t *net, const uint8_t *client_mac, uint8_t *buffer, size_t max_len)
{
    if (!net || !client_mac || !buffer || max_len < 128) {
        return -1;
    }

    size_t idx = 0;

    // --- 802.11 MAC Header (24 bytes) ---
    // Frame Control: Management (00), Subtype: Probe Response (0101) -> 0x0050 (0x50, 0x00)
    buffer[idx++] = 0x50;
    buffer[idx++] = 0x00;

    // Duration: 0
    buffer[idx++] = 0x00;
    buffer[idx++] = 0x00;

    // Destination Address: Requesting Client MAC
    memcpy(&buffer[idx], client_mac, 6);
    idx += 6;

    // Source Address (SA): Virtual BSSID
    memcpy(&buffer[idx], net->bssid, 6);
    idx += 6;

    // BSSID: Virtual BSSID
    memcpy(&buffer[idx], net->bssid, 6);
    idx += 6;

    // Sequence Control: (seq_num << 4) & 0xFFF0
    uint16_t seq_ctrl = (net->seq_num << 4) & 0xFFF0;
    buffer[idx++] = (uint8_t)(seq_ctrl & 0xFF);
    buffer[idx++] = (uint8_t)((seq_ctrl >> 8) & 0xFF);
    net->seq_num = (net->seq_num + 1) & 0x0FFF;

    // --- Fixed Management Frame Body ---
    // 1. Timestamp (8 bytes)
    uint64_t ts = esp_timer_get_time();
    for (int i = 0; i < 8; i++) {
        buffer[idx++] = (uint8_t)((ts >> (i * 8)) & 0xFF);
    }

    // 2. Beacon Interval (100 TUs)
    buffer[idx++] = 0x64;
    buffer[idx++] = 0x00;

    // 3. Capability Information: ESS (0x0001) | Short Preamble (0x0020) | Short Slot Time (0x0400) = 0x0421
    buffer[idx++] = 0x21;
    buffer[idx++] = 0x04;

    // --- Tagged Parameters ---
    // Tag 0: SSID Parameter Set
    size_t ssid_len = strlen(net->ssid);
    if (ssid_len > 32) ssid_len = 32;
    buffer[idx++] = 0x00;
    buffer[idx++] = (uint8_t)ssid_len;
    memcpy(&buffer[idx], net->ssid, ssid_len);
    idx += ssid_len;

    // Tag 1: Supported Rates
    buffer[idx++] = 0x01;
    buffer[idx++] = 0x08;
    buffer[idx++] = 0x82; // 1(B) Mbps
    buffer[idx++] = 0x84; // 2(B) Mbps
    buffer[idx++] = 0x8B; // 5.5(B) Mbps
    buffer[idx++] = 0x96; // 11(B) Mbps
    buffer[idx++] = 0x0C; // 6 Mbps
    buffer[idx++] = 0x12; // 9 Mbps
    buffer[idx++] = 0x18; // 12 Mbps
    buffer[idx++] = 0x24; // 18 Mbps

    // Tag 3: DS Parameter Set (Current Channel)
    buffer[idx++] = 0x03;
    buffer[idx++] = 0x01;
    buffer[idx++] = net->channel;

    // Tag 50 (0x32): Extended Supported Rates
    buffer[idx++] = 0x32;
    buffer[idx++] = 0x04;
    buffer[idx++] = 0x30; // 24 Mbps
    buffer[idx++] = 0x48; // 36 Mbps
    buffer[idx++] = 0x60; // 48 Mbps
    buffer[idx++] = 0x6C; // 54 Mbps

    return (int)idx;
}

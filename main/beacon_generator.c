#include "beacon_generator.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_log.h"

static const char *TAG = "probe_beacon";

#define FC0_BEACON         0x80
#define FC0_PROBE_RESPONSE 0x50

// Beacons and probe responses for one BSSID are built in different tasks.
static portMUX_TYPE s_seq_mux = portMUX_INITIALIZER_UNLOCKED;

void beacon_generator_init_networks(virtual_network_t *networks, const char ssids[][MAX_SSID_LEN + 1], size_t count,
                                    uint8_t channel, uint16_t beacon_interval_tu)
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
        networks[i].beacon_interval_tu = beacon_interval_tu;
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

static uint16_t next_seq(virtual_network_t *net)
{
    portENTER_CRITICAL(&s_seq_mux);
    uint16_t seq = net->seq_num;
    net->seq_num = (seq + 1) & 0x0FFF;
    portEXIT_CRITICAL(&s_seq_mux);
    return seq;
}

static int build_mgmt_frame(virtual_network_t *net, uint8_t fc0, const uint8_t *dest, uint8_t *buffer, size_t max_len)
{
    if (!net || !dest || !buffer || max_len < MAX_BEACON_FRAME_LEN) {
        return -1;
    }

    size_t idx = 0;

    // --- 802.11 MAC Header (24 bytes) ---
    // Frame Control: Management (00), Subtype: Beacon (1000) or Probe Response (0101)
    buffer[idx++] = fc0;
    buffer[idx++] = 0x00;

    // Duration: 0
    buffer[idx++] = 0x00;
    buffer[idx++] = 0x00;

    // Destination Address: broadcast for beacons, the requesting client for probe responses
    memcpy(&buffer[idx], dest, 6);
    idx += 6;

    // Source Address (SA): Virtual BSSID
    memcpy(&buffer[idx], net->bssid, 6);
    idx += 6;

    // BSSID: Virtual BSSID
    memcpy(&buffer[idx], net->bssid, 6);
    idx += 6;

    // Sequence Control: 12-bit sequence number, fragment 0
    uint16_t seq_ctrl = (uint16_t)(next_seq(net) << 4);
    buffer[idx++] = (uint8_t)(seq_ctrl & 0xFF);
    buffer[idx++] = (uint8_t)((seq_ctrl >> 8) & 0xFF);

    // --- 802.11 Management Frame Body ---

    // 1. Timestamp (8 bytes, in microseconds)
    uint64_t ts = (uint64_t)esp_timer_get_time();
    for (int i = 0; i < 8; i++) {
        buffer[idx++] = (uint8_t)((ts >> (i * 8)) & 0xFF);
    }

    // 2. Beacon Interval in Time Units (little endian)
    buffer[idx++] = (uint8_t)(net->beacon_interval_tu & 0xFF);
    buffer[idx++] = (uint8_t)(net->beacon_interval_tu >> 8);

    // 3. Capability Information: ESS (0x0001) | Short Preamble (0x0020) | Short Slot Time (0x0400) = 0x0421
    buffer[idx++] = 0x21;
    buffer[idx++] = 0x04;

    // --- Tagged Parameters (Information Elements) ---

    // Tag 0: SSID Parameter Set
    size_t ssid_len = strlen(net->ssid);
    if (ssid_len > MAX_SSID_LEN) ssid_len = MAX_SSID_LEN;
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

    if (fc0 == FC0_BEACON) {
        // Tag 5: Traffic Indication Map (TIM)
        buffer[idx++] = 0x05; // Tag Number
        buffer[idx++] = 0x04; // Length
        buffer[idx++] = 0x00; // DTIM Count: 0
        buffer[idx++] = 0x01; // DTIM Period: 1
        buffer[idx++] = 0x00; // Bitmap Control: 0
        buffer[idx++] = 0x00; // Partial Virtual Bitmap: 0
    }

    // Tag 42 (0x2A): ERP Information (no non-ERP stations, no protection, short preamble allowed)
    buffer[idx++] = 0x2A; // Tag Number
    buffer[idx++] = 0x01; // Length
    buffer[idx++] = 0x00;

    // Tag 50 (0x32): Extended Supported Rates (24, 36, 48, 54 Mbps)
    buffer[idx++] = 0x32; // Tag Number
    buffer[idx++] = 0x04; // Length
    buffer[idx++] = 0x30; // 24 Mbps
    buffer[idx++] = 0x48; // 36 Mbps
    buffer[idx++] = 0x60; // 48 Mbps
    buffer[idx++] = 0x6C; // 54 Mbps

    return (int)idx;
}

int beacon_generator_build_frame(virtual_network_t *net, uint8_t *buffer, size_t max_len)
{
    static const uint8_t broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    return build_mgmt_frame(net, FC0_BEACON, broadcast, buffer, max_len);
}

int beacon_generator_build_probe_response(virtual_network_t *net, const uint8_t *client_mac, uint8_t *buffer, size_t max_len)
{
    return build_mgmt_frame(net, FC0_PROBE_RESPONSE, client_mac, buffer, max_len);
}

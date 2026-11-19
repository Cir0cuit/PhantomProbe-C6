#pragma once

// Limits shared by the config parser, the Wi-Fi frame generator and the BLE advertiser.

#define MAX_VIRTUAL_NETWORKS 16
#define MAX_SSID_LEN 32      // bytes, IEEE 802.11 SSID element limit

#define MAX_BLE_NAMES 16
#define MAX_BLE_NAME_LEN 29  // bytes, a complete local name must fit one 31-byte scan response

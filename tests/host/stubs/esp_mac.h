#pragma once
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
typedef enum { ESP_MAC_WIFI_STA, ESP_MAC_WIFI_SOFTAP, ESP_MAC_BT } esp_mac_type_t;
extern uint8_t g_fake_mac[6];
static inline esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type) { (void)type; memcpy(mac, g_fake_mac, 6); return ESP_OK; }

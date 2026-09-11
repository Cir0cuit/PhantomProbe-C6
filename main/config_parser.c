#include "config_parser.h"
#include "embedded_config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_log.h"

static const char *TAG = "probe_config";

typedef enum {
    SECTION_NONE,
    SECTION_WIFI,
    SECTION_NETWORKS,
    SECTION_BLUETOOTH,
    SECTION_LED
} config_section_t;

static char *trim_whitespace(char *str)
{
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;

    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return str;
}

void config_parser_load(probe_config_t *config)
{
    // Probe default parameters
    config->channel = 1;
    config->burst_interval_ms = 50;
    config->burst_gap_ms = 2;
    config->enable_probe_responder = true;
    config->network_count = 0;

    config->enable_ble = true;
    config->ble_adv_interval_ms = 2000;
    config->ble_accessory_type = 12; // 12 = Auto-Distribute showcase mode
    config->ble_name_count = 0;

    config->enable_led = true;
    config->led_brightness = 5;

    size_t conf_len = strlen(EMBEDDED_SSID_LIST_CONF);
    if (conf_len == 0) {
        ESP_LOGW(TAG, "Embedded ssid_list.conf is empty! Using defensive probe defaults.");
        return;
    }

    ESP_LOGI(TAG, "Parsing PhantomProbe-C6 deployment manifest (%u bytes)...", (unsigned int)conf_len);

    config_section_t current_section = SECTION_NONE;
    const char *ptr = EMBEDDED_SSID_LIST_CONF;
    const char *end = ptr + conf_len;

    char line_buf[128];

    while (ptr < end) {
        size_t line_len = 0;
        while (ptr < end && *ptr != '\n' && *ptr != '\r' && line_len < sizeof(line_buf) - 1) {
            line_buf[line_len++] = *ptr++;
        }
        line_buf[line_len] = '\0';

        while (ptr < end && (*ptr == '\n' || *ptr == '\r')) {
            ptr++;
        }

        char *line = trim_whitespace(line_buf);
        if (*line == '\0' || *line == '#' || *line == ';') {
            continue;
        }

        // Check for section header
        if (*line == '[') {
            char *closing = strchr(line, ']');
            if (closing) {
                *closing = '\0';
                char *sec_name = trim_whitespace(line + 1);
                if (strcasecmp(sec_name, "wifi") == 0) {
                    current_section = SECTION_WIFI;
                } else if (strcasecmp(sec_name, "networks") == 0) {
                    current_section = SECTION_NETWORKS;
                } else if (strcasecmp(sec_name, "bluetooth") == 0) {
                    current_section = SECTION_BLUETOOTH;
                } else if (strcasecmp(sec_name, "led") == 0) {
                    current_section = SECTION_LED;
                } else {
                    current_section = SECTION_NONE;
                }
            }
            continue;
        }

        if (current_section == SECTION_WIFI) {
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                char *key = trim_whitespace(line);
                char *val = trim_whitespace(eq + 1);

                if (strcasecmp(key, "channel") == 0) {
                    int ch = atoi(val);
                    if (ch >= 1 && ch <= 13) config->channel = (uint8_t)ch;
                } else if (strcasecmp(key, "burst_interval_ms") == 0) {
                    int interval = atoi(val);
                    if (interval >= 10 && interval <= 5000) config->burst_interval_ms = (uint32_t)interval;
                } else if (strcasecmp(key, "burst_gap_ms") == 0) {
                    int gap = atoi(val);
                    if (gap >= 1 && gap <= 100) config->burst_gap_ms = (uint32_t)gap;
                } else if (strcasecmp(key, "enable_probe_responder") == 0) {
                    config->enable_probe_responder = (atoi(val) != 0);
                }
            }
        } else if (current_section == SECTION_NETWORKS) {
            if (config->network_count < MAX_VIRTUAL_NETWORKS) {
                strncpy(config->ssids[config->network_count], line, MAX_SSID_LEN);
                config->ssids[config->network_count][MAX_SSID_LEN] = '\0';
                config->network_count++;
            }
        } else if (current_section == SECTION_BLUETOOTH) {
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                char *key = trim_whitespace(line);
                char *val = trim_whitespace(eq + 1);
                if (strcasecmp(key, "enable_ble") == 0) {
                    config->enable_ble = (atoi(val) != 0);
                } else if (strcasecmp(key, "ble_adv_interval_ms") == 0) {
                    int itvl = atoi(val);
                    if (itvl >= 200 && itvl <= 10000) config->ble_adv_interval_ms = (uint32_t)itvl;
                } else if (strcasecmp(key, "accessory_type") == 0) {
                    int acc = atoi(val);
                    if (acc >= 0 && acc <= 12) config->ble_accessory_type = (uint8_t)acc;
                }
            } else {
                // Custom Bluetooth Name
                if (config->ble_name_count < MAX_BLE_NAMES) {
                    strncpy(config->ble_names[config->ble_name_count], line, MAX_BLE_NAME_LEN);
                    config->ble_names[config->ble_name_count][MAX_BLE_NAME_LEN] = '\0';
                    config->ble_name_count++;
                }
            }
        } else if (current_section == SECTION_LED) {
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                char *key = trim_whitespace(line);
                char *val = trim_whitespace(eq + 1);
                if (strcasecmp(key, "enable_led") == 0) {
                    config->enable_led = (atoi(val) != 0);
                } else if (strcasecmp(key, "brightness") == 0) {
                    int br = atoi(val);
                    if (br >= 1 && br <= 100) config->led_brightness = (uint8_t)br;
                }
            }
        }
    }

    // Default fallback networks if empty
    if (config->network_count == 0) {
        const char *fallbacks[] = {
            "IoT-Sensors-Net",
            "IoT-Cameras-Net",
            "IoT-Automation-Net",
            "IoT-Isolated-DMZ"
        };
        for (size_t i = 0; i < 4; i++) {
            strncpy(config->ssids[i], fallbacks[i], MAX_SSID_LEN);
            config->ssids[i][MAX_SSID_LEN] = '\0';
        }
        config->network_count = 4;
    }

    // If no custom BLE names configured, automatically derive from Wi-Fi SSIDs
    if (config->ble_name_count == 0) {
        for (size_t i = 0; i < config->network_count && i < MAX_BLE_NAMES; i++) {
            // Use SSID with -BLE suffix (or as-is if space tight)
            snprintf(config->ble_names[i], sizeof(config->ble_names[i]), "%.24s-BLE", config->ssids[i]);
            config->ble_name_count++;
        }
    }

    const char *acc_names[] = {
        "Generic", "Keyboard (HID)", "Mouse (HID)", "Gamepad (HID)",
        "Joystick (HID)", "Remote Control (HID)", "Smartwatch", "Heart Rate Monitor",
        "Thermometer", "Cycling Sensor", "Digital Stylus (HID)", "Barcode Scanner (HID)",
        "Auto-Distribute (Showcase Multi-Icon)"
    };
    const char *mode_str = (config->ble_accessory_type <= 12) ? acc_names[config->ble_accessory_type] : "Unknown";

    ESP_LOGI(TAG, "Config loaded: Wi-Fi=%u (CH=%u), BLE=%s (%u names, Mode=%s), LED=%s (Brightness=%u%%)",
             (unsigned int)config->network_count,
             config->channel,
             config->enable_ble ? "ON" : "OFF",
             (unsigned int)config->ble_name_count,
             mode_str,
             config->enable_led ? "ON" : "OFF",
             config->led_brightness);
}

#include "config_parser.h"
#include "embedded_config.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_log.h"

static const char *TAG = "probe_config";

typedef enum {
    SECTION_NONE,     // before the first header
    SECTION_UNKNOWN,  // inside a header that was not recognised (already reported)
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

// Length of the longest prefix of s that is at most max bytes and does not split a UTF-8 sequence.
static size_t utf8_prefix_len(const char *s, size_t max)
{
    size_t n = strlen(s);
    if (n <= max) return n;
    n = max;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    return n;
}

static void copy_name(char *dst, size_t max, const char *src, const char *what, unsigned int line_no)
{
    size_t n = utf8_prefix_len(src, max);
    memcpy(dst, src, n);
    dst[n] = '\0';
    if (src[n] != '\0') {
        ESP_LOGW(TAG, "line %u: %s longer than %u bytes, shortened to '%s'", line_no, what, (unsigned int)max, dst);
    }
}

// Removes one pair of surrounding double quotes, so names may start with '#', ';', '[' or a space.
static bool unquote(char **str)
{
    size_t n = strlen(*str);
    if (n >= 2 && (*str)[0] == '"' && (*str)[n - 1] == '"') {
        (*str)[n - 1] = '\0';
        (*str)++;
        return true;
    }
    return false;
}

// A value ends at the end of the line or at an inline comment.
static bool is_value_end(const char *p)
{
    while (isspace((unsigned char)*p)) p++;
    return *p == '\0' || *p == '#' || *p == ';';
}

static bool parse_int(const char *val, long lo, long hi, long *out)
{
    char *endp;
    long v = strtol(val, &endp, 10);
    if (endp == val || !is_value_end(endp) || v < lo || v > hi) {
        return false;
    }
    *out = v;
    return true;
}

static bool parse_bool(const char *val, bool *out)
{
    static const char *const words[] = { "1", "true", "yes", "on", "0", "false", "no", "off" };

    size_t n = 0;
    while (val[n] && !isspace((unsigned char)val[n]) && val[n] != '#' && val[n] != ';') n++;
    if (n == 0 || !is_value_end(val + n)) {
        return false;
    }
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (strlen(words[i]) == n && strncasecmp(val, words[i], n) == 0) {
            *out = (i < 4);
            return true;
        }
    }
    return false;
}

static void set_int(const char *key, const char *val, long lo, long hi, long *dst, unsigned int line_no)
{
    if (!parse_int(val, lo, hi, dst)) {
        ESP_LOGW(TAG, "line %u: %s = '%s' is not a number in %ld..%ld, keeping %ld", line_no, key, val, lo, hi, *dst);
    }
}

static void set_bool(const char *key, const char *val, bool *dst, unsigned int line_no)
{
    if (!parse_bool(val, dst)) {
        ESP_LOGW(TAG, "line %u: %s = '%s' is not 1/0 (true/false, yes/no, on/off), keeping %d", line_no, key, val, *dst);
    }
}

static void add_network(probe_config_t *config, const char *name, unsigned int line_no)
{
    if (*name == '\0') {
        ESP_LOGW(TAG, "line %u: empty SSID ignored", line_no);
        return;
    }
    if (config->network_count >= MAX_VIRTUAL_NETWORKS) {
        ESP_LOGW(TAG, "line %u: more than %d SSIDs, '%s' ignored", line_no, MAX_VIRTUAL_NETWORKS, name);
        return;
    }
    copy_name(config->ssids[config->network_count], MAX_SSID_LEN, name, "SSID", line_no);
    config->network_count++;
}

static void add_ble_name(probe_config_t *config, const char *name, unsigned int line_no)
{
    if (*name == '\0') {
        ESP_LOGW(TAG, "line %u: empty Bluetooth name ignored", line_no);
        return;
    }
    if (config->ble_name_count >= MAX_BLE_NAMES) {
        ESP_LOGW(TAG, "line %u: more than %d Bluetooth names, '%s' ignored", line_no, MAX_BLE_NAMES, name);
        return;
    }
    copy_name(config->ble_names[config->ble_name_count], MAX_BLE_NAME_LEN, name, "Bluetooth name", line_no);
    config->ble_name_count++;
}

static void parse_wifi_key(probe_config_t *config, const char *key, char *val, unsigned int line_no)
{
    long n;
    if (strcasecmp(key, "channel") == 0) {
        n = config->channel;
        set_int(key, val, 1, 13, &n, line_no);
        config->channel = (uint8_t)n;
    } else if (strcasecmp(key, "burst_interval_ms") == 0) {
        n = (long)config->burst_interval_ms;
        set_int(key, val, 10, 5000, &n, line_no);
        config->burst_interval_ms = (uint32_t)n;
    } else if (strcasecmp(key, "burst_gap_ms") == 0) {
        n = (long)config->burst_gap_ms;
        set_int(key, val, 0, 100, &n, line_no);
        config->burst_gap_ms = (uint32_t)n;
    } else if (strcasecmp(key, "enable_probe_responder") == 0) {
        set_bool(key, val, &config->enable_probe_responder, line_no);
    } else if (strcasecmp(key, "country") == 0) {
        if (isalpha((unsigned char)val[0]) && isalpha((unsigned char)val[1]) && is_value_end(val + 2)) {
            config->country[0] = (char)toupper((unsigned char)val[0]);
            config->country[1] = (char)toupper((unsigned char)val[1]);
            config->country[2] = '\0';
        } else if (!is_value_end(val)) {
            ESP_LOGW(TAG, "line %u: country = '%s' is not a two-letter code, ignored", line_no, val);
        }
    } else {
        ESP_LOGW(TAG, "line %u: unknown [wifi] key '%s' ignored", line_no, key);
    }
}

static void parse_bluetooth_key(probe_config_t *config, const char *key, char *val, unsigned int line_no)
{
    long n;
    if (strcasecmp(key, "enable_ble") == 0) {
        set_bool(key, val, &config->enable_ble, line_no);
    } else if (strcasecmp(key, "ble_adv_interval_ms") == 0) {
        n = (long)config->ble_adv_interval_ms;
        set_int(key, val, 1000, 10000, &n, line_no);
        config->ble_adv_interval_ms = (uint32_t)n;
    } else if (strcasecmp(key, "max_connection_s") == 0) {
        n = (long)config->ble_max_connection_s;
        set_int(key, val, 0, 3600, &n, line_no);
        config->ble_max_connection_s = (uint32_t)n;
    } else if (strcasecmp(key, "accessory_type") == 0) {
        n = config->ble_accessory_type;
        set_int(key, val, 0, 12, &n, line_no);
        config->ble_accessory_type = (uint8_t)n;
    } else if (strcasecmp(key, "name") == 0) {
        // A quoted value may be followed by a comment; an unquoted one is taken literally
        if (val[0] == '"') {
            for (char *q = strchr(val + 1, '"'); q; q = strchr(q + 1, '"')) {
                if (is_value_end(q + 1)) {
                    *q = '\0';
                    val++;
                    break;
                }
            }
        }
        add_ble_name(config, val, line_no);
    } else {
        ESP_LOGW(TAG, "line %u: unknown [bluetooth] key '%s' ignored (use 'name = ...' for a name containing '=')", line_no, key);
    }
}

static void parse_led_key(probe_config_t *config, const char *key, char *val, unsigned int line_no)
{
    long n;
    if (strcasecmp(key, "enable_led") == 0) {
        set_bool(key, val, &config->enable_led, line_no);
    } else if (strcasecmp(key, "brightness") == 0) {
        n = config->led_brightness;
        set_int(key, val, 0, 100, &n, line_no);
        config->led_brightness = (uint8_t)n;
    } else {
        ESP_LOGW(TAG, "line %u: unknown [led] key '%s' ignored", line_no, key);
    }
}

static void parse_key_value(probe_config_t *config, config_section_t section, char *line, unsigned int line_no)
{
    char *eq = strchr(line, '=');
    if (!eq) {
        ESP_LOGW(TAG, "line %u: expected key = value, got '%s'", line_no, line);
        return;
    }
    *eq = '\0';
    char *key = trim_whitespace(line);
    char *val = trim_whitespace(eq + 1);

    if (section == SECTION_WIFI) {
        parse_wifi_key(config, key, val, line_no);
    } else if (section == SECTION_BLUETOOTH) {
        parse_bluetooth_key(config, key, val, line_no);
    } else {
        parse_led_key(config, key, val, line_no);
    }
}

// "[name]" optionally followed by a comment is a section header. Anything else that merely
// starts with '[' (for example the SSID "[5G] Home") is ordinary data.
static bool parse_section_header(char *line, config_section_t *section, unsigned int line_no)
{
    if (*line != '[') return false;
    char *closing = strchr(line, ']');
    if (!closing || !is_value_end(closing + 1)) return false;

    *closing = '\0';
    char *name = trim_whitespace(line + 1);
    if (strcasecmp(name, "wifi") == 0) {
        *section = SECTION_WIFI;
    } else if (strcasecmp(name, "networks") == 0) {
        *section = SECTION_NETWORKS;
    } else if (strcasecmp(name, "bluetooth") == 0) {
        *section = SECTION_BLUETOOTH;
    } else if (strcasecmp(name, "led") == 0) {
        *section = SECTION_LED;
    } else {
        ESP_LOGW(TAG, "line %u: unknown section [%s], its lines are ignored (quote a name that starts with '[')", line_no, name);
        *section = SECTION_UNKNOWN;
    }
    return true;
}

static void derive_ble_names(probe_config_t *config)
{
    // Mirror the SSIDs. The "-BLE" suffix is used only when it fits for every name.
    bool suffix = true;
    for (size_t i = 0; i < config->network_count; i++) {
        if (strlen(config->ssids[i]) + 4 > MAX_BLE_NAME_LEN) suffix = false;
    }
    for (size_t i = 0; i < config->network_count && i < MAX_BLE_NAMES; i++) {
        char *dst = config->ble_names[i];
        size_t n = utf8_prefix_len(config->ssids[i], suffix ? MAX_BLE_NAME_LEN - 4 : MAX_BLE_NAME_LEN);
        memcpy(dst, config->ssids[i], n);
        if (suffix) {
            memcpy(dst + n, "-BLE", 4);
            n += 4;
        }
        dst[n] = '\0';
        config->ble_name_count++;
    }
}

void config_parser_parse(const char *text, size_t len, probe_config_t *config)
{
    memset(config, 0, sizeof(*config));

    // Probe default parameters
    config->channel = 1;
    config->burst_interval_ms = 50;
    config->burst_gap_ms = 2;
    config->enable_probe_responder = true;

    config->enable_ble = true;
    config->ble_adv_interval_ms = 2000;
    config->ble_max_connection_s = 30;
    config->ble_accessory_type = 12; // 12 = Auto-Distribute showcase mode

    config->enable_led = true;
    config->led_brightness = 5;

    config_section_t current_section = SECTION_NONE;
    const char *ptr = text;
    const char *end = text + len;
    unsigned int line_no = 0;

    char line_buf[128];

    while (ptr < end) {
        line_no++;
        size_t line_len = 0;
        while (ptr < end && *ptr != '\n' && *ptr != '\r' && line_len < sizeof(line_buf) - 1) {
            line_buf[line_len++] = *ptr++;
        }
        line_buf[line_len] = '\0';

        bool overlong = false;
        while (ptr < end && *ptr != '\n' && *ptr != '\r') {
            overlong = true;
            ptr++;
        }
        if (ptr < end && *ptr == '\r') ptr++;
        if (ptr < end && *ptr == '\n') ptr++;

        char *line = trim_whitespace(line_buf);
        if (*line == '\0' || *line == '#' || *line == ';') {
            continue;
        }
        if (overlong) {
            ESP_LOGW(TAG, "line %u: longer than %u bytes, the rest of the line is ignored", line_no, (unsigned int)(sizeof(line_buf) - 1));
        }

        if (parse_section_header(line, &current_section, line_no)) {
            continue;
        }

        if (current_section == SECTION_NETWORKS) {
            unquote(&line);
            add_network(config, line, line_no);
        } else if (current_section == SECTION_BLUETOOTH) {
            if (unquote(&line) || !strchr(line, '=')) {
                add_ble_name(config, line, line_no);
            } else {
                parse_key_value(config, current_section, line, line_no);
            }
        } else if (current_section == SECTION_WIFI || current_section == SECTION_LED) {
            parse_key_value(config, current_section, line, line_no);
        } else if (current_section == SECTION_NONE) {
            ESP_LOGW(TAG, "line %u: '%s' is not inside a section, ignored (a header is [name] alone on its line)", line_no, line);
        }
    }

    // Default fallback networks if empty
    if (config->network_count == 0) {
        static const char *const fallbacks[] = {
            "IoT-Sensors-Net",
            "IoT-Cameras-Net",
            "IoT-Automation-Net",
            "IoT-Isolated-DMZ"
        };
        ESP_LOGW(TAG, "No SSIDs in [networks]; using the built-in fallback SSIDs");
        for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); i++) {
            strcpy(config->ssids[i], fallbacks[i]);
        }
        config->network_count = sizeof(fallbacks) / sizeof(fallbacks[0]);
    }

    // If no custom BLE names configured, automatically derive from Wi-Fi SSIDs
    if (config->ble_name_count == 0) {
        derive_ble_names(config);
    }
}

void config_parser_load(probe_config_t *config)
{
    size_t conf_len = sizeof(EMBEDDED_SSID_LIST_CONF) - 1;
    ESP_LOGI(TAG, "Parsing PhantomProbe-C6 deployment manifest (%u bytes)...", (unsigned int)conf_len);
    config_parser_parse(EMBEDDED_SSID_LIST_CONF, conf_len, config);

    static const char *const acc_names[] = {
        "Generic", "Keyboard (HID)", "Mouse (HID)", "Gamepad (HID)",
        "Joystick (HID)", "Remote Control", "Smartwatch", "Heart Rate Monitor",
        "Thermometer", "Cycling Sensor", "Digital Pen (HID)", "Barcode Scanner (HID)",
        "Auto-Distribute (Showcase Multi-Icon)"
    };
    const char *mode_str = (config->ble_accessory_type <= 12) ? acc_names[config->ble_accessory_type] : "Unknown";

    ESP_LOGI(TAG, "Config loaded: Wi-Fi=%u (CH=%u, Responder=%s), BLE=%s (%u names, Mode=%s), LED=%s (Brightness=%u%%)",
             (unsigned int)config->network_count,
             config->channel,
             config->enable_probe_responder ? "ON" : "OFF",
             config->enable_ble ? "ON" : "OFF",
             (unsigned int)config->ble_name_count,
             mode_str,
             config->enable_led ? "ON" : "OFF",
             config->led_brightness);
}

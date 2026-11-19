// Host unit tests for the manifest parser and the 802.11 frame builder.
// Run with: python tests/host/run.py   (add -v to print the firmware's log lines)
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "beacon_generator.h"
#include "config_parser.h"

int g_log_warnings = 0;
int g_log_verbose = 0;
int64_t g_fake_time_us = 0x0102030405060708LL;
uint8_t g_fake_mac[6] = { 0x9C, 0xCC, 0x01, 0x44, 0x39, 0xA1 };

static int s_checks = 0;
static int s_failures = 0;

#define CHECK(cond) do { \
        s_checks++; \
        if (!(cond)) { s_failures++; printf("  FAIL %s:%d: %s\n", __func__, __LINE__, #cond); } \
    } while (0)
#define CHECK_STR(actual, expected) do { \
        s_checks++; \
        if (strcmp((actual), (expected)) != 0) { \
            s_failures++; printf("  FAIL %s:%d: got '%s', expected '%s'\n", __func__, __LINE__, (actual), (expected)); \
        } \
    } while (0)

static probe_config_t parse(const char *text)
{
    probe_config_t cfg;
    g_log_warnings = 0;
    config_parser_parse(text, strlen(text), &cfg);
    return cfg;
}

static void test_defaults_and_fallback(void)
{
    probe_config_t c = parse("");
    CHECK(c.channel == 1);
    CHECK(c.burst_interval_ms == 50);
    CHECK(c.burst_gap_ms == 2);
    CHECK(c.enable_probe_responder);
    CHECK(c.country[0] == '\0');
    CHECK(c.enable_ble);
    CHECK(c.ble_adv_interval_ms == 2000);
    CHECK(c.ble_max_connection_s == 30);
    CHECK(c.ble_accessory_type == 12);
    CHECK(c.enable_led);
    CHECK(c.led_brightness == 5);
    CHECK(c.network_count == 4);
    CHECK_STR(c.ssids[0], "IoT-Sensors-Net");
    CHECK(c.ble_name_count == 4);
    CHECK_STR(c.ble_names[0], "IoT-Sensors-Net-BLE");
    CHECK(g_log_warnings == 1); // the fallback is announced
}

static void test_full_manifest(void)
{
    probe_config_t c = parse(
        "# comment\r\n"
        "; other comment\r\n"
        "[wifi]\r\n"
        "channel = 6               # inline comment after a number\r\n"
        "burst_interval_ms=30\r\n"
        "burst_gap_ms = 0\r\n"
        "enable_probe_responder = off\r\n"
        "country = nl\r\n"
        "\r\n"
        "[networks]\r\n"
        "Alpha\r\n"
        "  Beta Net  \r\n"
        "\r\n"
        "[bluetooth]\r\n"
        "enable_ble = yes\r\n"
        "ble_adv_interval_ms = 1500\r\n"
        "max_connection_s = 0\r\n"
        "accessory_type = 3\r\n"
        "My Keyboard\r\n"
        "name = A=B\r\n"
        "\r\n"
        "[led]\r\n"
        "enable_led = TRUE\r\n"
        "brightness = 0\r\n");
    CHECK(g_log_warnings == 0);
    CHECK(c.channel == 6);
    CHECK(c.burst_interval_ms == 30);
    CHECK(c.burst_gap_ms == 0);
    CHECK(!c.enable_probe_responder);
    CHECK_STR(c.country, "NL");
    CHECK(c.network_count == 2);
    CHECK_STR(c.ssids[0], "Alpha");
    CHECK_STR(c.ssids[1], "Beta Net");
    CHECK(c.enable_ble);
    CHECK(c.ble_adv_interval_ms == 1500);
    CHECK(c.ble_max_connection_s == 0);
    CHECK(c.ble_accessory_type == 3);
    CHECK(c.ble_name_count == 2);
    CHECK_STR(c.ble_names[0], "My Keyboard");
    CHECK_STR(c.ble_names[1], "A=B");
    CHECK(c.enable_led);
    CHECK(c.led_brightness == 0);
}

static void test_invalid_values_keep_defaults(void)
{
    probe_config_t c = parse(
        "[wifi]\n"
        "channel = 14\n"
        "burst_interval_ms = fast\n"
        "burst_gap_ms = 101\n"
        "enable_probe_responder = maybe\n"
        "country = Netherlands\n"
        "tx_power = 5\n"
        "[bluetooth]\n"
        "ble_adv_interval_ms = 500\n"
        "accessory_type = 13\n"
        "mystery = 1\n"
        "[led]\n"
        "brightness = 101\n"
        "enable_led = 2\n"
        "[networks]\n"
        "Net\n");
    CHECK(c.channel == 1);
    CHECK(c.burst_interval_ms == 50);
    CHECK(c.burst_gap_ms == 2);
    CHECK(c.enable_probe_responder);
    CHECK(c.country[0] == '\0');
    CHECK(c.ble_adv_interval_ms == 2000);
    CHECK(c.ble_accessory_type == 12);
    CHECK(c.led_brightness == 5);
    CHECK(c.enable_led);
    CHECK(g_log_warnings == 11); // every rejected line is reported
}

static void test_overlong_lines(void)
{
    char text[700];
    char filler[201];
    memset(filler, 'x', 200);
    filler[200] = '\0';
    // A long comment whose tail used to be parsed as an SSID, and a long data line
    snprintf(text, sizeof(text), "[networks]\n# %s\nFirst\nchannel%s\nLast\n", filler, filler);
    probe_config_t c = parse(text);
    CHECK(c.network_count == 3);
    CHECK_STR(c.ssids[0], "First");
    CHECK(strlen(c.ssids[1]) == 32);
    CHECK_STR(c.ssids[2], "Last");
    CHECK(g_log_warnings == 2); // long data line: tail ignored + SSID shortened
}

static void test_section_headers_and_quotes(void)
{
    probe_config_t c = parse(
        "[networks]\n"
        "[5G] Home WiFi\n"
        "\"#FreeWifi\"\n"
        "\"; semi\"\n"
        "\"  padded  \"\n"
        "\"[networks]\"\n"
        "[ Networks ]   # header with a comment\n"
        "After\n");
    CHECK(g_log_warnings == 0);
    CHECK(c.network_count == 6);
    CHECK_STR(c.ssids[0], "[5G] Home WiFi");
    CHECK_STR(c.ssids[1], "#FreeWifi");
    CHECK_STR(c.ssids[2], "; semi");
    CHECK_STR(c.ssids[3], "  padded  ");
    CHECK_STR(c.ssids[4], "[networks]");
    CHECK_STR(c.ssids[5], "After");

    c = parse("[networks]\nKept\n[typo]\nLost\n[networks]\nBack\n\"\"\n");
    CHECK(c.network_count == 2);
    CHECK_STR(c.ssids[1], "Back");
    CHECK(g_log_warnings == 2); // unknown section + empty quoted name
}

static void test_stray_lines_and_quoted_names(void)
{
    // Text after a header's bracket makes it ordinary data, which is reported when no section is open
    probe_config_t c = parse("channel = 6\n[wifi] radio settings\nchannel = 7\n[networks]\nNet\n");
    CHECK(c.channel == 1);
    CHECK(c.network_count == 1);
    CHECK(g_log_warnings == 3);

    c = parse(
        "[networks]\nNet\n"
        "[bluetooth]\n"
        "name = \"Kitchen\" # comment\n"
        "name = \"My \"cool\" dev\"\n"
        "name = Printer #2\n"
        "name = \"\"\n");
    CHECK(c.ble_name_count == 3);
    CHECK_STR(c.ble_names[0], "Kitchen");
    CHECK_STR(c.ble_names[1], "My \"cool\" dev");
    CHECK_STR(c.ble_names[2], "Printer #2");
    CHECK(g_log_warnings == 1); // the empty name
}

static void test_limits_and_utf8(void)
{
    char text[1024] = "[networks]\n";
    for (int i = 0; i < 20; i++) {
        char line[16];
        snprintf(line, sizeof(line), "Net%02d\n", i);
        strcat(text, line);
    }
    probe_config_t c = parse(text);
    CHECK(c.network_count == MAX_VIRTUAL_NETWORKS);
    CHECK_STR(c.ssids[15], "Net15");
    CHECK(g_log_warnings == 4);

    // 31 ASCII bytes followed by a two-byte character: the cut must not split it
    c = parse("[networks]\n1234567890123456789012345678901\xC3\xA9tail\n");
    CHECK(strlen(c.ssids[0]) == 31);
    CHECK(g_log_warnings == 1);

    // Exactly 32 bytes ending in a complete two-byte character is kept whole
    c = parse("[networks]\n123456789012345678901234567890\xC3\xA9\n");
    CHECK(strlen(c.ssids[0]) == 32);
    CHECK(g_log_warnings == 0);

    c = parse("[bluetooth]\n123456789012345678901234567890\n[networks]\nN\n");
    CHECK(strlen(c.ble_names[0]) == MAX_BLE_NAME_LEN);
    CHECK(g_log_warnings == 1);
}

static void test_derived_ble_names(void)
{
    probe_config_t c = parse("[networks]\nShort\n1234567890123456789012345\n");
    CHECK(c.ble_name_count == 2);
    CHECK_STR(c.ble_names[0], "Short-BLE");
    CHECK_STR(c.ble_names[1], "1234567890123456789012345-BLE"); // 25 + 4 = 29 bytes

    // One name too long for the suffix: no name gets it, so the set stays uniform
    c = parse("[networks]\nShort\n12345678901234567890123456\n12345678901234567890123456789012\n");
    CHECK_STR(c.ble_names[0], "Short");
    CHECK_STR(c.ble_names[1], "12345678901234567890123456");
    CHECK(strlen(c.ble_names[2]) == MAX_BLE_NAME_LEN);

    c = parse("[networks]\nA\n[bluetooth]\nenable_ble = 0\n");
    CHECK(!c.enable_ble);
    CHECK(c.ble_name_count == 1);
}

static void test_shipped_manifest(void)
{
    probe_config_t c;
    g_log_warnings = 0;
    config_parser_load(&c);
    CHECK(g_log_warnings == 0);
    CHECK(c.network_count >= 1);
    CHECK(c.ble_name_count >= 1);
}

// Walks the information elements and returns true when they end exactly at the frame end.
static bool elements_are_well_formed(const uint8_t *frame, int len, int *count, const uint8_t **ssid_ie)
{
    int pos = 36; // 24-byte header + 12 bytes of fixed fields
    *count = 0;
    while (pos + 2 <= len) {
        if (frame[pos] == 0) *ssid_ie = &frame[pos];
        pos += 2 + frame[pos + 1];
        (*count)++;
    }
    return pos == len;
}

static void test_beacon_frame(void)
{
    virtual_network_t nets[MAX_VIRTUAL_NETWORKS];
    char ssids[MAX_VIRTUAL_NETWORKS][MAX_SSID_LEN + 1];
    for (int i = 0; i < MAX_VIRTUAL_NETWORKS; i++) {
        snprintf(ssids[i], sizeof(ssids[i]), "Net-%d", i);
    }
    strcpy(ssids[1], "12345678901234567890123456789012"); // maximum length
    beacon_generator_init_networks(nets, ssids, MAX_VIRTUAL_NETWORKS, 6, 46);

    for (int i = 0; i < MAX_VIRTUAL_NETWORKS; i++) {
        CHECK((nets[i].bssid[0] & 0x03) == 0x02); // locally administered, unicast
        for (int j = 0; j < i; j++) {
            CHECK(memcmp(nets[i].bssid, nets[j].bssid, 6) != 0);
        }
    }

    uint8_t frame[MAX_BEACON_FRAME_LEN];
    int len = beacon_generator_build_frame(&nets[0], frame, sizeof(frame));
    CHECK(len == 24 + 12 + (2 + 5) + 10 + 3 + 6 + 3 + 6);
    CHECK(frame[0] == 0x80 && frame[1] == 0x00);
    static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK(memcmp(&frame[4], bcast, 6) == 0);
    CHECK(memcmp(&frame[10], nets[0].bssid, 6) == 0);
    CHECK(memcmp(&frame[16], nets[0].bssid, 6) == 0);
    CHECK(frame[22] == 0x00 && frame[23] == 0x00);        // sequence 0
    CHECK(frame[24] == 0x08 && frame[31] == 0x01);        // little-endian timestamp
    CHECK(frame[32] == 46 && frame[33] == 0);             // beacon interval in TU
    CHECK(frame[34] == 0x21 && frame[35] == 0x04);        // capabilities

    int count = 0;
    const uint8_t *ssid_ie = NULL;
    CHECK(elements_are_well_formed(frame, len, &count, &ssid_ie));
    CHECK(count == 6);
    CHECK(ssid_ie == &frame[36] && ssid_ie[1] == 5 && memcmp(&ssid_ie[2], "Net-0", 5) == 0);
    CHECK(frame[36 + 7 + 10 + 2] == 6);                   // DS parameter: channel

    len = beacon_generator_build_frame(&nets[0], frame, sizeof(frame));
    CHECK(frame[22] == 0x10 && frame[23] == 0x00);        // sequence 1 << 4
    CHECK(nets[0].beacons_sent == 0);                     // counted by the caller on TX success

    nets[0].seq_num = 0x0FFF;
    beacon_generator_build_frame(&nets[0], frame, sizeof(frame));
    CHECK(frame[22] == 0xF0 && frame[23] == 0xFF);
    CHECK(nets[0].seq_num == 0);                          // 12-bit wrap

    len = beacon_generator_build_frame(&nets[1], frame, sizeof(frame));
    CHECK(len == 98 && len <= MAX_BEACON_FRAME_LEN);
    CHECK(elements_are_well_formed(frame, len, &count, &ssid_ie));
    CHECK(ssid_ie[1] == 32);

    CHECK(beacon_generator_build_frame(&nets[0], frame, MAX_BEACON_FRAME_LEN - 1) == -1);
    CHECK(beacon_generator_build_frame(NULL, frame, sizeof(frame)) == -1);
}

static void test_probe_response_frame(void)
{
    virtual_network_t nets[1];
    char ssids[1][MAX_SSID_LEN + 1] = { "Honeypot" };
    beacon_generator_init_networks(nets, ssids, 1, 11, 100);

    static const uint8_t client[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    uint8_t frame[MAX_BEACON_FRAME_LEN];
    int len = beacon_generator_build_probe_response(&nets[0], client, frame, sizeof(frame));
    CHECK(len == 24 + 12 + (2 + 8) + 10 + 3 + 3 + 6);     // no TIM element
    CHECK(frame[0] == 0x50);
    CHECK(memcmp(&frame[4], client, 6) == 0);
    CHECK(memcmp(&frame[10], nets[0].bssid, 6) == 0);
    CHECK(frame[32] == 100 && frame[33] == 0);

    int count = 0;
    const uint8_t *ssid_ie = NULL;
    CHECK(elements_are_well_formed(frame, len, &count, &ssid_ie));
    CHECK(count == 5);
    for (int pos = 36; pos < len; pos += 2 + frame[pos + 1]) {
        CHECK(frame[pos] != 0x05);
    }
    CHECK(beacon_generator_build_probe_response(&nets[0], NULL, frame, sizeof(frame)) == -1);
}

int main(int argc, char **argv)
{
    g_log_verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);

    test_defaults_and_fallback();
    test_full_manifest();
    test_invalid_values_keep_defaults();
    test_overlong_lines();
    test_section_headers_and_quotes();
    test_stray_lines_and_quoted_names();
    test_limits_and_utf8();
    test_derived_ble_names();
    test_shipped_manifest();
    test_beacon_frame();
    test_probe_response_frame();

    printf("%d checks, %d failed\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}

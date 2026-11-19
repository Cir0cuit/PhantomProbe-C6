#pragma once
#include <stdio.h>

// Host stand-in for the ESP-IDF logger: counts warnings and errors so tests can assert on them.
extern int g_log_warnings;
extern int g_log_verbose;

#define STUB_LOG(level, tag, fmt, ...) do { if (g_log_verbose) printf("    %s (%s) " fmt "\n", level, tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, fmt, ...) do { g_log_warnings++; STUB_LOG("E", tag, fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) do { g_log_warnings++; STUB_LOG("W", tag, fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGI(tag, fmt, ...) STUB_LOG("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) STUB_LOG("D", tag, fmt, ##__VA_ARGS__)

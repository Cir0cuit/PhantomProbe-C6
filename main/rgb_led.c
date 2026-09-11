#include "rgb_led.h"
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"

static const char *TAG = "probe_led";

static rmt_channel_handle_t s_tx_chan = NULL;
static rmt_encoder_handle_t s_copy_encoder = NULL;
static uint8_t s_brightness = 5; // Default to eye-safe 5% (PWM ~6/255 -> ~2.3% duty cycle)
static QueueHandle_t s_event_queue = NULL;

static void ws2812_write(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_tx_chan || !s_copy_encoder) return;

    if (s_brightness == 0) {
        r = g = b = 0;
    } else {
        // Eye-safe scaling:
        // Capped so even at 100% it is gentle and comfortable for desktop development.
        // For s_brightness = 5 (default): max_pwm = 3 + (5 * 70)/100 = 6.
        // With PWM = 6 (out of 255), duty cycle is ~2.3%, which produces a soft,
        // clearly visible glow with zero eye fatigue or retina persistence.
        uint32_t max_pwm = 3 + ((uint32_t)s_brightness * 70) / 100;
        if (max_pwm > 75) max_pwm = 75;

        uint32_t scaled_r = ((uint32_t)r * max_pwm) / 255;
        uint32_t scaled_g = ((uint32_t)g * max_pwm) / 255;
        uint32_t scaled_b = ((uint32_t)b * max_pwm) / 255;

        r = (r > 0 && scaled_r == 0) ? 1 : (uint8_t)scaled_r;
        g = (g > 0 && scaled_g == 0) ? 1 : (uint8_t)scaled_g;
        b = (b > 0 && scaled_b == 0) ? 1 : (uint8_t)scaled_b;
    }

    // WS2812 expects GRB order (Green, Red, Blue)
    uint32_t grb = ((uint32_t)g << 16) | ((uint32_t)r << 8) | (uint32_t)b;

    rmt_symbol_word_t symbols[25];
    for (int i = 0; i < 24; i++) {
        bool bit = (grb >> (23 - i)) & 1;
        if (bit) {
            // WS2812 '1': 0.7us high, 0.5us low (7 ticks, 5 ticks @ 10MHz)
            symbols[i].duration0 = 7;
            symbols[i].level0 = 1;
            symbols[i].duration1 = 5;
            symbols[i].level1 = 0;
        } else {
            // WS2812 '0': 0.4us high, 0.8us low (4 ticks, 8 ticks @ 10MHz)
            symbols[i].duration0 = 4;
            symbols[i].level0 = 1;
            symbols[i].duration1 = 8;
            symbols[i].level1 = 0;
        }
    }

    // Reset code (>50us low): 2x 300 ticks (30us) at 10MHz = 60us low
    symbols[24].duration0 = 300;
    symbols[24].level0 = 0;
    symbols[24].duration1 = 300;
    symbols[24].level1 = 0;

    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    esp_err_t err = rmt_transmit(s_tx_chan, s_copy_encoder, symbols, sizeof(symbols), &tx_config);
    if (err == ESP_OK) {
        rmt_tx_wait_all_done(s_tx_chan, 10);
    }
}

static bool s_client_connected = false;

void rgb_led_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    ws2812_write(r, g, b);
}

void rgb_led_set_connected(bool connected)
{
    s_client_connected = connected;
}

void rgb_led_trigger_event(led_event_t event)
{
    if (s_event_queue) {
        xQueueSend(s_event_queue, &event, 0);
    }
}

static void rgb_led_anim_task(void *pvParameters)
{
    uint32_t event_ticks_remaining = 0;
    uint8_t active_r = 0, active_g = 0, active_b = 0;
    float phase = 0.0f;

    while (1) {
        led_event_t evt;
        // Drain incoming events with strict priority hierarchy
        while (xQueueReceive(s_event_queue, &evt, 0) == pdTRUE) {
            if (evt == LED_EVENT_BLE_CONNECT) {
                // BLE connection established: (0, 255, 140)
                active_r = 0;
                active_g = 255;
                active_b = 140;
                event_ticks_remaining = pdMS_TO_TICKS(350);
                break;
            } else if (evt == LED_EVENT_BLE_DISCONNECT) {
                // BLE connection terminated: (255, 20, 20)
                active_r = 255;
                active_g = 20;
                active_b = 20;
                event_ticks_remaining = pdMS_TO_TICKS(220);
                break;
            } else if (evt == LED_EVENT_WIFI_AUTH_ASSOC) {
                // Wi-Fi connection/auth attempt: (255, 10, 180)
                active_r = 255;
                active_g = 10;
                active_b = 180;
                event_ticks_remaining = pdMS_TO_TICKS(200);
                break;
            } else if (evt == LED_EVENT_WIFI_DEAUTH) {
                // Wi-Fi deauth/disassociation intercepted: (255, 60, 0)
                active_r = 255;
                active_g = 60;
                active_b = 0;
                event_ticks_remaining = pdMS_TO_TICKS(180);
                break;
            } else if (evt == LED_EVENT_BLE_GATT_READ && event_ticks_remaining < pdMS_TO_TICKS(100)) {
                // GATT characteristic read access: (255, 255, 255)
                active_r = 255;
                active_g = 255;
                active_b = 255;
                event_ticks_remaining = pdMS_TO_TICKS(90);
            } else if (evt == LED_EVENT_PROBE_RESPONSE && event_ticks_remaining < pdMS_TO_TICKS(100)) {
                // Directed probe response injected: (255, 140, 0)
                active_r = 255;
                active_g = 140;
                active_b = 0;
                event_ticks_remaining = pdMS_TO_TICKS(140);
            } else if (evt == LED_EVENT_WIFI_WILDCARD_SCAN && event_ticks_remaining < pdMS_TO_TICKS(60)) {
                // Wildcard 802.11 scan detected: (160, 40, 255)
                active_r = 160;
                active_g = 40;
                active_b = 255;
                event_ticks_remaining = pdMS_TO_TICKS(50);
            } else if (evt == LED_EVENT_BLE_ADV && event_ticks_remaining < pdMS_TO_TICKS(50)) {
                // BLE identity rotation: (0, 60, 255)
                active_r = 0;
                active_g = 60;
                active_b = 255;
                event_ticks_remaining = pdMS_TO_TICKS(90);
            } else if (evt == LED_EVENT_BEACON_BURST && event_ticks_remaining == 0) {
                // 802.11 beacon injection: (0, 210, 255)
                active_r = 0;
                active_g = 210;
                active_b = 255;
                event_ticks_remaining = pdMS_TO_TICKS(40);
            }
        }

        if (event_ticks_remaining > 0) {
            ws2812_write(active_r, active_g, active_b);
            const TickType_t step = pdMS_TO_TICKS(20);
            if (event_ticks_remaining > step) {
                event_ticks_remaining -= step;
            } else {
                event_ticks_remaining = 0;
            }
            vTaskDelay(step);
        } else {
            // Idle breathing heartbeat
            phase += 0.08f;
            if (phase >= 6.28318f) phase -= 6.28318f;
            float breath = (sinf(phase) + 1.0f) * 0.5f; // 0.0 to 1.0

            if (s_client_connected) {
                // Connected state heartbeat: peak (0, 160, 120)
                uint8_t idle_g = (uint8_t)(20 + 140.0f * breath);
                uint8_t idle_b = (uint8_t)(15 + 105.0f * breath);
                ws2812_write(0, idle_g, idle_b);
            } else {
                // Unconnected idle heartbeat: peak (0, 255, 20)
                uint8_t idle_g = (uint8_t)(40 + 215.0f * breath);
                uint8_t idle_b = (uint8_t)(5 + 25.0f * breath);
                ws2812_write(0, idle_g, idle_b);
            }
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
}

bool rgb_led_init(uint8_t brightness)
{
    s_brightness = brightness;

    ESP_LOGI(TAG, "Initializing WS2812 RGB LED on GPIO %d (Eye-safe Brightness: %u%%)...",
             RGB_LED_GPIO_PIN, s_brightness);

    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = RGB_LED_GPIO_PIN,
        .mem_block_symbols = 64,
        .resolution_hz = 10000000, // 10MHz -> 1 tick = 0.1us
        .trans_queue_depth = 4,
    };

    esp_err_t err = rmt_new_tx_channel(&tx_chan_config, &s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RMT TX channel: %s", esp_err_to_name(err));
        return false;
    }

    rmt_copy_encoder_config_t copy_encoder_config = {};
    err = rmt_new_copy_encoder(&copy_encoder_config, &s_copy_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RMT copy encoder: %s", esp_err_to_name(err));
        return false;
    }

    err = rmt_enable(s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable RMT channel: %s", esp_err_to_name(err));
        return false;
    }

    s_event_queue = xQueueCreate(16, sizeof(led_event_t));

    // Clear initial state
    ws2812_write(0, 0, 0);

    // Launch non-blocking animation task
    xTaskCreate(rgb_led_anim_task, "rgb_led_anim", 2048, NULL, 4, NULL);

    ESP_LOGI(TAG, "RGB LED driver active.");
    return true;
}

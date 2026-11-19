#include "rgb_led.h"
#include <string.h>
#include <math.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"

static const char *TAG = "probe_led";

static rmt_channel_handle_t s_tx_chan = NULL;
static rmt_encoder_handle_t s_copy_encoder = NULL;
static uint8_t s_brightness = 5; // Default to eye-safe 5% (PWM ~6/255 -> ~2.3% duty cycle)
static atomic_uint s_pending_events;   // one bit per led_event_t, set by any task
static volatile bool s_animating = false;
static volatile bool s_client_connected = false;

typedef struct {
    uint8_t rank; // 0 = no pulse; a running pulse yields only to the same or a higher rank
    uint8_t r, g, b;
    uint16_t duration_ms;
} led_pulse_t;

static const led_pulse_t s_pulses[LED_EVENT_COUNT] = {
    [LED_EVENT_BEACON_BURST]       = { 1,   0, 210, 255,  40 },
    [LED_EVENT_BLE_ADV]            = { 2,   0,  60, 255,  90 },
    [LED_EVENT_WIFI_WILDCARD_SCAN] = { 3, 160,  40, 255,  80 },
    [LED_EVENT_PROBE_RESPONSE]     = { 4, 255, 140,   0, 140 },
    [LED_EVENT_BLE_GATT_READ]      = { 5, 255, 255, 255,  90 },
    [LED_EVENT_WIFI_DEAUTH]        = { 6, 255,  60,   0, 180 },
    [LED_EVENT_WIFI_AUTH_ASSOC]    = { 7, 255,  10, 180, 200 },
    [LED_EVENT_BLE_DISCONNECT]     = { 8, 255,  20,  20, 220 },
    [LED_EVENT_BLE_CONNECT]        = { 9,   0, 255, 140, 350 },
};

// Only the animation task (and rgb_led_init before that task exists) may call this.
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

        uint32_t scaled_r = ((uint32_t)r * max_pwm) / 255;
        uint32_t scaled_g = ((uint32_t)g * max_pwm) / 255;
        uint32_t scaled_b = ((uint32_t)b * max_pwm) / 255;

        r = (r > 0 && scaled_r == 0) ? 1 : (uint8_t)scaled_r;
        g = (g > 0 && scaled_g == 0) ? 1 : (uint8_t)scaled_g;
        b = (b > 0 && scaled_b == 0) ? 1 : (uint8_t)scaled_b;
    }

    // WS2812 expects GRB order (Green, Red, Blue)
    uint32_t grb = ((uint32_t)g << 16) | ((uint32_t)r << 8) | (uint32_t)b;

    // rmt_transmit is asynchronous, so the symbols must outlive this call
    static rmt_symbol_word_t symbols[25];
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

void rgb_led_set_connected(bool connected)
{
    s_client_connected = connected;
}

void rgb_led_trigger_event(led_event_t event)
{
    if (s_animating && (unsigned int)event < LED_EVENT_COUNT) {
        atomic_fetch_or(&s_pending_events, 1u << event);
    }
}

static void rgb_led_anim_task(void *pvParameters)
{
    uint32_t event_ticks_remaining = 0;
    uint8_t active_rank = 0;
    uint8_t active_r = 0, active_g = 0, active_b = 0;
    float phase = 0.0f;

    while (1) {
        // Take the events raised since the last pass; the highest rank wins
        unsigned int pending = atomic_exchange(&s_pending_events, 0);
        for (unsigned int evt = 0; evt < LED_EVENT_COUNT; evt++) {
            const led_pulse_t *pulse = &s_pulses[evt];
            if (!(pending & (1u << evt)) || pulse->rank == 0 || pulse->rank < active_rank) {
                continue;
            }
            active_rank = pulse->rank;
            active_r = pulse->r;
            active_g = pulse->g;
            active_b = pulse->b;
            event_ticks_remaining = pdMS_TO_TICKS(pulse->duration_ms);
        }

        if (event_ticks_remaining > 0) {
            ws2812_write(active_r, active_g, active_b);
            const TickType_t step = pdMS_TO_TICKS(20);
            if (event_ticks_remaining > step) {
                event_ticks_remaining -= step;
            } else {
                event_ticks_remaining = 0;
                active_rank = 0;
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

bool rgb_led_init(uint8_t brightness, bool enabled)
{
    if (brightness > 100) brightness = 100;
    s_brightness = brightness;
    if (brightness == 0) enabled = false;

    if (enabled) {
        ESP_LOGI(TAG, "Initializing WS2812 RGB LED on GPIO %d (Eye-safe Brightness: %u%%)...",
                 RGB_LED_GPIO_PIN, s_brightness);
    } else {
        ESP_LOGI(TAG, "WS2812 RGB LED on GPIO %d disabled; switching it off.", RGB_LED_GPIO_PIN);
    }

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

    // Clear initial state. The channel stays enabled so the data line is held low.
    ws2812_write(0, 0, 0);
    if (!enabled) {
        return true;
    }

    // Launch non-blocking animation task
    if (xTaskCreate(rgb_led_anim_task, "rgb_led_anim", 2048, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create the LED animation task");
        return false;
    }
    s_animating = true;

    ESP_LOGI(TAG, "RGB LED driver active.");
    return true;
}

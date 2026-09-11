#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RGB_LED_GPIO_PIN 8

/**
 * @brief Telemetry events mapped to distinct optical indicators on the WS2812 RGB LED.
 */
typedef enum {
    LED_EVENT_IDLE,                 // Idle heartbeat pulse (unconnected / client paired)
    LED_EVENT_BEACON_BURST,          // 802.11 raw beacon burst injection pulse
    LED_EVENT_PROBE_RESPONSE,        // Active client probe response dispatched
    LED_EVENT_WIFI_WILDCARD_SCAN,    // External device active 802.11 probe scan detected
    LED_EVENT_WIFI_AUTH_ASSOC,       // Client Wi-Fi connection/authentication attempt
    LED_EVENT_WIFI_DEAUTH,           // Wi-Fi Deauthentication/Disassociation frame intercepted
    LED_EVENT_BLE_ADV,               // BLE virtual peripheral identity rotation
    LED_EVENT_BLE_CONNECT,           // BLE client connection established
    LED_EVENT_BLE_DISCONNECT,        // BLE client connection terminated
    LED_EVENT_BLE_GATT_READ,         // Client GATT characteristic read access
} led_event_t;

/**
 * @brief Initializes the hardware RMT peripheral on GPIO8 to drive the onboard WS2812 RGB LED.
 *        Applies eye-safe logarithmic PWM scaling to prevent glare and retina fatigue.
 *
 * @param brightness Max brightness percentage (1 to 100, default 5%).
 * @return true on success, false on failure.
 */
bool rgb_led_init(uint8_t brightness);

/**
 * @brief Directly sets the RGB LED raw channel values before eye-safe scaling.
 *
 * @param r Channel R value (0-255).
 * @param g Channel G value (0-255).
 * @param b Channel B value (0-255).
 */
void rgb_led_set_color(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Updates client connection state (switches idle breathing pattern when connected).
 *
 * @param connected true if external client is connected, false if idle.
 */
void rgb_led_set_connected(bool connected);

/**
 * @brief Triggers an asynchronous, non-blocking visual telemetry event on the RGB LED.
 *        Higher-priority RF events preempt the idle heartbeat animation.
 *
 * @param event Event type to display.
 */
void rgb_led_trigger_event(led_event_t event);

#ifdef __cplusplus
}
#endif

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the status LED (WS2812 on GPIO48). */
esp_err_t status_led_init(void);

/** Breathing blue — connecting to WiFi. */
void status_led_wifi_connecting(void);

/** Solid green — WiFi connected. */
void status_led_wifi_connected(void);

/** Slow yellow blink — AP config mode. */
void status_led_ap_mode(void);

/** Purple pulse — ESP32 OTA in progress (0-100). */
void status_led_ota_esp32(int progress_pct);

/** Orange pulse — STM32 OTA in progress (0-100). */
void status_led_ota_stm32(int progress_pct);

/** Fast red blink — error occurred. */
void status_led_error(void);

#ifdef __cplusplus
}
#endif
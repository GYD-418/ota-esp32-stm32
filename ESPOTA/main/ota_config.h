#pragma once

/* ---- Auto-OTA Configuration ---- */

/* ---- STM32 OTA ---- */
#define STM32_OTA_BASE_URL   "http://esp32-ota-t.oss-cn-beijing.aliyuncs.com/STM32_OTA"
#define STM32_VERSION_URL    STM32_OTA_BASE_URL "/version.txt"

/* ---- ESP32 self OTA ---- */
#define ESP32_OTA_BASE_URL   "http://esp32-ota-t.oss-cn-beijing.aliyuncs.com/ESP32_OTA"
#define ESP32_VERSION_URL    ESP32_OTA_BASE_URL "/version.txt"

/* Enable auto-check on boot (1=on, 0=off) */
#define OTA_AUTO_CHECK  1

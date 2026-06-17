#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Pin configuration (ESP32-S3 → STM32F407) ---- */
#define STM32_UART_PORT      UART_NUM_1
#define STM32_UART_TX        17
#define STM32_UART_RX        18

#define STM32_SPI_HOST       SPI2_HOST
#define STM32_SPI_SCK        10
#define STM32_SPI_MOSI       11
#define STM32_SPI_MISO       12
#define STM32_SPI_CS         13
#define STM32_RST_PIN        14

#define STM32_SPI_BLOCK_SIZE 256
#define STM32_UART_BAUDRATE  115200

/* ---- Forwarder events ---- */
typedef enum {
    FORWARDER_EVENT_PROGRESS,
    FORWARDER_EVENT_COMPLETED,
    FORWARDER_EVENT_FAILED,
} forwarder_event_t;

typedef struct {
    int progress_pct;
    int sent_bytes;
    int total_bytes;
    char error[128];
} forwarder_progress_t;

typedef void (*forwarder_callback_t)(forwarder_event_t event, const forwarder_progress_t *p);

/* ---- API ---- */
esp_err_t stm32_forwarder_init(void);
esp_err_t stm32_forwarder_start(const char *firmware_url, forwarder_callback_t cb);
bool stm32_forwarder_is_running(void);
void stm32_forwarder_abort(void);

#ifdef __cplusplus
}
#endif
#include "stm32_forwarder.h"
#include "driver/gpio.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "status_led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "stm32_fwd";

/* ---- UART frame protocol (mirrors STM32 ota_receiver.h) ---- */
#define UART_HEADER1  0xAA
#define UART_HEADER2  0x55
#define CMD_START     0x01
#define CMD_DATA      0x02
#define CMD_VERIFY    0x03
#define CMD_ABORT     0x04
#define CMD_COMPLETE  0x05
#define CMD_ERROR     0xFE

/* ---- SPI block protocol ---- */
#define SPI_BLOCK_MAGIC   0xA5
#define SPI_BLOCK_HEADER  5
#define SPI_BLOCK_FOOTER  2
#define SPI_PKT_SIZE      (SPI_BLOCK_HEADER + STM32_SPI_BLOCK_SIZE + SPI_BLOCK_FOOTER)

/* ---- Internal state ---- */
static spi_device_handle_t s_spi;
static forwarder_callback_t s_callback;
static volatile bool s_running = false;
static volatile bool s_abort = false;

/* ========================================================================
 *  CRC16 (CCITT — matches STM32)
 * ======================================================================== */
static uint16_t crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else              crc <<= 1;
        }
    }
    return crc;
}

/* ========================================================================
 *  UART helpers
 * ======================================================================== */
static void uart_send_frame(uint8_t cmd, const uint8_t *data, uint16_t len)
{
    uint8_t frame[64];
    frame[0] = UART_HEADER1;
    frame[1] = UART_HEADER2;
    frame[2] = cmd;
    frame[3] = (len >> 8) & 0xFF;
    frame[4] = len & 0xFF;
    if (data && len) memcpy(&frame[5], data, len);
    uint16_t crc = crc16(frame, 5 + len);
    frame[5 + len] = (crc >> 8) & 0xFF;
    frame[6 + len] = crc & 0xFF;
    uart_write_bytes(STM32_UART_PORT, (const char *)frame, 7 + len);
}

static esp_err_t uart_wait_response(uint8_t expected_cmd, int timeout_ms)
{
    uint8_t buf[64];
    int total = 0;
    int64_t start = esp_timer_get_time() / 1000;

    while ((esp_timer_get_time() / 1000 - start) < timeout_ms) {
        int len = uart_read_bytes(STM32_UART_PORT, buf + total,
                                   sizeof(buf) - total, pdMS_TO_TICKS(50));
        if (len > 0) {
            char hex[200] = {0};
            int pos = 0;
            for (int k = 0; k < len && pos < 190; k++) {
                pos += sprintf(hex + pos, "%02X ", buf[total + k]);
            }
            ESP_LOGI(TAG, "UART RX %d: %s", len, hex);
        }
        total += len;
        vTaskDelay(1);  /* yield to prevent task watchdog */

        /* Find frame start */
        int frame_start = -1;
        for (int i = 0; i <= total - 7; i++) {
            if (buf[i] == UART_HEADER1 && buf[i+1] == UART_HEADER2) {
                uint16_t dlen = ((uint16_t)buf[i+3] << 8) | buf[i+4];
                int flen = 5 + dlen + 2;
                if (total - i >= flen) {
                    /* Verify CRC */
                    uint16_t fcrc = ((uint16_t)buf[i+5+dlen] << 8) | buf[i+5+dlen+1];
                    if (crc16(buf + i, 5 + dlen) == fcrc) {
                        if (buf[i+2] == expected_cmd) return ESP_OK;
                        if (buf[i+2] == CMD_ERROR)    return ESP_FAIL;
                    }
                    /* Shift remaining */
                    memmove(buf, buf + i + flen, total - i - flen);
                    total -= (i + flen);
                    i = -1;
                } else {
                    frame_start = i;
                    break;
                }
            }
        }
        if (frame_start > 0) {
            memmove(buf, buf + frame_start, total - frame_start);
            total -= frame_start;
        }
    }
    return ESP_ERR_TIMEOUT;
}

/* ========================================================================
 *  SPI send block
 * ======================================================================== */
static esp_err_t spi_send_block(uint16_t index, const uint8_t *data, uint16_t size)
{
    uint8_t pkt[SPI_PKT_SIZE];
    pkt[0] = SPI_BLOCK_MAGIC;
    pkt[1] = (index >> 8) & 0xFF;
    pkt[2] = index & 0xFF;
    pkt[3] = (size >> 8) & 0xFF;
    pkt[4] = size & 0xFF;
    memcpy(&pkt[SPI_BLOCK_HEADER], data, size);

    uint16_t crc = crc16(data, size);
    pkt[SPI_BLOCK_HEADER + size] = (crc >> 8) & 0xFF;
    pkt[SPI_BLOCK_HEADER + size + 1] = crc & 0xFF;

    /* Always send full packet size so STM32 DMA completes */
    int total = SPI_PKT_SIZE;

    spi_transaction_t t = {
        .length = total * 8,
        .tx_buffer = pkt,
        .rx_buffer = NULL,
    };
    return spi_device_transmit(s_spi, &t);
}

/* ========================================================================
 *  Forwarder task
 * ======================================================================== */
static void forwarder_task(void *arg)
{
    char *url = (char *)arg;
    esp_err_t ret;

    ESP_LOGI(TAG, "Downloading STM32 firmware: %s", url);

    /* ---- HTTP download ---- */
    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 30000,
        .buffer_size = 4096,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t http = esp_http_client_init(&http_cfg);
    if (!http) {
        if (s_callback) {
            forwarder_progress_t p = {.error = "HTTP init failed"};
            s_callback(FORWARDER_EVENT_FAILED, &p);
        }
        s_running = false;
        goto cleanup;
    }

    ret = esp_http_client_open(http, 0);
    if (ret != ESP_OK) {
        esp_http_client_cleanup(http);
        if (s_callback) {
            forwarder_progress_t p = {.error = "HTTP open failed"};
            s_callback(FORWARDER_EVENT_FAILED, &p);
        }
        s_running = false;
        goto cleanup;
    }

    int content_len = esp_http_client_fetch_headers(http);
    if (content_len <= 0) content_len = esp_http_client_get_content_length(http);
    ESP_LOGI(TAG, "Firmware size: %d bytes", content_len);

    if (content_len <= 0) {
        esp_http_client_close(http);
        esp_http_client_cleanup(http);
        if (s_callback) {
            forwarder_progress_t p = {.error = "Invalid firmware size"};
            s_callback(FORWARDER_EVENT_FAILED, &p);
        }
        s_running = false;
        goto cleanup;
    }

    /* ---- Send OTA_START to STM32 ---- */
    uint8_t start_data[12];
    start_data[0] = (content_len >> 24) & 0xFF;
    start_data[1] = (content_len >> 16) & 0xFF;
    start_data[2] = (content_len >> 8)  & 0xFF;
    start_data[3] = content_len & 0xFF;

    /* CRC32 placeholder — STM32 computes actual CRC on verify */
    start_data[4] = start_data[5] = start_data[6] = start_data[7] = 0;

    /* Version placeholder */
    start_data[8] = start_data[9] = start_data[10] = start_data[11] = 0;

    uart_flush_input(STM32_UART_PORT);
    uart_send_frame(CMD_START, start_data, 12);
    ESP_LOGI(TAG, "Sent CMD_START, waiting for ACK...");

    if (uart_wait_response(CMD_START, 5000) != ESP_OK) {
        ESP_LOGE(TAG, "STM32 did not ACK START (timeout or bad response)");
        esp_http_client_close(http);
        esp_http_client_cleanup(http);
        if (s_callback) {
            forwarder_progress_t p = {.error = "STM32 did not ACK START"};
            s_callback(FORWARDER_EVENT_FAILED, &p);
        }
        s_running = false;
        goto cleanup;
    }
    ESP_LOGI(TAG, "STM32 OTA started, waiting for Flash erase...");
    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG, "Sending blocks...");
    uint8_t block_buf[STM32_SPI_BLOCK_SIZE];
    int read_total = 0;
    int block_idx = 0;

    /* Compute CRC32 while streaming */
    uint32_t running_crc32 = 0xFFFFFFFF;

    while (read_total < content_len && !s_abort) {
        int to_read = STM32_SPI_BLOCK_SIZE;
        if (read_total + to_read > content_len) {
            to_read = content_len - read_total;
        }
        if (to_read > STM32_SPI_BLOCK_SIZE) to_read = STM32_SPI_BLOCK_SIZE;
        memset(block_buf, 0xFF, STM32_SPI_BLOCK_SIZE);

        int r = esp_http_client_read(http, (char *)block_buf, to_read);
        if (r <= 0) break;

        /* Pad with 0xFF up to SPI_BLOCK_SIZE for STM32 DMA */
        int padded = (r < STM32_SPI_BLOCK_SIZE) ? STM32_SPI_BLOCK_SIZE : r;

        read_total += r;

        /* Debug: print first block's first 16 bytes */
        if (block_idx == 0) {
            char hex[100] = {0}; int p = 0;
            for (int k = 0; k < 16 && k < r; k++)
                p += sprintf(hex + p, "%02X ", block_buf[k]);
            ESP_LOGI(TAG, "Block0[0..15]: %s", hex);
        }

        /* Update CRC32 (only over actual data, not padding) */
        for (int i = 0; i < r; i++) {
            running_crc32 ^= block_buf[i];
            for (int j = 0; j < 8; j++) {
                if (running_crc32 & 1) running_crc32 = (running_crc32 >> 1) ^ 0xEDB88320;
                else                    running_crc32 >>= 1;
            }
        }

        /* Send block over SPI */
        ret = spi_send_block(block_idx, block_buf, r);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPI send failed at block %d", block_idx);
            break;
        }

        /* Wait for ACK (15s timeout for slow STM32 processing) */
        ret = uart_wait_response(CMD_DATA, 15000);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "No ACK for block %d", block_idx);
            break;
        }

        block_idx++;

        /* Progress callback */
        if (s_callback && (block_idx % 10 == 0 || read_total >= content_len)) {
            forwarder_progress_t p = {
                .progress_pct = read_total * 100 / content_len,
                .sent_bytes = read_total,
                .total_bytes = content_len,
            };
            s_callback(FORWARDER_EVENT_PROGRESS, &p);
        }
    }

    esp_http_client_close(http);
    esp_http_client_cleanup(http);

    /* Finalize CRC32 */
    running_crc32 = ~running_crc32;

    if (s_abort || read_total < content_len) {
        uart_send_frame(CMD_ABORT, NULL, 0);
        if (s_callback) {
            forwarder_progress_t p = {.error = "Transfer aborted or incomplete"};
            s_callback(FORWARDER_EVENT_FAILED, &p);
        }
        s_running = false;
        goto cleanup;
    }

    /* ---- Send CRC32 + VERIFY ---- */
    ESP_LOGI(TAG, "All %d blocks sent. CRC32=0x%08lX. Sending VERIFY.",
             block_idx, running_crc32);

    uint8_t verify_data[8];
    verify_data[0] = (content_len >> 24) & 0xFF;
    verify_data[1] = (content_len >> 16) & 0xFF;
    verify_data[2] = (content_len >> 8)  & 0xFF;
    verify_data[3] = content_len & 0xFF;
    verify_data[4] = (running_crc32 >> 24) & 0xFF;
    verify_data[5] = (running_crc32 >> 16) & 0xFF;
    verify_data[6] = (running_crc32 >> 8)  & 0xFF;
    verify_data[7] = running_crc32 & 0xFF;

    uart_send_frame(CMD_VERIFY, verify_data, 8);

    /* Wait for COMPLETE (STM32 will reset after sending this) */
    /* Flush any stale data first */
    uart_flush_input(STM32_UART_PORT);
    if (uart_wait_response(CMD_COMPLETE, 15000) == ESP_OK) {
        ESP_LOGI(TAG, "STM32 OTA successful! STM32 is rebooting...");

        /* Hardware reset: pulse RST pin LOW for 10ms */
        gpio_set_level(STM32_RST_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(STM32_RST_PIN, 1);

        if (s_callback) {
            forwarder_progress_t p = {.progress_pct = 100};
            s_callback(FORWARDER_EVENT_COMPLETED, &p);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    } else {
        if (s_callback) {
            forwarder_progress_t p = {.error = "VERIFY failed"};
            s_callback(FORWARDER_EVENT_FAILED, &p);
        }
    }

    s_running = false;
    status_led_wifi_connected();  /* restore LED */

cleanup:
    free(url);
    vTaskDelete(NULL);
}

/* ========================================================================
 *  Public API
 * ======================================================================== */
esp_err_t stm32_forwarder_init(void)
{
    /* ---- UART init ---- */
    uart_config_t uart_cfg = {
        .baud_rate = STM32_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(STM32_UART_PORT, &uart_cfg);
    uart_set_pin(STM32_UART_PORT, STM32_UART_TX, STM32_UART_RX,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(STM32_UART_PORT, 1024, 0, 0, NULL, 0);

    /* ---- RST pin (keep HIGH, pulse LOW to reset STM32) ---- */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << STM32_RST_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(STM32_RST_PIN, 1);  /* HIGH = not reset */


    /* ---- SPI Master init ---- */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = STM32_SPI_MOSI,
        .miso_io_num = STM32_SPI_MISO,
        .sclk_io_num = STM32_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    spi_bus_initialize(STM32_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 8000000,  /* 8 MHz */
        .mode = 0,                   /* CPOL=0, CPHA=0 */
        .spics_io_num = STM32_SPI_CS,
        .queue_size = 4,
    };
    spi_bus_add_device(STM32_SPI_HOST, &dev_cfg, &s_spi);

    ESP_LOGI(TAG, "STM32 forwarder initialized (UART%d + SPI%d)",
             STM32_UART_PORT, STM32_SPI_HOST);
    return ESP_OK;
}

esp_err_t stm32_forwarder_start(const char *url, forwarder_callback_t cb)
{
    if (s_running) return ESP_ERR_INVALID_STATE;
    if (!url || !*url) return ESP_ERR_INVALID_ARG;

    s_callback = cb;
    s_running = true;
    s_abort = false;

    char *url_copy = strdup(url);
    xTaskCreate(forwarder_task, "stm32_fwd", 8192, url_copy, 5, NULL);
    return ESP_OK;
}

bool stm32_forwarder_is_running(void)
{
    return s_running;
}

void stm32_forwarder_abort(void)
{
    s_abort = true;
}
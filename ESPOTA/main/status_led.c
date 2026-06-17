#include "status_led.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip_encoder.h"

#define WS2812_GPIO       48
#define RMT_RESOLUTION_HZ 10000000
#define LED_TASK_PERIOD_MS 20

typedef enum {
    LED_STATE_OFF = 0,
    LED_STATE_WIFI_CONNECTING,
    LED_STATE_WIFI_CONNECTED,
    LED_STATE_AP_MODE,
    LED_STATE_OTA_ESP32,
    LED_STATE_OTA_STM32,
    LED_STATE_ERROR,
} led_state_t;

static const char *TAG = "status_led";
static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_encoder;
static led_state_t s_state = LED_STATE_OFF;
static int s_progress = 0;
static TaskHandle_t s_task = NULL;

/* ========================================================================
 *  Set WS2812 color
 * ======================================================================== */
static void ws2812_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    /* WS2812 GRB order */
    uint8_t pixels[3] = { g, r, b };
    rmt_transmit_config_t tx = { .loop_count = 0 };
    rmt_transmit(s_chan, s_encoder, pixels, sizeof(pixels), &tx);
    rmt_tx_wait_all_done(s_chan, portMAX_DELAY);
}

/* ========================================================================
 *  LED task — runs animations based on state
 * ======================================================================== */
static void led_task(void *arg)
{
    uint32_t tick = 0;
    led_state_t prev_state = LED_STATE_OFF;

    while (1) {
        led_state_t state = s_state;
        int progress = s_progress;

        /* State transition — reset tick */
        if (state != prev_state) {
            tick = 0;
            prev_state = state;
        }

        switch (state) {
        case LED_STATE_OFF:
            ws2812_set_color(0, 0, 0);
            break;

        case LED_STATE_WIFI_CONNECTING: {
            /* Blue breathing */
            uint8_t b = (uint8_t)((tick % 100) < 50 ?
                        (tick % 50) * 5 : (50 - (tick % 50)) * 5);
            ws2812_set_color(0, 0, b);
            break;
        }

        case LED_STATE_WIFI_CONNECTED:
            ws2812_set_color(0, 64, 0);  /* dim green */
            break;

        case LED_STATE_AP_MODE: {
            /* Yellow slow blink: 500ms on, 500ms off */
            if ((tick % 50) < 25)
                ws2812_set_color(64, 48, 0);
            else
                ws2812_set_color(0, 0, 0);
            break;
        }

        case LED_STATE_OTA_ESP32: {
            /* Purple pulse at progress speed */
            uint8_t v = (uint8_t)((tick % 40) < 20 ?
                        (tick % 20) * 6 : (20 - (tick % 20)) * 6);
            ws2812_set_color(v, 0, v);
            break;
        }

        case LED_STATE_OTA_STM32: {
            /* Orange pulse at progress speed */
            uint8_t v = (uint8_t)((tick % 40) < 20 ?
                        (tick % 20) * 6 : (20 - (tick % 20)) * 6);
            ws2812_set_color(v, v / 3, 0);
            break;
        }

        case LED_STATE_ERROR: {
            /* Red fast blink (WS2812 is GRB, so set G=128 for red) */
            if ((tick % 20) < 10)
                ws2812_set_color(128, 0, 0);
            else
                ws2812_set_color(0, 0, 0);
            break;
        }
        }

        tick++;
        vTaskDelay(pdMS_TO_TICKS(LED_TASK_PERIOD_MS));
    }
}

/* ========================================================================
 *  Public API
 * ======================================================================== */
esp_err_t status_led_init(void)
{
    rmt_tx_channel_config_t tx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = WS2812_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_cfg, &s_chan));

    led_strip_encoder_config_t enc_cfg = { .resolution = RMT_RESOLUTION_HZ };
    ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&enc_cfg, &s_encoder));
    ESP_ERROR_CHECK(rmt_enable(s_chan));

    xTaskCreate(led_task, "led_task", 3072, NULL, 2, &s_task);
    ESP_LOGI(TAG, "Status LED initialized on GPIO%d", WS2812_GPIO);
    return ESP_OK;
}

void status_led_wifi_connecting(void) { s_state = LED_STATE_WIFI_CONNECTING; }
void status_led_wifi_connected(void)  { s_state = LED_STATE_WIFI_CONNECTED; }
void status_led_ap_mode(void)         { s_state = LED_STATE_AP_MODE; }
void status_led_error(void)           { s_state = LED_STATE_ERROR; }

void status_led_ota_esp32(int pct)    { s_state = LED_STATE_OTA_ESP32; s_progress = pct; }
void status_led_ota_stm32(int pct)    { s_state = LED_STATE_OTA_STM32; s_progress = pct; }
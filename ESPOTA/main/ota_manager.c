#include "ota_manager.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ota_mgr";

#define OTA_TASK_STACK_SIZE  8192
#define OTA_RECV_TIMEOUT_MS  10000

static ota_callback_t s_callback = NULL;
static volatile bool s_running = false;
static char s_pending_version[32] = {0};

/* ========================================================================
 *  Internal: HTTP event handler for progress tracking
 * ======================================================================== */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    static int last_pct = -1;

    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGE(TAG, "HTTP error");
        break;

    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGI(TAG, "HTTP connected");
        break;

    case HTTP_EVENT_HEADER_SENT:
        break;

    case HTTP_EVENT_ON_HEADER: {
        /* Extract content-length from headers */
        char *cl = NULL;
        esp_http_client_get_header(evt->client, "Content-Length", &cl);
        if (cl) {
            int total = atoi(cl);
            ESP_LOGI(TAG, "Firmware size: %d bytes", total);
            if (s_callback) {
                ota_progress_t p = {.total_bytes = total};
                s_callback(OTA_EVENT_STARTED, &p);
            }
        }
        break;
    }

    case HTTP_EVENT_ON_DATA: {
        static int downloaded = 0;
        downloaded += evt->data_len;

        int pct = 0;
        int total = (int)esp_http_client_get_content_length(evt->client);
        if (total > 0) {
            pct = downloaded * 100 / total;
        }

        if (pct != last_pct && s_callback) {
            last_pct = pct;
            ota_progress_t p = {
                .progress_pct = pct,
                .downloaded_bytes = downloaded,
                .total_bytes = total,
            };
            s_callback(OTA_EVENT_PROGRESS, &p);
        }
        break;
    }

    case HTTP_EVENT_ON_FINISH:
        ESP_LOGI(TAG, "HTTP download finished");
        break;

    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HTTP disconnected");
        if (s_callback) {
            ota_progress_t p = {.progress_pct = 100};
            s_callback(OTA_EVENT_COMPLETED, &p);
        }
        break;

    default:
        break;
    }
    return ESP_OK;
}

/* ========================================================================
 *  Internal: OTA task — runs the download + verify + apply
 * ======================================================================== */
static void ota_task(void *arg)
{
    char *url = (char *)arg;
    esp_err_t ret;

    ESP_LOGI(TAG, "OTA task starting for: %s", url);

    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = OTA_RECV_TIMEOUT_MS,
        .buffer_size_tx = 1024,
        .buffer_size = 1024,
        .skip_cert_common_name_check = true,
        .event_handler = http_event_handler,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    ESP_LOGI(TAG, "Starting OTA download...");
    ret = esp_https_ota(&ota_config);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA successful! Restarting in 2 seconds...");

        /* Read the new app description for version tracking */
        const esp_partition_t *running = esp_ota_get_running_partition();
        const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
        if (running && next && running != next) {
            /* new firmware was written to the other partition */
            esp_app_desc_t new_desc;
            if (esp_ota_get_partition_description(next, &new_desc) == ESP_OK) {
                strncpy(s_pending_version, new_desc.version, sizeof(s_pending_version) - 1);
                ESP_LOGI(TAG, "New version: %s", s_pending_version);
            }
        }

        if (s_callback) {
            ota_progress_t p = {.progress_pct = 100};
            s_callback(OTA_EVENT_COMPLETED, &p);
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
        if (s_callback) {
            ota_progress_t p = {0};
            snprintf(p.error_msg, sizeof(p.error_msg), "OTA error: %s", esp_err_to_name(ret));
            s_callback(OTA_EVENT_FAILED, &p);
        }
    }

    s_running = false;
    free(url);
    vTaskDelete(NULL);
}

/* ========================================================================
 *  Public API
 * ======================================================================== */
esp_err_t ota_manager_init(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    if (!desc) {
        ESP_LOGE(TAG, "Failed to get app description");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Current firmware version: %s", desc->version);
    ESP_LOGI(TAG, "Project: %s", desc->project_name);

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        ESP_LOGI(TAG, "Running from: %s (0x%lx, size %lu)",
                 running->label, running->address, running->size);
    }

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (update) {
        ESP_LOGI(TAG, "OTA target: %s (0x%lx, size %lu)",
                 update->label, update->address, update->size);
    }

    return ESP_OK;
}

esp_err_t ota_manager_start(const char *url, ota_callback_t cb)
{
    if (s_running) {
        ESP_LOGW(TAG, "OTA already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    if (!url || strlen(url) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    s_callback = cb;
    s_running = true;

    char *url_copy = strdup(url);
    if (!url_copy) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreate(ota_task, "ota_task", OTA_TASK_STACK_SIZE,
                                  url_copy, 5, NULL);
    if (ret != pdPASS) {
        free(url_copy);
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool ota_manager_is_running(void)
{
    return s_running;
}

const char *ota_manager_get_version(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    return desc ? desc->version : "unknown";
}

const char *ota_manager_get_pending_version(void)
{
    if (s_pending_version[0] == '\0') {
        return NULL;
    }
    return s_pending_version;
}
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "ota_config.h"
#include "ota_manager.h"
#include "status_led.h"
#include "stm32_forwarder.h"
#include "wifi_provisioning/ssid_manager.h"
#include "wifi_provisioning/wifi_config_ap.h"

#define AP_SSID_PREFIX          "ESP32-OTA"

static const char *TAG = "espota";

/* ---------- netif ---------- */
static esp_netif_t *s_sta_netif = NULL;

/* ---------- event group bits ---------- */
#define STA_STARTED_BIT   BIT0
#define STA_CONNECTED_BIT BIT1
#define STA_FAIL_BIT      BIT2
#define SCAN_DONE_BIT     BIT3

static EventGroupHandle_t s_sta_evt_group;
static httpd_handle_t s_ota_server = NULL;

/* ---------- EMBED_TXTFILES symbols ---------- */
extern const char dashboard_html_start[] asm("_binary_dashboard_html_start");

/* ========================================================================
 *  OTA HTTP handlers
 * ======================================================================== */

/* GET /api/version */
static esp_err_t version_handler(httpd_req_t *req)
{
    char json[128];
    const char *ver = ota_manager_get_version();
    const char *pending = ota_manager_get_pending_version();
    snprintf(json, sizeof(json),
             "{\"version\":\"%s\",\"pending\":%s}",
             ver, pending ? pending : "null");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

/* GET /api/ota/status */
static esp_err_t ota_status_handler(httpd_req_t *req)
{
    char json[64];
    snprintf(json, sizeof(json), "{\"running\":%s}",
             ota_manager_is_running() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

/* GET /  — OTA dashboard */
static esp_err_t dashboard_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, dashboard_html_start, strlen(dashboard_html_start));
    return ESP_OK;
}

/* POST /api/stm32/update  — body: {"url": "http://..."} */
static esp_err_t stm32_update_handler(httpd_req_t *req)
{
    char buf[512] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    buf[ret] = '\0';

    char url[256] = {0};
    char *url_key = strstr(buf, "\"url\"");
    if (url_key) {
        char *colon = strchr(url_key + 5, ':');
        if (colon) {
            char *start = strchr(colon + 1, '"');
            if (start) {
                start++;
                char *end = strchr(start, '"');
                if (end) {
                    int len = (end - start < 255) ? (int)(end - start) : 255;
                    strncpy(url, start, len);
                }
            }
        }
    }

    if (strlen(url) == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_send(req, "{\"success\":false,\"error\":\"URL required\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (stm32_forwarder_is_running()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_send(req, "{\"success\":false,\"error\":\"STM32 OTA already running\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "STM32 OTA triggered: %s", url);
    status_led_ota_stm32(0);

    esp_err_t err = stm32_forwarder_start(url, NULL);
    if (err != ESP_OK) {
        status_led_error();
    }
    /* LED restores to connected when forwarder_task exits */
    ESP_LOGI(TAG, "STM32 OTA start result: %s", err == ESP_OK ? "OK" : esp_err_to_name(err));
    if (err != ESP_OK) {
        char resp[128];
        snprintf(resp, sizeof(resp), "{\"success\":false,\"error\":\"%s\"}",
                 esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_send(req, resp, strlen(resp));
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* GET /api/stm32/status */
static esp_err_t stm32_status_handler(httpd_req_t *req)
{
    char json[64];
    snprintf(json, sizeof(json), "{\"running\":%s}",
             stm32_forwarder_is_running() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

/* POST /api/ota/update  — body: {"url": "http://..."} */
static esp_err_t ota_update_handler(httpd_req_t *req)
{
    char buf[512] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char url[256] = {0};

    /* parse "url" from JSON */
    char *url_key = strstr(buf, "\"url\"");
    if (url_key) {
        char *colon = strchr(url_key + 5, ':');
        if (colon) {
            char *start = strchr(colon + 1, '"');
            if (start) {
                start++;
                char *end = strchr(start, '"');
                if (end) {
                    int len = end - start;
                    if (len > (int)sizeof(url) - 1) len = sizeof(url) - 1;
                    strncpy(url, start, len);
                }
            }
        }
    }

    if (strlen(url) == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_send(req, "{\"success\":false,\"error\":\"URL required\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (ota_manager_is_running()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_send(req, "{\"success\":false,\"error\":\"OTA already running\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA update triggered: %s", url);
    status_led_ota_esp32(0);

    esp_err_t err = ota_manager_start(url, NULL);
    if (err != ESP_OK) {
        char resp[128];
        snprintf(resp, sizeof(resp), "{\"success\":false,\"error\":\"%s\"}",
                 esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_send(req, resp, strlen(resp));
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ========================================================================
 *  Start OTA management HTTP server on STA interface
 * ======================================================================== */
static void start_ota_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 6;
    config.stack_size = 6144;

    if (httpd_start(&s_ota_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start OTA server");
        return;
    }

    httpd_uri_t uri_dashboard = {
        .uri = "/", .method = HTTP_GET,
        .handler = dashboard_handler,
    };
    httpd_register_uri_handler(s_ota_server, &uri_dashboard);

    httpd_uri_t uri_version = {
        .uri = "/api/version", .method = HTTP_GET,
        .handler = version_handler,
    };
    httpd_register_uri_handler(s_ota_server, &uri_version);

    httpd_uri_t uri_status = {
        .uri = "/api/ota/status", .method = HTTP_GET,
        .handler = ota_status_handler,
    };
    httpd_register_uri_handler(s_ota_server, &uri_status);

    httpd_uri_t uri_update = {
        .uri = "/api/ota/update", .method = HTTP_POST,
        .handler = ota_update_handler,
    };
    httpd_register_uri_handler(s_ota_server, &uri_update);

    httpd_uri_t uri_stm32_update = {
        .uri = "/api/stm32/update", .method = HTTP_POST,
        .handler = stm32_update_handler,
    };
    httpd_register_uri_handler(s_ota_server, &uri_stm32_update);

    httpd_uri_t uri_stm32_status = {
        .uri = "/api/stm32/status", .method = HTTP_GET,
        .handler = stm32_status_handler,
    };
    httpd_register_uri_handler(s_ota_server, &uri_stm32_status);

    ESP_LOGI(TAG, "OTA HTTP server started");
}

static void stop_ota_server(void)
{
    if (s_ota_server) {
        httpd_stop(s_ota_server);
        s_ota_server = NULL;
        ESP_LOGI(TAG, "OTA HTTP server stopped");
    }
}

/* ========================================================================
 *  WiFi event handler — maps async events to event group bits
 * ======================================================================== */
static void on_config_done(void);
static void auto_ota_task(void *arg);
static void boot_button_task(void *arg);

/* ========================================================================
 *  BOOT button task (GPIO0)
 * ======================================================================== */
static void boot_button_task(void *arg)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << 0),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cfg);

    int pressed = 0;
    while (1) {
        if (gpio_get_level(0) == 0) {  /* pressed (active low) */
            if (!pressed) {
                pressed = 1;
                vTaskDelay(pdMS_TO_TICKS(50));  /* debounce */
                if (gpio_get_level(0) == 0) {
                    if (!s_sta_netif || !wifi_config_ap_is_running()) {
                        /* Check if WiFi is connected */
                        wifi_ap_record_t ap;
                        if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
                            ESP_LOGI(TAG, "BOOT: No WiFi, starting config AP...");
                            status_led_ap_mode();
                            wifi_config_ap_start(AP_SSID_PREFIX, s_sta_netif, on_config_done);
                        } else {
                            ESP_LOGI(TAG, "BOOT: WiFi connected, checking updates...");
                            xTaskCreate(auto_ota_task, "auto_ota", 6144, NULL, 3, NULL);
                        }
                    }
                }
            }
        } else {
            pressed = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void auto_ota_task(void *arg);
static void stm32_listen_task(void *arg)
{
    uint8_t buf[64];
    while (1) {
        if (!stm32_forwarder_is_running()) {
            int len = uart_read_bytes(UART_NUM_1, buf, sizeof(buf), pdMS_TO_TICKS(1000));
            if (len >= 7) {
                for (int i = 0; i <= len - 7; i++) {
                    if (buf[i] == 0xAA && buf[i+1] == 0x55 && buf[i+2] == 0x06) {
                        ESP_LOGI(TAG, "*** STM32 BOOT COMPLETE — OTA SUCCESS ***");
                        break;
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
static void sta_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(s_sta_evt_group, STA_STARTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(s_sta_evt_group, SCAN_DONE_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        stop_ota_server();
        xEventGroupSetBits(s_sta_evt_group, STA_FAIL_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "STA IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_sta_evt_group, STA_CONNECTED_BIT);
        status_led_wifi_connected();
        /* start OTA server only if config AP is NOT running (pure STA mode) */
        if (!s_ota_server && !wifi_config_ap_is_running()) {
            start_ota_server();
#if OTA_AUTO_CHECK
            xTaskCreate(auto_ota_task, "auto_ota", 6144, NULL, 3, NULL);
            xTaskCreate(stm32_listen_task, "stm_lsn", 4096, NULL, 2, NULL);
#endif
            /* Listen for STM32 boot messages */
        }
    }
}

/* ========================================================================
 *  try_scan_connect
 * ======================================================================== */
static bool try_scan_connect(void)
{
    if (!ssid_manager_has_saved()) {
        ESP_LOGI(TAG, "No saved credentials");
        return false;
    }

    EventBits_t started = xEventGroupWaitBits(s_sta_evt_group, STA_STARTED_BIT,
                                              pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));
    if (!(started & STA_STARTED_BIT)) {
        ESP_LOGW(TAG, "STA start timeout");
        return false;
    }

    wifi_ssid_item_t saved_list[SSID_MANAGER_MAX_COUNT];
    int saved_count = 0;
    ssid_manager_get_list(saved_list, &saved_count);

    ESP_LOGI(TAG, "Scanning for %d saved SSID(s)...", saved_count);

    xEventGroupClearBits(s_sta_evt_group, SCAN_DONE_BIT);
    esp_wifi_scan_start(NULL, true);
    EventBits_t scan_bits = xEventGroupWaitBits(s_sta_evt_group, SCAN_DONE_BIT,
                                                pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));
    if (!(scan_bits & SCAN_DONE_BIT)) {
        ESP_LOGW(TAG, "Scan timeout");
        return false;
    }

    uint16_t ap_count = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
    if (ap_count == 0) {
        ESP_LOGW(TAG, "No APs found");
        return false;
    }

    wifi_ap_record_t *ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!ap_records) {
        ESP_LOGE(TAG, "OOM for scan records");
        return false;
    }
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_records));
    ESP_LOGI(TAG, "Scan found %d AP(s)", ap_count);

    int best_rssi = -128;
    int best_channel = 0;
    char best_ssid[33] = {0};
    char best_password[65] = {0};

    for (int i = 0; i < ap_count; i++) {
        for (int j = 0; j < saved_count; j++) {
            if (strcmp((char *)ap_records[i].ssid, saved_list[j].ssid) == 0) {
                if (ap_records[i].rssi > best_rssi) {
                    best_rssi = ap_records[i].rssi;
                    best_channel = ap_records[i].primary;
                    strncpy(best_ssid, (char *)ap_records[i].ssid, 32);
                    strncpy(best_password, saved_list[j].password, 64);
                }
            }
        }
    }
    free(ap_records);

    if (best_ssid[0] == '\0') {
        ESP_LOGW(TAG, "No saved SSID found");
        return false;
    }

    ESP_LOGI(TAG, "Connecting to %s (RSSI=%d, ch=%d)...",
             best_ssid, best_rssi, best_channel);

    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, best_ssid, sizeof(sta_config.sta.ssid));
    if (strlen(best_password) > 0) {
        strncpy((char *)sta_config.sta.password, best_password,
                sizeof(sta_config.sta.password));
        sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    sta_config.sta.channel = best_channel;
    sta_config.sta.scan_method = WIFI_FAST_SCAN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));

    xEventGroupClearBits(s_sta_evt_group, STA_CONNECTED_BIT | STA_FAIL_BIT);
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_sta_evt_group,
                       STA_CONNECTED_BIT | STA_FAIL_BIT,
                       pdTRUE, pdFALSE, pdMS_TO_TICKS(15000));

    if (bits & STA_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to %s!", best_ssid);
        ssid_manager_add(best_ssid, best_password);
        status_led_wifi_connected();
        return true;
    }

    ESP_LOGW(TAG, "Failed to connect to %s", best_ssid);
    status_led_wifi_connecting();  /* keep trying */
    return false;
}

/* ========================================================================
 *  Config done callback
 * ======================================================================== */
static void on_config_done(void)
{
    ESP_LOGI(TAG, "WiFi configured, transitioning to STA mode...");
    status_led_wifi_connecting();
    wifi_config_ap_stop();
    try_scan_connect();
}

/* ========================================================================
 *  STM32 boot message listener
 * ======================================================================== */

/* ========================================================================
 *  Auto OTA check task
 * ======================================================================== */
static void auto_ota_task(void *arg)
{
    /* Wait for WiFi to stabilize */
    vTaskDelay(pdMS_TO_TICKS(5000));

    ESP_LOGI(TAG, "Auto-checking for firmware updates...");

    /* ---- ESP32 check FIRST ---- */
    {
        ESP_LOGI(TAG, "Checking ESP32 firmware...");

        esp_http_client_config_t ecfg = {
            .url = ESP32_VERSION_URL,
            .timeout_ms = 10000,
            .buffer_size = 256,
            .skip_cert_common_name_check = true,
        };
        esp_http_client_handle_t eh = esp_http_client_init(&ecfg);
        if (eh) {
            if (esp_http_client_open(eh, 0) == ESP_OK) {
                int cl = esp_http_client_fetch_headers(eh);
                if (cl <= 0) cl = 128;
                char *b2 = malloc(cl + 1);
                if (b2) {
                    int t2 = 0;
                    while (t2 < cl) { int r2 = esp_http_client_read(eh, b2 + t2, cl - t2); if (r2 <= 0) break; t2 += r2; }
                    b2[t2] = '\0';
                    char *nl2 = strchr(b2, '\n');
                    if (nl2) *nl2 = '\0';
                    char *esp_ver = b2;
                    char last_esp_ver[64] = "";
                    nvs_handle_t n2;
                    if (nvs_open("ota_esp32", NVS_READONLY, &n2) == ESP_OK) {
                        size_t l2 = sizeof(last_esp_ver);
                        nvs_get_str(n2, "last_ver", last_esp_ver, &l2);
                        nvs_close(n2);
                    }
                    if (strlen(esp_ver) > 0 && strcmp(esp_ver, ota_manager_get_version()) != 0
                        && strcmp(esp_ver, last_esp_ver) != 0) {
                        ESP_LOGI(TAG, "New ESP32 version %s! Updating...", esp_ver);
                        char esp_url[256];
                        snprintf(esp_url, sizeof(esp_url), "%s/%s", ESP32_OTA_BASE_URL, "ESPOTA.bin");
                        status_led_ota_esp32(0);
                        ota_manager_start(esp_url, NULL);
                        if (nvs_open("ota_esp32", NVS_READWRITE, &n2) == ESP_OK) {
                            nvs_set_str(n2, "last_ver", esp_ver);
                            nvs_commit(n2); nvs_close(n2);
                        }
                        free(b2);
                        esp_http_client_close(eh);
                        esp_http_client_cleanup(eh);
                        goto done;  /* ESP32 update in progress, skip STM32 check */
                    }
                    ESP_LOGI(TAG, "ESP32 firmware up to date (%s)", esp_ver);
                    free(b2);
                }
                esp_http_client_close(eh);
            }
            esp_http_client_cleanup(eh);
        }
    }

    /* ---- STM32 check ---- */

    /* Step 1: HTTP GET version file */
    esp_http_client_config_t http_cfg = {
        .url = STM32_VERSION_URL,
        .timeout_ms = 10000,
        .buffer_size = 512,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t http = esp_http_client_init(&http_cfg);
    if (!http) {
        ESP_LOGW(TAG, "Auto-check: HTTP init failed");
        goto done;
    }

    esp_err_t ret = esp_http_client_open(http, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Auto-check: HTTP open failed");
        esp_http_client_cleanup(http);
        goto done;
    }

    int content_len = esp_http_client_fetch_headers(http);
    if (content_len <= 0) content_len = 256;

    char *buf = malloc(content_len + 1);
    if (!buf) {
        esp_http_client_close(http);
        esp_http_client_cleanup(http);
        goto done;
    }

    int total = 0;
    while (total < content_len) {
        int r = esp_http_client_read(http, buf + total, content_len - total);
        if (r <= 0) break;
        total += r;
    }
    buf[total] = '\0';

    esp_http_client_close(http);
    esp_http_client_cleanup(http);

    /* Step 2: Parse version file (format: version\nfilename) */
    char *newline = strchr(buf, '\n');
    char filename_buf[128] = {0};
    if (newline) {
        *newline = '\0';
        strncpy(filename_buf, newline + 1, sizeof(filename_buf) - 1);
        /* Trim trailing \r \n */
        for (int i = strlen(filename_buf) - 1; i >= 0; i--) {
            if (filename_buf[i] == '\r' || filename_buf[i] == '\n')
                filename_buf[i] = '\0';
            else break;
        }
    }
    char *version = buf;

    ESP_LOGI(TAG, "Auto-check: server version=%s, file=%s", version, filename_buf);

    /* Step 3: Compare with NVS-stored version */
    nvs_handle_t nvs;
    char last_ver[64] = "";
    if (nvs_open("ota_stm32", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(last_ver);
        nvs_get_str(nvs, "last_ver", last_ver, &len);
        nvs_close(nvs);
    }

    if (strlen(version) > 0 && strcmp(version, last_ver) != 0) {
        ESP_LOGI(TAG, "New STM32 version %s (was %s)! Triggering OTA...", version, last_ver);
        char full_url[256];
        snprintf(full_url, sizeof(full_url), "%s/%s", STM32_OTA_BASE_URL,
                 strlen(filename_buf) > 0 ? filename_buf : "OTAESP32STM32.bin");
        status_led_ota_stm32(0);
        stm32_forwarder_start(full_url, NULL);

        /* Save new version */
        if (nvs_open("ota_stm32", NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_str(nvs, "last_ver", version);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
    } else {
        ESP_LOGI(TAG, "Auto-check: STM32 firmware is up to date (ver %s)", last_ver);
    }

    free(buf);

    /* ---- ESP32 check (only if STM32 didn't start OTA) ---- */
    if (!stm32_forwarder_is_running()) {
        ESP_LOGI(TAG, "Checking ESP32 firmware...");
    {
        esp_http_client_config_t ecfg = {
            .url = ESP32_VERSION_URL, .timeout_ms = 10000, .buffer_size = 256,
            .skip_cert_common_name_check = true,
        };
        esp_http_client_handle_t eh = esp_http_client_init(&ecfg);
        if (eh && esp_http_client_open(eh, 0) == ESP_OK) {
            int cl = esp_http_client_fetch_headers(eh);
            if (cl <= 0) cl = 128;
            char *b2 = malloc(cl + 1);
            if (b2) {
                int t2 = 0;
                while (t2 < cl) { int r2 = esp_http_client_read(eh, b2 + t2, cl - t2); if (r2 <= 0) break; t2 += r2; }
                b2[t2] = '\0';
                char *nl2 = strchr(b2, '\n');
                if (nl2) *nl2 = '\0';
                char *esp_ver = b2;
                char last_esp_ver[64] = "";
                nvs_handle_t n2;
                if (nvs_open("ota_esp32", NVS_READONLY, &n2) == ESP_OK) {
                    size_t l2 = sizeof(last_esp_ver);
                    nvs_get_str(n2, "last_ver", last_esp_ver, &l2);
                    nvs_close(n2);
                }
                if (strlen(esp_ver) > 0 && strcmp(esp_ver, ota_manager_get_version()) != 0
                    && strcmp(esp_ver, last_esp_ver) != 0) {
                    ESP_LOGI(TAG, "New ESP32 version %s! Updating...", esp_ver);
                    char esp_url[256];
                    snprintf(esp_url, sizeof(esp_url), "%s/%s", ESP32_OTA_BASE_URL, "ESPOTA.bin");
                    status_led_ota_esp32(0);
                    ota_manager_start(esp_url, NULL);
                    if (nvs_open("ota_esp32", NVS_READWRITE, &n2) == ESP_OK) {
                        nvs_set_str(n2, "last_ver", esp_ver);
                        nvs_commit(n2); nvs_close(n2);
                    }
                } else {
                    ESP_LOGI(TAG, "ESP32 up to date (%s)", esp_ver);
                }
                free(b2);
            }
            esp_http_client_close(eh);
        }
        if (eh) esp_http_client_cleanup(eh);
    }
    } /* end if !forwarder_running */

done:
    vTaskDelete(NULL);
}

/* ========================================================================
 *  app_main
 * ======================================================================== */
void app_main(void)
{
    /* ---- NVS flash init ---- */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ---- WiFi credential manager ---- */
    ssid_manager_init();
    xTaskCreate(boot_button_task, "boot_btn", 3072, NULL, 2, NULL);

    /* ---- Status LED ---- */
    status_led_init();
    status_led_wifi_connecting();

    /* ---- OTA manager (print version/partition info) ---- */
    ota_manager_init();

    /* ---- STM32 forwarder (UART + SPI) ---- */
    stm32_forwarder_init();

    /* ---- netif + event loop + wifi driver ---- */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();

    /* ---- event group + handlers ---- */
    s_sta_evt_group = xEventGroupCreate();
    esp_event_handler_instance_t wifi_inst = NULL;
    esp_event_handler_instance_t ip_inst = NULL;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &sta_event_handler, NULL, &wifi_inst));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &sta_event_handler, NULL, &ip_inst));

    /* ---- WiFi init + start ---- */
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    xEventGroupClearBits(s_sta_evt_group, STA_STARTED_BIT);
    ESP_ERROR_CHECK(esp_wifi_start());

    /* ---- connect or start config AP ---- */
    bool connected = false;
    for (int attempt = 0; attempt < 3 && !connected; attempt++) {
        if (attempt > 0) {
            ESP_LOGI(TAG, "Retry %d/2...", attempt);
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
        connected = try_scan_connect();
    }

    if (!connected) {
        ESP_LOGI(TAG, "Starting config AP...");
        status_led_ap_mode();
        wifi_config_ap_start(AP_SSID_PREFIX, s_sta_netif, on_config_done);
        ESP_LOGI(TAG, "Config AP: ESP32-OTA-XXXX, http://192.168.4.1");
    }

    /* ---- main loop ---- */
    ESP_LOGI(TAG, "System ready. Version: %s", ota_manager_get_version());
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
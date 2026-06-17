#include "wifi_config_ap.h"
#include "ssid_manager.h"
#include "dns_server.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "wifi_cfg_ap";

/* ---------- EMBED_TXTFILES symbols ---------- */
extern const char wifi_config_html_start[] asm("_binary_wifi_config_html_start");
extern const char done_html_start[] asm("_binary_done_html_start");

/* ---------- state ---------- */
static httpd_handle_t s_server = NULL;
static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;   /* borrowed from main.c */
static bool s_running = false;
static wifi_config_exit_cb_t s_done_cb = NULL;
static char s_ap_ssid[32] = {0};
static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_event_handler_instance_t s_wifi_handler = NULL;
static esp_event_handler_instance_t s_ip_handler = NULL;

/* context for done_task */
typedef struct {
    wifi_config_exit_cb_t cb;
} done_task_ctx_t;
static done_task_ctx_t s_done_ctx;

/* ---------- scan ---------- */
#define MAX_AP_RECORDS  15
static wifi_ap_record_t s_ap_records[MAX_AP_RECORDS];
static uint16_t s_ap_count = 0;
static esp_timer_handle_t s_scan_timer = NULL;
static bool s_is_connecting = false;

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

/* ========================================================================
 *  Event handler (APSTA mode: listens for STA connect/fail + scan done)
 * ======================================================================== */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_is_connecting) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        if (s_is_connecting) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        uint16_t ap_num = 0;
        esp_wifi_scan_get_ap_num(&ap_num);
        if (ap_num > MAX_AP_RECORDS) {
            ap_num = MAX_AP_RECORDS;
        }
        esp_wifi_scan_get_ap_records(&ap_num, s_ap_records);
        s_ap_count = ap_num;

        /* schedule next scan in 10 seconds */
        if (s_scan_timer && s_running && !s_is_connecting) {
            esp_timer_start_once(s_scan_timer, 10 * 1000000);
        }
    }
}

/* ========================================================================
 *  Scan timer callback
 * ======================================================================== */
static void scan_timer_cb(void *arg)
{
    if (s_running && !s_is_connecting) {
        esp_wifi_scan_start(NULL, false);
    }
}

/* ========================================================================
 *  JSON string escape (for SSID in scan output)
 * ======================================================================== */
static void json_escape_str(const char *src, char *dst, size_t dst_size)
{
    size_t di = 0;
    for (size_t si = 0; src[si] && di + 2 < dst_size; si++) {
        unsigned char c = (unsigned char)src[si];
        switch (c) {
        case '"':  dst[di++] = '\\'; dst[di++] = '"'; break;
        case '\\': dst[di++] = '\\'; dst[di++] = '\\'; break;
        case '\b': dst[di++] = '\\'; dst[di++] = 'b'; break;
        case '\f': dst[di++] = '\\'; dst[di++] = 'f'; break;
        case '\n': dst[di++] = '\\'; dst[di++] = 'n'; break;
        case '\r': dst[di++] = '\\'; dst[di++] = 'r'; break;
        case '\t': dst[di++] = '\\'; dst[di++] = 't'; break;
        default:
            if (c < 0x20) {
                int n = snprintf(dst + di, dst_size - di, "\\u%04x", c);
                if (n > 0) di += n;
            } else {
                dst[di++] = c;
            }
            break;
        }
    }
    dst[di] = '\0';
}

/* ========================================================================
 *  HTTP handlers
 * ======================================================================== */

/* GET /  — main config page */
static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, wifi_config_html_start, strlen(wifi_config_html_start));
}

/* GET /scan  — return cached scan results */
static esp_err_t scan_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");

    if (s_ap_count == 0) {
        return httpd_resp_send(req, "[]", 2);
    }

    char *json = malloc(s_ap_count * 160 + 8);
    if (!json) {
        return httpd_resp_send(req, "[]", 2);
    }

    char escaped_ssid[100];

    int pos = 0;
    json[pos++] = '[';
    for (int i = 0; i < s_ap_count; i++) {
        if (strlen((char *)s_ap_records[i].ssid) == 0) {
            continue;
        }
        json_escape_str((char *)s_ap_records[i].ssid, escaped_ssid, sizeof(escaped_ssid));
        if (pos > 1) {
            json[pos++] = ',';
        }
        pos += sprintf(json + pos, "{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                       escaped_ssid,
                       s_ap_records[i].rssi,
                       s_ap_records[i].authmode);
    }
    json[pos++] = ']';

    esp_err_t ret = httpd_resp_send(req, json, pos);
    free(json);
    return ret;
}

/* ========================================================================
 *  /submit  — test-connect with user-provided SSID/password.
 *  On success: save to NVS, disconnect test connection, and trigger done_cb.
 *  No reboot — the main loop switches from APSTA to pure STA.
 * ======================================================================== */
static esp_err_t submit_handler(httpd_req_t *req)
{
    char buf[300] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char ssid[33] = {0};
    char password[65] = {0};

    /* find "ssid" value */
    char *ssid_key = strstr(buf, "\"ssid\"");
    if (ssid_key) {
        char *colon = strchr(ssid_key + 6, ':');
        if (colon) {
            char *start = strchr(colon + 1, '"');
            if (start) {
                start++;
                char *end = strchr(start, '"');
                if (end) {
                    int len = end - start;
                    if (len > 32) len = 32;
                    strncpy(ssid, start, len);
                }
            }
        }
    }

    /* find "password" value */
    char *pass_key = strstr(buf, "\"password\"");
    if (pass_key) {
        char *colon = strchr(pass_key + 10, ':');
        if (colon) {
            char *start = strchr(colon + 1, '"');
            if (start) {
                start++;
                char *end = strchr(start, '"');
                if (end) {
                    int len = end - start;
                    if (len > 64) len = 64;
                    strncpy(password, start, len);
                }
            }
        }
    }

    if (strlen(ssid) == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_send(req, "{\"success\":false,\"error\":\"SSID is empty\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Try connect: SSID=[%s] PASS_LEN=%d", ssid, (int)strlen(password));

    /* ---- test-connect using STA ---- */
    s_is_connecting = true;

    /* stop periodic scan during connection */
    if (s_scan_timer) {
        esp_timer_stop(s_scan_timer);
    }
    esp_wifi_scan_stop();

    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
    if (strlen(password) > 0) {
        strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password));
        sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                       WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                       pdTRUE, pdFALSE, pdMS_TO_TICKS(15000));

    s_is_connecting = false;

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to %s — saving credentials", ssid);
        ssid_manager_add(ssid, password);

        /* disconnect the test connection; main loop will reconnect */
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));

        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);

        ESP_LOGI(TAG, "Credentials saved, config done.");
        /* done_cb will be called after done.html hits /exit */
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Failed to connect to %s", ssid);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    /* resume scanning */
    if (s_running && s_scan_timer) {
        esp_wifi_scan_start(NULL, false);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"Connection failed\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* GET /done.html  — success page */
static esp_err_t done_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, done_html_start, strlen(done_html_start));
}

/* task that runs the done callback after HTTP response is flushed */
static void done_task(void *ctx)
{
    vTaskDelay(pdMS_TO_TICKS(300));
    done_task_ctx_t *dctx = (done_task_ctx_t *)ctx;
    if (dctx->cb) {
        dctx->cb();
    }
    vTaskDelete(NULL);
}

/* POST /exit  — user clicked "done" on the success page */
static esp_err_t exit_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);

    ESP_LOGI(TAG, "Exit requested — triggering done callback...");

    s_done_ctx.cb = s_done_cb;
    xTaskCreate(done_task, "done_task", 4096, &s_done_ctx, 5, NULL);

    return ESP_OK;
}

/* GET /saved/list */
static esp_err_t saved_list_handler(httpd_req_t *req)
{
    wifi_ssid_item_t list[SSID_MANAGER_MAX_COUNT];
    int count = 0;
    ssid_manager_get_list(list, &count);

    char *json = malloc(count * 40 + 16);
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int pos = 0;
    json[pos++] = '[';
    for (int i = 0; i < count; i++) {
        if (i > 0) json[pos++] = ',';
        pos += sprintf(json + pos, "\"%s\"", list[i].ssid);
    }
    json[pos++] = ']';

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t ret = httpd_resp_send(req, json, pos);
    free(json);
    return ret;
}

/* GET /saved/delete?index=N */
static esp_err_t saved_delete_handler(httpd_req_t *req)
{
    char buf[32] = {0};
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char index_str[8] = {0};
        if (httpd_query_key_value(buf, "index", index_str, sizeof(index_str)) == ESP_OK) {
            int index = atoi(index_str);
            ssid_manager_remove(index);
            ESP_LOGI(TAG, "Deleted WiFi at index %d", index);
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, "{}", 2);
    return ESP_OK;
}

/* captive portal redirect */
static esp_err_t captive_portal_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ========================================================================
 *  Web server
 * ======================================================================== */
static void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        return;
    }

    httpd_uri_t uri_index = {
        .uri = "/", .method = HTTP_GET, .handler = index_handler,
    };
    httpd_register_uri_handler(s_server, &uri_index);

    httpd_uri_t uri_done = {
        .uri = "/done.html", .method = HTTP_GET, .handler = done_page_handler,
    };
    httpd_register_uri_handler(s_server, &uri_done);

    httpd_uri_t uri_scan = {
        .uri = "/scan", .method = HTTP_GET, .handler = scan_handler,
    };
    httpd_register_uri_handler(s_server, &uri_scan);

    httpd_uri_t uri_submit = {
        .uri = "/submit", .method = HTTP_POST, .handler = submit_handler,
    };
    httpd_register_uri_handler(s_server, &uri_submit);

    httpd_uri_t uri_exit = {
        .uri = "/exit", .method = HTTP_POST, .handler = exit_handler,
    };
    httpd_register_uri_handler(s_server, &uri_exit);

    httpd_uri_t uri_saved_list = {
        .uri = "/saved/list", .method = HTTP_GET, .handler = saved_list_handler,
    };
    httpd_register_uri_handler(s_server, &uri_saved_list);

    httpd_uri_t uri_saved_delete = {
        .uri = "/saved/delete", .method = HTTP_GET, .handler = saved_delete_handler,
    };
    httpd_register_uri_handler(s_server, &uri_saved_delete);

    /* captive portal detection URLs */
    const char *captive_urls[] = {
        "/hotspot-detect.html",
        "/generate_204",
        "/connectivity-check.html",
        "/ncsi.txt",
        "/success.txt",
        "/portal.html",
        "/fwlink/",
        "/mobile/status.php",
        "/check_network_status.txt",
        "/library/test/success.html",
    };
    for (int i = 0; i < sizeof(captive_urls) / sizeof(captive_urls[0]); i++) {
        httpd_uri_t uri = {
            .uri = captive_urls[i], .method = HTTP_GET,
            .handler = captive_portal_handler,
        };
        httpd_register_uri_handler(s_server, &uri);
    }

    ESP_LOGI(TAG, "Web server started");
}

/* ========================================================================
 *  Public API
 * ======================================================================== */
esp_err_t wifi_config_ap_start(const char *ssid_prefix, esp_netif_t *sta_netif, wifi_config_exit_cb_t done_cb)
{
    if (s_running) {
        ESP_LOGW(TAG, "Config AP already running");
        return ESP_OK;
    }

    s_done_cb = done_cb;
    s_sta_netif = sta_netif;   /* reuse main.c's STA netif */
    s_wifi_event_group = xEventGroupCreate();

    /* create AP netif */
    s_ap_netif = esp_netif_create_default_wifi_ap();

    /* generate AP SSID from MAC */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X",
             ssid_prefix ? ssid_prefix : "ESP32", mac[4], mac[5]);

    /* configure AP IP */
    esp_netif_ip_info_t ip_info = {
        .ip.addr = esp_ip4addr_aton("192.168.4.1"),
        .gw.addr = esp_ip4addr_aton("192.168.4.1"),
        .netmask.addr = esp_ip4addr_aton("255.255.255.0"),
    };
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_set_ip_info(s_ap_netif, &ip_info);
    esp_netif_dhcps_start(s_ap_netif);

    /* DNS captive portal */
    ip4_addr_t gw = {.addr = ip_info.gw.addr};
    dns_server_start(gw);

    /* register APSTA event handlers */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                    ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &s_wifi_handler));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                    IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &s_ip_handler));

    /* configure AP */
    wifi_config_t ap_config = {0};
    strncpy((char *)ap_config.ap.ssid, s_ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(s_ap_ssid);
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    /* WiFi already running (started by main.c) — just set the AP config
     * and restart to apply the mode change */
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* start web server */
    start_webserver();

    /* periodic WiFi scan for the web UI */
    s_running = true;
    esp_wifi_scan_start(NULL, false);

    esp_timer_create_args_t timer_args = {
        .callback = scan_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_scan",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_scan_timer));

    ESP_LOGI(TAG, "Config AP started: %s, visit http://192.168.4.1", s_ap_ssid);
    return ESP_OK;
}

void wifi_config_ap_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;

    /* stop scan timer */
    if (s_scan_timer) {
        esp_timer_stop(s_scan_timer);
        esp_timer_delete(s_scan_timer);
        s_scan_timer = NULL;
    }

    /* stop web server */
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }

    /* stop DNS server */
    dns_server_stop();

    /* unregister our event handlers */
    if (s_wifi_handler) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler);
        s_wifi_handler = NULL;
    }
    if (s_ip_handler) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_handler);
        s_ip_handler = NULL;
    }

    /* stop WiFi (driver stays alive) */
    esp_wifi_stop();

    /* destroy AP netif */
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }

    /* free event group */
    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    /* switch back to pure STA mode and restart */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Config AP stopped — running in STA mode");
}

bool wifi_config_ap_is_running(void)
{
    return s_running;
}
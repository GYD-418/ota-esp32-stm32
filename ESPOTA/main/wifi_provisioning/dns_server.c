#include "dns_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdatomic.h>

static const char *TAG = "dns_server";
static TaskHandle_t s_dns_task = NULL;
static int s_sock = -1;
static atomic_bool s_running = false;
static ip4_addr_t s_gateway;

#define DNS_PORT 53
#define DNS_BUF_SIZE 512

static void dns_server_task(void *arg)
{
    uint8_t rx_buf[DNS_BUF_SIZE];
    uint8_t tx_buf[DNS_BUF_SIZE];
    struct sockaddr_in client_addr;
    socklen_t addr_len;

    while (s_running) {
        addr_len = sizeof(client_addr);
        int len = recvfrom(s_sock, rx_buf, sizeof(rx_buf), 0,
                          (struct sockaddr *)&client_addr, &addr_len);

        if (len < 12 || !s_running) {
            continue;
        }

        memcpy(tx_buf, rx_buf, len);

        tx_buf[2] = 0x81;
        tx_buf[3] = 0x80;
        tx_buf[6] = 0x00;
        tx_buf[7] = 0x01;

        int offset = len;

        tx_buf[offset++] = 0xC0;
        tx_buf[offset++] = 0x0C;
        tx_buf[offset++] = 0x00;
        tx_buf[offset++] = 0x01;
        tx_buf[offset++] = 0x00;
        tx_buf[offset++] = 0x01;
        tx_buf[offset++] = 0x00;
        tx_buf[offset++] = 0x00;
        tx_buf[offset++] = 0x00;
        tx_buf[offset++] = 0x1C;
        tx_buf[offset++] = 0x00;
        tx_buf[offset++] = 0x04;

        uint32_t ip = ntohl(s_gateway.addr);
        tx_buf[offset++] = (ip >> 24) & 0xFF;
        tx_buf[offset++] = (ip >> 16) & 0xFF;
        tx_buf[offset++] = (ip >> 8) & 0xFF;
        tx_buf[offset++] = ip & 0xFF;

        sendto(s_sock, tx_buf, offset, 0,
               (struct sockaddr *)&client_addr, addr_len);
    }

    /* only close if s_sock is still valid — dns_server_stop() may have
     * already closed it and set s_sock to -1 */
    int sock = s_sock;
    s_sock = -1;
    if (sock >= 0) {
        close(sock);
    }
    vTaskDelete(NULL);
}

esp_err_t dns_server_start(ip4_addr_t gateway)
{
    if (s_running) {
        ESP_LOGW(TAG, "DNS server already running");
        return ESP_OK;
    }

    s_gateway = gateway;

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        return ESP_FAIL;
    }

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(s_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind socket");
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }

    s_running = true;
    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, &s_dns_task);

    ESP_LOGI(TAG, "DNS server started on port %d", DNS_PORT);
    return ESP_OK;
}

void dns_server_stop(void)
{
    if (!s_running) {
        return;
    }

    s_running = false;

    /* atomically capture and invalidate s_sock; then close it to
     * unblock recvfrom() in the DNS task — the task will see s_sock == -1
     * and skip its own close, avoiding a double-close race. */
    int sock = s_sock;
    s_sock = -1;
    if (sock >= 0) {
        close(sock);
    }

    /* wait for the task to notice s_running == false and exit */
    if (s_dns_task) {
        vTaskDelay(pdMS_TO_TICKS(200));
        s_dns_task = NULL;
    }

    ESP_LOGI(TAG, "DNS server stopped");
}
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SSID_MANAGER_MAX_COUNT      10
#define SSID_MANAGER_MAX_SSID_LEN   33
#define SSID_MANAGER_MAX_PASS_LEN   65

typedef struct {
    char ssid[SSID_MANAGER_MAX_SSID_LEN];
    char password[SSID_MANAGER_MAX_PASS_LEN];
} wifi_ssid_item_t;

esp_err_t ssid_manager_init(void);
esp_err_t ssid_manager_add(const char *ssid, const char *password);
esp_err_t ssid_manager_remove(int index);
esp_err_t ssid_manager_set_default(int index);
esp_err_t ssid_manager_get_list(wifi_ssid_item_t *list, int *count);
esp_err_t ssid_manager_get_first(char *ssid, char *password);
bool ssid_manager_has_saved(void);
esp_err_t ssid_manager_clear(void);

#ifdef __cplusplus
}
#endif
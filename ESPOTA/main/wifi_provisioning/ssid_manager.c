#include "ssid_manager.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ssid_mgr";
static const char *NVS_NAMESPACE = "wifi";

static const char *SSID_KEYS[] = {
    "ssid", "ssid1", "ssid2", "ssid3", "ssid4",
    "ssid5", "ssid6", "ssid7", "ssid8", "ssid9"
};
static const char *PASS_KEYS[] = {
    "password", "password1", "password2", "password3", "password4",
    "password5", "password6", "password7", "password8", "password9"
};

static wifi_ssid_item_t s_ssid_list[SSID_MANAGER_MAX_COUNT];
static int s_ssid_count = 0;
static bool s_initialized = false;

static esp_err_t load_from_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "No saved WiFi config");
        s_ssid_count = 0;
        return ESP_OK;
    }

    s_ssid_count = 0;
    for (int i = 0; i < SSID_MANAGER_MAX_COUNT; i++) {
        wifi_ssid_item_t *item = &s_ssid_list[i];
        memset(item, 0, sizeof(wifi_ssid_item_t));

        size_t ssid_len = SSID_MANAGER_MAX_SSID_LEN;
        size_t pass_len = SSID_MANAGER_MAX_PASS_LEN;

        esp_err_t ssid_ret = nvs_get_str(nvs, SSID_KEYS[i], item->ssid, &ssid_len);
        esp_err_t pass_ret = nvs_get_str(nvs, PASS_KEYS[i], item->password, &pass_len);

        if (ssid_ret == ESP_OK && strlen(item->ssid) > 0) {
            s_ssid_count++;
        }
        /* continue scanning even if this slot is empty —
         * later slots may still hold valid entries */
    }

    nvs_close(nvs);
    ESP_LOGI(TAG, "Loaded %d saved WiFi(s)", s_ssid_count);
    return ESP_OK;
}

static esp_err_t save_to_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    for (int i = 0; i < SSID_MANAGER_MAX_COUNT; i++) {
        if (i < s_ssid_count) {
            nvs_set_str(nvs, SSID_KEYS[i], s_ssid_list[i].ssid);
            nvs_set_str(nvs, PASS_KEYS[i], s_ssid_list[i].password);
        } else {
            nvs_erase_key(nvs, SSID_KEYS[i]);
            nvs_erase_key(nvs, PASS_KEYS[i]);
        }
    }

    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "Saved %d WiFi(s) to NVS", s_ssid_count);
    return ESP_OK;
}

esp_err_t ssid_manager_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    load_from_nvs();
    s_initialized = true;
    return ESP_OK;
}

esp_err_t ssid_manager_add(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < s_ssid_count; i++) {
        if (strcmp(s_ssid_list[i].ssid, ssid) == 0) {
            if (i == 0) {
                strncpy(s_ssid_list[0].password, password, SSID_MANAGER_MAX_PASS_LEN - 1);
            } else {
                memmove(&s_ssid_list[1], &s_ssid_list[0], i * sizeof(wifi_ssid_item_t));
                strncpy(s_ssid_list[0].ssid, ssid, SSID_MANAGER_MAX_SSID_LEN - 1);
                strncpy(s_ssid_list[0].password, password, SSID_MANAGER_MAX_PASS_LEN - 1);
            }
            return save_to_nvs();
        }
    }

    if (s_ssid_count >= SSID_MANAGER_MAX_COUNT) {
        s_ssid_count = SSID_MANAGER_MAX_COUNT - 1;
    }

    memmove(&s_ssid_list[1], &s_ssid_list[0], s_ssid_count * sizeof(wifi_ssid_item_t));
    strncpy(s_ssid_list[0].ssid, ssid, SSID_MANAGER_MAX_SSID_LEN - 1);
    strncpy(s_ssid_list[0].password, password, SSID_MANAGER_MAX_PASS_LEN - 1);
    s_ssid_count++;

    return save_to_nvs();
}

esp_err_t ssid_manager_remove(int index)
{
    if (index < 0 || index >= s_ssid_count) {
        return ESP_ERR_INVALID_ARG;
    }

    if (index < s_ssid_count - 1) {
        memmove(&s_ssid_list[index], &s_ssid_list[index + 1],
                (s_ssid_count - index - 1) * sizeof(wifi_ssid_item_t));
    }
    s_ssid_count--;
    memset(&s_ssid_list[s_ssid_count], 0, sizeof(wifi_ssid_item_t));

    return save_to_nvs();
}

esp_err_t ssid_manager_set_default(int index)
{
    if (index < 0 || index >= s_ssid_count || index == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_ssid_item_t temp = s_ssid_list[index];
    memmove(&s_ssid_list[1], &s_ssid_list[0], index * sizeof(wifi_ssid_item_t));
    s_ssid_list[0] = temp;

    return save_to_nvs();
}

esp_err_t ssid_manager_get_list(wifi_ssid_item_t *list, int *count)
{
    *count = s_ssid_count;
    if (list && s_ssid_count > 0) {
        memcpy(list, s_ssid_list, s_ssid_count * sizeof(wifi_ssid_item_t));
    }
    return ESP_OK;
}

esp_err_t ssid_manager_get_first(char *ssid, char *password)
{
    if (s_ssid_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    strncpy(ssid, s_ssid_list[0].ssid, SSID_MANAGER_MAX_SSID_LEN - 1);
    strncpy(password, s_ssid_list[0].password, SSID_MANAGER_MAX_PASS_LEN - 1);
    return ESP_OK;
}

bool ssid_manager_has_saved(void)
{
    return s_ssid_count > 0;
}

esp_err_t ssid_manager_clear(void)
{
    s_ssid_count = 0;
    memset(s_ssid_list, 0, sizeof(s_ssid_list));
    return save_to_nvs();
}
#pragma once

#include "esp_err.h"
#include "esp_netif.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wifi_config_exit_cb_t)(void);

/**
 * Start the configuration AP on top of an existing STA netif.
 *
 * @param ssid_prefix  prefix for the AP SSID (e.g. "ESP32" → "ESP32-ABCD")
 * @param sta_netif    an already-created STA netif (owned by caller)
 * @param done_cb      called when the user successfully submits credentials
 *                     (after the AP is stopped and STA has an IP)
 */
esp_err_t wifi_config_ap_start(const char *ssid_prefix, esp_netif_t *sta_netif, wifi_config_exit_cb_t done_cb);

/** Stop the configuration AP. STA stays alive. */
void wifi_config_ap_stop(void);

/** Is the configuration AP currently running? */
bool wifi_config_ap_is_running(void);

#ifdef __cplusplus
}
#endif
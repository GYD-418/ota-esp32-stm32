#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** OTA event types for callback */
typedef enum {
    OTA_EVENT_STARTED = 0,
    OTA_EVENT_PROGRESS,
    OTA_EVENT_COMPLETED,
    OTA_EVENT_FAILED,
} ota_event_t;

/** Progress data passed to callback */
typedef struct {
    int progress_pct;
    int downloaded_bytes;
    int total_bytes;
    char error_msg[128];
} ota_progress_t;

/** Callback for OTA events */
typedef void (*ota_callback_t)(ota_event_t event, const ota_progress_t *progress);

/**
 * Initialize OTA subsystem: registers app description, validates partitions.
 */
esp_err_t ota_manager_init(void);

/**
 * Start an OTA update from a URL.
 * Downloads firmware in the background and applies it on success.
 *
 * @param url      HTTP/HTTPS URL to firmware binary
 * @param cb       Optional callback for progress/result
 */
esp_err_t ota_manager_start(const char *url, ota_callback_t cb);

/**
 * Check if an OTA update is in progress.
 */
bool ota_manager_is_running(void);

/**
 * Get the current firmware version string.
 */
const char *ota_manager_get_version(void);

/**
 * Get pending (newly downloaded but not yet booted) version string.
 * Returns NULL if no pending update.
 */
const char *ota_manager_get_pending_version(void);

#ifdef __cplusplus
}
#endif
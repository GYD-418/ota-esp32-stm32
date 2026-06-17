#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Firmware image header (placed at the start of each firmware binary) ---- */
#define FW_HEADER_MAGIC     0x53544D33U   /* "STM3" */

typedef struct __attribute__((packed)) {
    uint32_t magic;          /* FW_HEADER_MAGIC */
    uint32_t version;        /* firmware version (e.g. 0x00010000 = 1.0.0) */
    uint32_t size;           /* payload size in bytes (excluding header) */
    uint32_t crc32;          /* CRC32 of payload */
    uint32_t reserved[4];    /* future use */
} firmware_header_t;

#define FW_HEADER_SIZE  sizeof(firmware_header_t)

/* ---- Config area structure (at CONFIG_ADDR) ---- */
#define CONFIG_MAGIC_UPDATE_REQUEST   0x5A5A5A5AU
#define CONFIG_MAGIC_UPDATE_SUCCESS   0xA5A5A5A5U
#define CONFIG_MAGIC_NO_UPDATE        0xFFFFFFFFU

typedef struct __attribute__((packed)) {
    uint32_t update_magic;       /* update flag */
    uint32_t new_fw_size;        /* size of firmware in download area */
    uint32_t new_fw_crc;         /* CRC32 of firmware in download area */
    uint32_t new_fw_version;     /* version of firmware in download area */
    uint32_t last_good_version;  /* last known good version (for rollback) */
    uint32_t boot_count;         /* incremented each boot, cleared after success */
    uint32_t reserved[2];
} config_area_t;

#ifdef __cplusplus
}
#endif
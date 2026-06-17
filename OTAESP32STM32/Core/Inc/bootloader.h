#pragma once

#include <stdint.h>
#include "firmware_header.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Partition addresses ---- */
#define BOOTLOADER_ADDR     0x08000000U
#define APP_ADDR            0x08008000U
#define DOWNLOAD_ADDR       0x08040000U
#define CONFIG_ADDR         0x0807C000U

#define BOOTLOADER_SIZE     0x8000U    /* 32 KB */
#define APP_SIZE            0x38000U   /* 224 KB */
#define DOWNLOAD_SIZE       0x3C000U   /* 240 KB */
#define CONFIG_SIZE         0x4000U    /* 16 KB */

/* ---- Bootloader API ---- */
void bootloader_check_and_launch(void);
void bootloader_jump_to_app(void);

#ifdef __cplusplus
}
#endif
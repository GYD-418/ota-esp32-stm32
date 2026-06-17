#include "bootloader.h"
#include "firmware_header.h"
#include "stm32f4xx_hal.h"
#include <string.h>

static void iwdg_feed(void) { IWDG->KR = 0xAAAA; }

/* ========================================================================
 *  Minimal CRC32 (Ethernet polynomial)
 * ======================================================================== */
static uint32_t crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
    }
    return ~crc;
}

/* ========================================================================
 *  Flash erase: sectors covering [addr, addr+size)
 *  STM32F407 sectors: 4×16K + 1×64K + 7×128K
 * ======================================================================== */
static HAL_StatusTypeDef flash_erase_range(uint32_t addr, uint32_t size)
{
    HAL_FLASH_Unlock();
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t sector_error = 0;

    uint32_t end = addr + size - 1;

    /* Determine which sectors to erase */
    for (uint32_t s = 0; s < 12; s++) {
        uint32_t sector_start, sector_size;
        if (s < 4)       { sector_start = 0x08000000 + s * 0x4000;  sector_size = 0x4000;  }
        else if (s == 4) { sector_start = 0x08010000;               sector_size = 0x10000; }
        else             { sector_start = 0x08020000 + (s-5)*0x20000; sector_size = 0x20000;}

        if (addr < sector_start + sector_size && end >= sector_start) {
            iwdg_feed();
            erase.TypeErase = FLASH_TYPEERASE_SECTORS;
            erase.Sector = s;
            erase.NbSectors = 1;
            erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
            if (HAL_FLASHEx_Erase(&erase, &sector_error) != HAL_OK) {
                HAL_FLASH_Lock();
                return HAL_ERROR;
            }
        }
    }
    HAL_FLASH_Lock();
    return HAL_OK;
}

/* ========================================================================
 *  Flash program: write buffer to addr (must be erased first)
 * ======================================================================== */
static HAL_StatusTypeDef flash_program(uint32_t addr, const uint8_t *data, uint32_t size)
{
    HAL_FLASH_Unlock();
    for (uint32_t i = 0; i < size; i += 4) {
        iwdg_feed();
        uint32_t word = 0;
        uint32_t remaining = size - i;
        if (remaining >= 4) {
            word = *(uint32_t *)(data + i);
        } else {
            memcpy(&word, data + i, remaining);
        }
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i, word) != HAL_OK) {
            HAL_FLASH_Lock();
            return HAL_ERROR;
        }
    }
    HAL_FLASH_Lock();
    return HAL_OK;
}

/* ========================================================================
 *  Jump to application
 * ======================================================================== */
void bootloader_jump_to_app(void)
{
    uint32_t app_sp = *(volatile uint32_t *)APP_ADDR;
    uint32_t app_pc = *(volatile uint32_t *)(APP_ADDR + 4);

    /* Validate stack pointer — must be in RAM */
    if (app_sp < 0x20000000 || app_sp > 0x20020000) {
        while (1) {}
    }

    /* Disable IWDG before jumping */
    IWDG->KR = 0x5555;   /* unlock */
    IWDG->KR = 0xCCCC;   /* start (reload counter to max) */
    IWDG->KR = 0x5555;   /* unlock */
    IWDG->KR = 0x0000;   /* disable (needs hardware watchdog not enabled in OTP) */

    iwdg_feed();  /* one last feed before jumping */

    /* Set vector table offset */
    SCB->VTOR = APP_ADDR;

    /* Reset stack pointer */
    __set_MSP(app_sp);

    /* Jump to app */
    void (*app_reset)(void) = (void (*)(void))app_pc;
    app_reset();
}

/* ========================================================================
 *  Bootloader main
 * ======================================================================== */
void bootloader_check_and_launch(void)
{
    iwdg_feed();

    /* Read current config */
    config_area_t cfg;
    memcpy(&cfg, (const void *)CONFIG_ADDR, sizeof(cfg));

    /* Increment boot count (rollback detection) */
    uint32_t new_boot_count = cfg.boot_count + 1;
    flash_erase_range(CONFIG_ADDR + offsetof(config_area_t, boot_count), 4);
    flash_program(CONFIG_ADDR + offsetof(config_area_t, boot_count),
                  (const uint8_t *)&new_boot_count, 4);
    cfg.boot_count = new_boot_count;

    /* Check for update request */
    if (cfg.update_magic == CONFIG_MAGIC_UPDATE_REQUEST) {
        uint32_t fw_size = cfg.new_fw_size;
        uint32_t expected_crc = cfg.new_fw_crc;
        uint32_t new_version = cfg.new_fw_version;
        uint32_t last_good = cfg.last_good_version;

        /* Prevent downgrade below last good version */
        if (last_good != 0xFFFFFFFF && new_version < last_good) {
            /* Downgrade attempt — clear flag, keep current app */
            flash_erase_range(CONFIG_ADDR, sizeof(config_area_t));
            config_area_t clean = {0};
            clean.update_magic = CONFIG_MAGIC_NO_UPDATE;
            clean.last_good_version = last_good;
            flash_program(CONFIG_ADDR, (const uint8_t *)&clean, sizeof(uint32_t) * 2);
            bootloader_jump_to_app();
            return;
        }

        /* Erase App area */
        if (flash_erase_range(APP_ADDR, fw_size) != HAL_OK) {
            while (1) {}
        }

        /* Copy Download → App */
        const uint8_t *src = (const uint8_t *)DOWNLOAD_ADDR;
        if (flash_program(APP_ADDR, src, fw_size) != HAL_OK) {
            while (1) {}
        }

        /* Verify CRC32 of the firmware payload (excluding header on STM32 side) */
        uint32_t actual_crc = crc32((const uint8_t *)(APP_ADDR + FW_HEADER_SIZE),
                                    fw_size - FW_HEADER_SIZE);
        if (actual_crc == expected_crc) {
            /* Success — record version and clear update flag */
            flash_erase_range(CONFIG_ADDR, sizeof(config_area_t));
            config_area_t new_cfg = {0};
            new_cfg.update_magic = CONFIG_MAGIC_UPDATE_SUCCESS;
            new_cfg.last_good_version = new_version;
            flash_program(CONFIG_ADDR, (const uint8_t *)&new_cfg, sizeof(uint32_t) * 3);
        } else {
            /* CRC mismatch — clear flag, keep last good version */
            flash_erase_range(CONFIG_ADDR, sizeof(config_area_t));
            config_area_t rollback_cfg = {0};
            rollback_cfg.update_magic = CONFIG_MAGIC_NO_UPDATE;
            rollback_cfg.last_good_version = last_good;
            flash_program(CONFIG_ADDR, (const uint8_t *)&rollback_cfg, sizeof(uint32_t) * 2);
        }
    }

    /* If boot count exceeds threshold, rollback (firmware didn't clear the counter) */
    if (cfg.boot_count > 3 && cfg.last_good_version != 0xFFFFFFFF) {
        /* TODO: implement full rollback — restore last good firmware from a backup slot */
        /* For now, just continue booting */
    }

    /* Jump to application */
    bootloader_jump_to_app();
}
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- SPI block protocol ---- */
#define SPI_BLOCK_MAGIC     0xA5U
#define SPI_BLOCK_SIZE      256U
#define SPI_BLOCK_HEADER    5U    /* magic(1) + index(2) + size(2) */
#define SPI_BLOCK_FOOTER    2U    /* CRC16(2) */
#define SPI_PACKET_SIZE     (SPI_BLOCK_HEADER + SPI_BLOCK_SIZE + SPI_BLOCK_FOOTER)

/* ---- UART command protocol ---- */
#define UART_FRAME_HEADER1  0xAAU
#define UART_FRAME_HEADER2  0x55U

typedef enum {
    OTA_CMD_START   = 0x01,  /* payload: firmware_size[4]+crc32[4]+version[4] */
    OTA_CMD_DATA    = 0x02,  /* payload: last_ack_block[2] */
    OTA_CMD_VERIFY  = 0x03,  /* request CRC32 verification */
    OTA_CMD_ABORT   = 0x04,  /* abort update */
    OTA_CMD_COMPLETE = 0x05, /* STM32→ESP32: OTA done, rebooting */
    OTA_CMD_BOOTED  = 0x06,  /* STM32→ESP32: boot complete */
    OTA_CMD_ERROR   = 0xFE,  /* STM32→ESP32: error_code[2] */
} ota_cmd_t;

/* ---- OTA state machine ---- */
typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_STARTED,
    OTA_STATE_RECEIVING,
    OTA_STATE_VERIFYING,
    OTA_STATE_COMPLETE,
    OTA_STATE_ERROR,
} ota_state_t;

/* ---- Public API ---- */
void ota_receiver_init(void);
void ota_receiver_process(void);
ota_state_t ota_receiver_get_state(void);
void ota_send_booted(void);

#ifdef __cplusplus
}
#endif
#include "ota_receiver.h"
#include "bootloader.h"
#include "main.h"
#include <stdio.h>
#include <string.h>

extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart2;
extern SPI_HandleTypeDef hspi1;
extern DMA_HandleTypeDef hdma_spi1_rx;

/* ---- OTA state ---- */
static ota_state_t s_state = OTA_STATE_IDLE;
static uint32_t s_fw_size = 0, s_fw_crc = 0, s_fw_version = 0, s_rx_bytes = 0;

/* ---- SPI DMA ping-pong ---- */
static uint8_t s_spi_a[SPI_PACKET_SIZE], s_spi_b[SPI_PACKET_SIZE];
static volatile uint8_t *s_spi_act = NULL;
static volatile uint8_t s_spi_rdy = 0;

/* ---- UART2 interrupt RX ---- */
static uint8_t s_ub, s_ubuf[64];
static volatile uint8_t s_uidx = 0;

static void uart2_rx_handler(uint8_t b);

/* ========================================================================
 *  CRC16
 * ======================================================================== */
static uint16_t crc16(const uint8_t *d, uint32_t n)
{
    uint16_t c = 0xFFFF;
    for (uint32_t i = 0; i < n; i++) {
        c ^= (uint16_t)d[i] << 8;
        for (int j = 0; j < 8; j++) c = (c & 0x8000) ? (c << 1) ^ 0x1021 : (c << 1);
    }
    return c;
}

/* ========================================================================
 *  Send response over USART2
 * ======================================================================== */
static void uart_send(ota_cmd_t cmd, const uint8_t *d, uint16_t n)
{
    uint8_t f[64];
    f[0] = 0xAA; f[1] = 0x55; f[2] = (uint8_t)cmd;
    f[3] = (uint8_t)(n >> 8); f[4] = (uint8_t)n;
    if (d && n) memcpy(f + 5, d, n);
    uint16_t c = crc16(f, 5 + n);
    f[5 + n] = (uint8_t)(c >> 8); f[6 + n] = (uint8_t)c;
    HAL_UART_Transmit(&huart2, f, 7 + n, 100);
}

/* ========================================================================
 *  UART frame parser
 * ======================================================================== */
static void parse_frame(const uint8_t *f)
{
    if (f[0] != 0xAA || f[1] != 0x55) return;
    uint16_t dl = ((uint16_t)f[3] << 8) | f[4];
    if (crc16(f, 5 + dl) != (((uint16_t)f[5+dl] << 8) | f[6+dl])) return;

    switch (f[2]) {
    case OTA_CMD_START:
        if (dl >= 12) {
            s_fw_size = ((uint32_t)f[5] << 24) | ((uint32_t)f[6] << 16) | ((uint32_t)f[7] << 8) | f[8];
            s_fw_crc  = ((uint32_t)f[9] << 24) | ((uint32_t)f[10] << 16) | ((uint32_t)f[11] << 8) | f[12];
            s_fw_version = ((uint32_t)f[13] << 24) | ((uint32_t)f[14] << 16) | ((uint32_t)f[15] << 8) | f[16];
            if (s_fw_size == 0 || s_fw_size > DOWNLOAD_SIZE) { uart_send(OTA_CMD_ERROR, NULL, 0); return; }
            s_rx_bytes = 0; s_state = OTA_STATE_STARTED;

            /* Erase download area */
            HAL_FLASH_Unlock();
            FLASH_EraseInitTypeDef ee = {0}; uint32_t se = 0;
            uint32_t end = DOWNLOAD_ADDR + s_fw_size - 1;
            for (uint32_t s = 0; s < 12; s++) {
                uint32_t ss, sz;
                if (s < 4) { ss = 0x08000000 + s*0x4000; sz = 0x4000; }
                else if (s == 4) { ss = 0x08010000; sz = 0x10000; }
                else { ss = 0x08020000 + (s-5)*0x20000; sz = 0x20000; }
                if (DOWNLOAD_ADDR < ss + sz && end >= ss) {
                    ee.TypeErase = FLASH_TYPEERASE_SECTORS; ee.Sector = s;
                    ee.NbSectors = 1; ee.VoltageRange = FLASH_VOLTAGE_RANGE_3;
                    IWDG->KR = 0xAAAA;
                    HAL_FLASHEx_Erase(&ee, &se);
                }
            }
            HAL_FLASH_Lock();

            uart_send(OTA_CMD_START, NULL, 0);
            HAL_UART_Transmit(&huart1, (uint8_t *)"\r\n[OTA START]\r\n", 14, 100);
        }
        break;
    case OTA_CMD_ABORT: s_state = OTA_STATE_IDLE; break;
    case OTA_CMD_VERIFY:
        if (dl >= 8) {
            s_fw_crc = ((uint32_t)f[9] << 24) | ((uint32_t)f[10] << 16) | ((uint32_t)f[11] << 8) | f[12];
        }
        s_state = OTA_STATE_VERIFYING;
        break;
    }
}

/* ========================================================================
 *  HAL callbacks
 * ======================================================================== */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *h)
{
    if (h->Instance == USART2) {
        uart2_rx_handler(s_ub);
        HAL_UART_Receive_IT(&huart2, &s_ub, 1);  /* re-arm */
    }
}

void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *h)
{
    if (h->Instance == SPI1) s_spi_rdy = 1;
}

/* UART2 byte processor */
void uart2_rx_handler(uint8_t b)
{
    if (s_uidx == 0 && b != 0xAA) return;
    s_ubuf[s_uidx++] = b;
    if (s_uidx >= 5) {
        uint16_t dl = ((uint16_t)s_ubuf[3] << 8) | s_ubuf[4];
        uint16_t fl = 5 + dl + 2;
        if (fl > sizeof(s_ubuf)) { s_uidx = 0; return; }
        if (s_uidx >= fl) { parse_frame(s_ubuf); s_uidx = 0; }
    }
}

/* ========================================================================
 *  SPI + Flash
 * ======================================================================== */
static void spi_start(volatile uint8_t *b) { s_spi_rdy = 0; s_spi_act = b; HAL_SPI_Receive_DMA(&hspi1, (uint8_t *)b, SPI_PACKET_SIZE); }

static HAL_StatusTypeDef fw(uint32_t a, const uint8_t *d, uint32_t s)
{
    HAL_FLASH_Unlock();
    for (uint32_t i = 0; i < s; i += 4) {
        uint32_t w; uint32_t r = s - i;
        if (r >= 4) w = *(uint32_t *)(d + i); else { w = 0; memcpy(&w, d + i, r); }
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, a + i, w) != HAL_OK) { HAL_FLASH_Lock(); return HAL_ERROR; }
    }
    HAL_FLASH_Lock(); return HAL_OK;
}

static int spi_block_cnt = 0;

static void proc_spi(const uint8_t *b)
{
    /* Debug: indicate SPI received */
    HAL_UART_Transmit(&huart1, (uint8_t *)"\r\n[SPI]\r\n", 8, 100);

    if (b[0] != 0xA5) {
        HAL_UART_Transmit(&huart1, (uint8_t *)"BAD_MAGIC\r\n", 11, 100);
        return;
    }
    uint16_t bi = ((uint16_t)b[1] << 8) | b[2], bs = ((uint16_t)b[3] << 8) | b[4];
    if (bs == 0 || bs > SPI_BLOCK_SIZE) return;
    const uint8_t *d = b + SPI_BLOCK_HEADER;
    uint16_t ec = ((uint16_t)b[SPI_BLOCK_HEADER + bs] << 8) | b[SPI_BLOCK_HEADER + bs + 1];
    if (crc16(d, bs) != ec) return;
    if (fw(DOWNLOAD_ADDR + (uint32_t)bi * SPI_BLOCK_SIZE, d, bs) != HAL_OK) return;
    s_rx_bytes += bs;
    uart_send(OTA_CMD_DATA, b + 1, 2);
}

/* ========================================================================
 *  Public API
 * ======================================================================== */
void ota_receiver_init(void)
{
    s_state = OTA_STATE_IDLE; s_uidx = 0; s_spi_rdy = 0; s_rx_bytes = 0;
    HAL_UART_Receive_IT(&huart2, &s_ub, 1);
}

void ota_receiver_process(void)
{
    /* Start SPI DMA when OTA begins */
    if (s_state == OTA_STATE_STARTED && s_spi_act == NULL) {
        spi_start(s_spi_a);
        s_state = OTA_STATE_RECEIVING;
    }

    if (s_spi_rdy) {
        s_spi_rdy = 0;
        proc_spi((const uint8_t *)s_spi_act);
        if (s_state == OTA_STATE_STARTED || s_state == OTA_STATE_RECEIVING) {
            s_state = OTA_STATE_RECEIVING;
            spi_start(s_spi_act == s_spi_a ? s_spi_b : s_spi_a);
        }
    }

    if (s_state == OTA_STATE_VERIFYING) {
        uint32_t crc = 0xFFFFFFFF;
        const uint8_t *p = (const uint8_t *)DOWNLOAD_ADDR;
        for (uint32_t i = 0; i < s_rx_bytes; i++) {
            crc ^= p[i];
            for (int j = 0; j < 8; j++) crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320 : (crc >> 1);
        }
        crc = ~crc;
        {
            char dbg[64];
            snprintf(dbg, sizeof(dbg), "\r\nCRC: calc=0x%08lX expect=0x%08lX sz=%lu\r\n",
                     crc, s_fw_crc, s_rx_bytes);
            HAL_UART_Transmit(&huart1, (uint8_t *)dbg, strlen(dbg), 100);
        }
        /* Print first 32 bytes at download addr for verification */
        {
            const uint8_t *dp = (const uint8_t *)DOWNLOAD_ADDR;
            char hex[100];
            int p = 0;
            for (int i = 0; i < 32 && p < 90; i++) {
                p += snprintf(hex + p, sizeof(hex) - p, "%02X ", dp[i]);
            }
            hex[p++] = '\r'; hex[p++] = '\n'; hex[p] = 0;
            HAL_UART_Transmit(&huart1, (uint8_t *)hex, p, 100);
        }
        if (crc == s_fw_crc && s_rx_bytes == s_fw_size) {
            HAL_FLASH_Unlock();
            FLASH_EraseInitTypeDef e = {0}; uint32_t se = 0;
            e.TypeErase = FLASH_TYPEERASE_SECTORS; e.Sector = 11; e.NbSectors = 1;
            e.VoltageRange = FLASH_VOLTAGE_RANGE_3;
            HAL_FLASHEx_Erase(&e, &se); HAL_FLASH_Lock();
            uint32_t c[4] = { CONFIG_MAGIC_UPDATE_REQUEST, s_fw_size, s_fw_crc, s_fw_version };
            HAL_FLASH_Unlock();
            for (int i = 0; i < 4; i++) HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, CONFIG_ADDR + i*4, c[i]);
            HAL_FLASH_Lock();
            s_state = OTA_STATE_COMPLETE;
            uart_send(OTA_CMD_COMPLETE, NULL, 0);

            /* Standard Cortex-M system reset */
            __DSB();
            SCB->AIRCR = ((0x5FAUL << 16U) | SCB_AIRCR_SYSRESETREQ_Msk);
            __DSB();
            while(1);
        } else { uart_send(OTA_CMD_ERROR, NULL, 0); s_state = OTA_STATE_ERROR; }
    }
}

ota_state_t ota_receiver_get_state(void) { return s_state; }

void ota_send_booted(void) {
    uint8_t f[7];
    f[0] = 0xAA; f[1] = 0x55; f[2] = OTA_CMD_BOOTED; f[3] = 0; f[4] = 0;
    uint16_t c = crc16(f, 5); f[5] = (uint8_t)(c >> 8); f[6] = (uint8_t)c;
    HAL_UART_Transmit(&huart2, f, 7, 100);
}
#ifndef __OLED_H__
#define __OLED_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "oled_data.h"
#include <stdarg.h>
#include <stdint.h>

#define OLED_WIDTH          128U        /* OLED 屏幕宽度 */
#define OLED_HEIGHT         64U         /* OLED 屏幕高度 */
#define OLED_PAGE_COUNT     8U          /* OLED 页数 */
#define OLED_I2C_ADDR       (0x3CU << 1)/* OLED I2C 地址 */

void OLED_WriteCommand(uint8_t command);
void OLED_WriteData(uint8_t data);
void OLED_SetCursor(uint8_t x, uint8_t page);

void OLED_Init(void);
void OLED_Clear(void);
void OLED_AreaClear(int16_t x, int16_t y, int16_t x1, int16_t y1);
void OLED_AreaReversal(int16_t x, int16_t y, int16_t x1, int16_t y1);
void OLED_AreaRefresh(int16_t x, int16_t y, int16_t x1, int16_t y1);
void OLED_Update(void);

void OLED_ShowChar(int16_t x, int16_t y, uint32_t number);
void OLED_ShowString(int16_t x, int16_t y, const char *string);
void OLED_Printf(int16_t x, int16_t y, const char *format, ...);
void OLED_ShowChar6x8(int16_t x, int16_t y, char character);
void OLED_ShowString6x8(int16_t x, int16_t y, const char *string);
void OLED_Printf6x8(int16_t x, int16_t y, const char *format, ...);
void OLED_ShowChar5x8(int16_t x, int16_t y, char character);
void OLED_ShowString5x8(int16_t x, int16_t y, const char *string);
void OLED_Printf5x8(int16_t x, int16_t y, const char *format, ...);
void OLED_ShowImage(uint8_t x, uint8_t y, uint8_t length, uint8_t width, const uint8_t image[]);

/* Compatibility aliases for the reference code style. */
#define OLED_init          OLED_Init

#ifdef __cplusplus
}
#endif

#endif

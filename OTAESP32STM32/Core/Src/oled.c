#include "oled.h"

#include "i2c.h"
#include <stdio.h>
#include <string.h>

static uint8_t OLED_DisplayBuf[OLED_PAGE_COUNT][OLED_WIDTH];
static const uint8_t OLED_Font6x8_Space[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static const uint8_t *OLED_GetFont6x8(char character)
{
    static const uint8_t font_colon[6] = {0x00, 0x36, 0x36, 0x00, 0x00, 0x00};
    static const uint8_t font_question[6] = {0x02, 0x01, 0x51, 0x09, 0x06, 0x00};
    static const uint8_t font_minus[6] = {0x08, 0x08, 0x08, 0x08, 0x08, 0x00};
    static const uint8_t font_comma[6] = {0x00, 0x80, 0x60, 0x00, 0x00, 0x00};
    static const uint8_t font_dot[6] = {0x00, 0x60, 0x60, 0x00, 0x00, 0x00};
    static const uint8_t font_equal[6] = {0x14, 0x14, 0x14, 0x14, 0x14, 0x00};
    static const uint8_t font_digits[10][6] = {
        {0x3E, 0x51, 0x49, 0x45, 0x3E, 0x00},
        {0x00, 0x42, 0x7F, 0x40, 0x00, 0x00},
        {0x42, 0x61, 0x51, 0x49, 0x46, 0x00},
        {0x21, 0x41, 0x45, 0x4B, 0x31, 0x00},
        {0x18, 0x14, 0x12, 0x7F, 0x10, 0x00},
        {0x27, 0x45, 0x45, 0x45, 0x39, 0x00},
        {0x3C, 0x4A, 0x49, 0x49, 0x30, 0x00},
        {0x01, 0x71, 0x09, 0x05, 0x03, 0x00},
        {0x36, 0x49, 0x49, 0x49, 0x36, 0x00},
        {0x06, 0x49, 0x49, 0x29, 0x1E, 0x00}
    };
    static const uint8_t font_upper[26][6] = {
        {0x7E, 0x11, 0x11, 0x11, 0x7E, 0x00},
        {0x7F, 0x49, 0x49, 0x49, 0x36, 0x00},
        {0x3E, 0x41, 0x41, 0x41, 0x22, 0x00},
        {0x7F, 0x41, 0x41, 0x22, 0x1C, 0x00},
        {0x7F, 0x49, 0x49, 0x49, 0x41, 0x00},
        {0x7F, 0x09, 0x09, 0x09, 0x01, 0x00},
        {0x3E, 0x41, 0x49, 0x49, 0x7A, 0x00},
        {0x7F, 0x08, 0x08, 0x08, 0x7F, 0x00},
        {0x00, 0x41, 0x7F, 0x41, 0x00, 0x00},
        {0x20, 0x40, 0x41, 0x3F, 0x01, 0x00},
        {0x7F, 0x08, 0x14, 0x22, 0x41, 0x00},
        {0x7F, 0x40, 0x40, 0x40, 0x40, 0x00},
        {0x7F, 0x02, 0x0C, 0x02, 0x7F, 0x00},
        {0x7F, 0x04, 0x08, 0x10, 0x7F, 0x00},
        {0x3E, 0x41, 0x41, 0x41, 0x3E, 0x00},
        {0x7F, 0x09, 0x09, 0x09, 0x06, 0x00},
        {0x3E, 0x41, 0x51, 0x21, 0x5E, 0x00},
        {0x7F, 0x09, 0x19, 0x29, 0x46, 0x00},
        {0x46, 0x49, 0x49, 0x49, 0x31, 0x00},
        {0x01, 0x01, 0x7F, 0x01, 0x01, 0x00},
        {0x3F, 0x40, 0x40, 0x40, 0x3F, 0x00},
        {0x1F, 0x20, 0x40, 0x20, 0x1F, 0x00},
        {0x7F, 0x20, 0x18, 0x20, 0x7F, 0x00},
        {0x63, 0x14, 0x08, 0x14, 0x63, 0x00},
        {0x07, 0x08, 0x70, 0x08, 0x07, 0x00},
        {0x61, 0x51, 0x49, 0x45, 0x43, 0x00}
    };

    if ((character >= '0') && (character <= '9'))
    {
        return font_digits[character - '0'];
    }
    if ((character >= 'A') && (character <= 'Z'))
    {
        return font_upper[character - 'A'];
    }
    if ((character >= 'a') && (character <= 'z'))
    {
        return font_upper[character - 'a'];
    }

    switch (character)
    {
        case ' ':
            return OLED_Font6x8_Space;
        case ':':
            return font_colon;
        case '?':
            return font_question;
        case '-':
            return font_minus;
        case ',':
            return font_comma;
        case '.':
            return font_dot;
        case '=':
            return font_equal;
        default:
            return OLED_Font6x8_Space;
    }
}

static int16_t OLED_Clamp(int16_t value, int16_t min, int16_t max)
{
    if (value < min)
    {
        return min;
    }
    if (value > max)
    {
        return max;
    }
    return value;
}

static void OLED_SetPixel(int16_t x, int16_t y, uint8_t on)
{
    uint8_t page;
    uint8_t bit;

    if ((x < 0) || (x >= OLED_WIDTH) || (y < 0) || (y >= OLED_HEIGHT))
    {
        return;
    }

    page = (uint8_t)(y / 8);
    bit = (uint8_t)(1U << (y % 8));

    if (on != 0U)
    {
        OLED_DisplayBuf[page][x] |= bit;
    }
    else
    {
        OLED_DisplayBuf[page][x] &= (uint8_t)(~bit);
    }
}

static void OLED_WriteColumnByte(int16_t x, int16_t y, uint8_t data)
{
    uint8_t bit_index;

    for (bit_index = 0; bit_index < 8U; bit_index++)
    {
        OLED_SetPixel(x, y + bit_index, (uint8_t)((data >> bit_index) & 0x01U));
    }
}

static void OLED_WritePageRange(uint8_t start_page, uint8_t end_page, uint8_t start_x, uint8_t end_x)
{
    uint8_t page;
    uint8_t tx_buffer[OLED_WIDTH + 1U];

    if ((start_page >= OLED_PAGE_COUNT) || (end_page >= OLED_PAGE_COUNT) || (start_page > end_page))
    {
        return;
    }

    if ((start_x >= OLED_WIDTH) || (end_x >= OLED_WIDTH) || (start_x > end_x))
    {
        return;
    }

    tx_buffer[0] = 0x40U;
    for (page = start_page; page <= end_page; page++)
    {
        uint16_t size;

        OLED_SetCursor(start_x, page);
        memcpy(&tx_buffer[1], &OLED_DisplayBuf[page][start_x], (size_t)(end_x - start_x + 1U));
        size = (uint16_t)(end_x - start_x + 2U);
        (void)HAL_I2C_Master_Transmit(&hi2c1, OLED_I2C_ADDR, tx_buffer, size, 50U);
    }
}

void OLED_WriteCommand(uint8_t command)
{
    uint8_t tx_buffer[2];

    tx_buffer[0] = 0x00U;
    tx_buffer[1] = command;
    (void)HAL_I2C_Master_Transmit(&hi2c1, OLED_I2C_ADDR, tx_buffer, 2U, 20U);
}

void OLED_WriteData(uint8_t data)
{
    uint8_t tx_buffer[2];

    tx_buffer[0] = 0x40U;
    tx_buffer[1] = data;
    (void)HAL_I2C_Master_Transmit(&hi2c1, OLED_I2C_ADDR, tx_buffer, 2U, 20U);
}

void OLED_SetCursor(uint8_t x, uint8_t page)
{
    OLED_WriteCommand((uint8_t)(0x0FU & x));
    OLED_WriteCommand((uint8_t)(0x10U | ((x >> 4) & 0x0FU)));
    OLED_WriteCommand((uint8_t)(0xB0U | (page & 0x07U)));
}

void OLED_Init(void)
{
    memset(OLED_DisplayBuf, 0, sizeof(OLED_DisplayBuf));
    HAL_Delay(100);

    OLED_WriteCommand(0xAE);
    OLED_WriteCommand(0xD5);
    OLED_WriteCommand(0x80);
    OLED_WriteCommand(0xA8);
    OLED_WriteCommand(0x3F);
    OLED_WriteCommand(0xD3);
    OLED_WriteCommand(0x00);
    OLED_WriteCommand(0x40);
    OLED_WriteCommand(0xA1);
    OLED_WriteCommand(0xC8);
    OLED_WriteCommand(0xDA);
    OLED_WriteCommand(0x12);
    OLED_WriteCommand(0x81);
    OLED_WriteCommand(0xCF);
    OLED_WriteCommand(0xD9);
    OLED_WriteCommand(0xF1);
    OLED_WriteCommand(0xDB);
    OLED_WriteCommand(0x30);
    OLED_WriteCommand(0xA4);
    OLED_WriteCommand(0xA6);
    OLED_WriteCommand(0x8D);
    OLED_WriteCommand(0x14);
    OLED_WriteCommand(0xAF);

    OLED_Update();
    HAL_Delay(100);
}

void OLED_Clear(void)
{
    memset(OLED_DisplayBuf, 0, sizeof(OLED_DisplayBuf));
    OLED_Update();
}

void OLED_AreaClear(int16_t x, int16_t y, int16_t x1, int16_t y1)
{
    int16_t row;
    int16_t col;

    x = OLED_Clamp(x, 0, OLED_WIDTH);
    x1 = OLED_Clamp(x1, 0, OLED_WIDTH);
    y = OLED_Clamp(y, 0, OLED_HEIGHT);
    y1 = OLED_Clamp(y1, 0, OLED_HEIGHT);

    if ((x >= x1) || (y >= y1))
    {
        return;
    }

    for (row = y; row < y1; row++)
    {
        for (col = x; col < x1; col++)
        {
            OLED_SetPixel(col, row, 0U);
        }
    }
}

void OLED_AreaReversal(int16_t x, int16_t y, int16_t x1, int16_t y1)
{
    int16_t row;
    int16_t col;

    x = OLED_Clamp(x, 0, OLED_WIDTH);
    x1 = OLED_Clamp(x1, 0, OLED_WIDTH);
    y = OLED_Clamp(y, 0, OLED_HEIGHT);
    y1 = OLED_Clamp(y1, 0, OLED_HEIGHT);

    if ((x >= x1) || (y >= y1))
    {
        return;
    }

    for (row = y; row < y1; row++)
    {
        for (col = x; col < x1; col++)
        {
            uint8_t page = (uint8_t)(row / 8);
            uint8_t bit = (uint8_t)(1U << (row % 8));
            OLED_DisplayBuf[page][col] ^= bit;
        }
    }
}

void OLED_AreaRefresh(int16_t x, int16_t y, int16_t x1, int16_t y1)
{
    uint8_t start_page;
    uint8_t end_page;
    uint8_t start_x;
    uint8_t end_x;

    x = OLED_Clamp(x, 0, OLED_WIDTH);
    x1 = OLED_Clamp(x1, 0, OLED_WIDTH);
    y = OLED_Clamp(y, 0, OLED_HEIGHT);
    y1 = OLED_Clamp(y1, 0, OLED_HEIGHT);

    if ((x >= x1) || (y >= y1))
    {
        return;
    }

    start_page = (uint8_t)(y / 8);
    end_page = (uint8_t)((y1 - 1) / 8);
    start_x = (uint8_t)x;
    end_x = (uint8_t)(x1 - 1);

    OLED_WritePageRange(start_page, end_page, start_x, end_x);
}

void OLED_Update(void)
{
    OLED_WritePageRange(0U, OLED_PAGE_COUNT - 1U, 0U, OLED_WIDTH - 1U);
}

void OLED_ShowChar(int16_t x, int16_t y, uint32_t number)
{
    uint8_t col;
    const uint8_t *glyph;

    if ((number >= 95U) || (x >= OLED_WIDTH) || (y >= OLED_HEIGHT))
    {
        return;
    }

    glyph = OLED_F8x16[number];
    for (col = 0U; col < 8U; col++)
    {
        OLED_WriteColumnByte(x + col, y, glyph[col]);
        OLED_WriteColumnByte(x + col, y + 8, glyph[col + 8U]);
    }
}

void OLED_ShowString(int16_t x, int16_t y, const char *string)
{
    if (string == NULL)
    {
        return;
    }

    while ((*string != '\0') && (x <= (OLED_WIDTH - 8)))
    {
        if ((*string >= 32) && (*string <= 126))
        {
            OLED_ShowChar(x, y, (uint32_t)(*string - 32));
        }
        string++;
        x += 8;
    }
}

void OLED_Printf(int16_t x, int16_t y, const char *format, ...)
{
    char string[64];
    va_list args;

    va_start(args, format);
    (void)vsnprintf(string, sizeof(string), format, args);
    va_end(args);

    OLED_ShowString(x, y, string);
}

void OLED_ShowChar6x8(int16_t x, int16_t y, char character)
{
    uint8_t col;
    const uint8_t *glyph;

    if ((x >= OLED_WIDTH) || (y >= OLED_HEIGHT))
    {
        return;
    }

    glyph = OLED_GetFont6x8(character);
    for (col = 0U; col < 6U; col++)
    {
        OLED_WriteColumnByte(x + col, y, glyph[col]);
    }
}

void OLED_ShowString6x8(int16_t x, int16_t y, const char *string)
{
    if (string == NULL)
    {
        return;
    }

    while ((*string != '\0') && (x <= (OLED_WIDTH - 6)))
    {
        OLED_ShowChar6x8(x, y, *string);
        string++;
        x += 6;
    }
}

void OLED_Printf6x8(int16_t x, int16_t y, const char *format, ...)
{
    char string[64];
    va_list args;

    va_start(args, format);
    (void)vsnprintf(string, sizeof(string), format, args);
    va_end(args);

    OLED_ShowString6x8(x, y, string);
}

void OLED_ShowChar5x8(int16_t x, int16_t y, char character)
{
    uint8_t col;
    const uint8_t *glyph;

    if ((x >= OLED_WIDTH) || (y >= OLED_HEIGHT))
    {
        return;
    }

    glyph = OLED_GetFont6x8(character);
    for (col = 0U; col < 5U; col++)
    {
        OLED_WriteColumnByte(x + col, y, glyph[col]);
    }
}

void OLED_ShowString5x8(int16_t x, int16_t y, const char *string)
{
    if (string == NULL)
    {
        return;
    }

    while ((*string != '\0') && (x <= (OLED_WIDTH - 5)))
    {
        OLED_ShowChar5x8(x, y, *string);
        string++;
        x += 5;
    }
}

void OLED_Printf5x8(int16_t x, int16_t y, const char *format, ...)
{
    char string[64];
    va_list args;

    va_start(args, format);
    (void)vsnprintf(string, sizeof(string), format, args);
    va_end(args);

    OLED_ShowString5x8(x, y, string);
}

void OLED_ShowImage(uint8_t x, uint8_t y, uint8_t length, uint8_t width, const uint8_t image[])
{
    uint8_t page_count;
    uint8_t page;
    uint8_t col;

    if ((image == NULL) || (length == 0U) || (width == 0U) || ((width % 8U) != 0U) || ((y % 8U) != 0U))
    {
        return;
    }

    page_count = (uint8_t)(width / 8U);
    for (page = 0U; page < page_count; page++)
    {
        for (col = 0U; col < length; col++)
        {
            OLED_WriteColumnByte((int16_t)x + col,
                                 (int16_t)y + ((int16_t)page * 8),
                                 image[(page * length) + col]);
        }
    }
}

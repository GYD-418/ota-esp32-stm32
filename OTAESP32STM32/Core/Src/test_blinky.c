/* Minimal test: blink PA0, print "HELLO" on USART1. No HAL needed. */
#include "stm32f407xx.h"   /* CMSIS register defs */

void SystemInit(void) {}   /* stub — startup calls this */

static void delay(int ms)
{
    for (int i = 0; i < ms * 16000; i++) { __NOP(); }
}

int main(void)
{
    /* Enable GPIOA + USART1 clocks */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

    /* PA0 = output */
    GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODER0) | GPIO_MODER_MODER0_0;

    /* PA9 = AF7 (USART1 TX) */
    GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODER9) | GPIO_MODER_MODER9_1;
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~GPIO_AFRH_AFSEL9_Msk) | (7 << GPIO_AFRH_AFSEL9_Pos);

    /* USART1: 115200 @ 16MHz */
    USART1->BRR = 16000000 / 115200;
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE;

    const char *msg = "HELLO\r\n";
    for (const char *p = msg; *p; p++) {
        while (!(USART1->SR & USART_SR_TXE)) {}
        USART1->DR = *p;
    }

    while (1) {
        GPIOA->ODR ^= GPIO_ODR_OD0;
        delay(200);
        IWDG->KR = 0xAAAA;  /* feed watchdog */
    }
}
/**
  * @brief  STM32F407 Bootloader — checks for firmware update, then launches app.
  */
#include "bootloader.h"
#include "stm32f4xx_hal.h"

static void iwdg_feed(void) { IWDG->KR = 0xAAAA; }

void SysTick_Handler(void)
{
    HAL_IncTick();
}

int main(void)
{
    /* LED ON = bootloader active */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODER0) | GPIO_MODER_MODER0_0;
    GPIOA->ODR |= GPIO_ODR_OD0;

    iwdg_feed();
    HAL_Init();
    iwdg_feed();

    bootloader_check_and_launch();

    while (1) { iwdg_feed(); }
}
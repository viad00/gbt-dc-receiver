/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : helpers.c
  * @brief          : Low-level timing and LED helpers
  ******************************************************************************
  */
/* USER CODE END Header */

#include "helpers.h"

#include "main.h"

uint32_t GetTS(void)
{
	return HAL_GetTick();
}

void DelayMs(uint32_t ms)
{
	HAL_Delay(ms);
}

void LED(uint8_t id)
{
	if (id == 1) HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin);
	if (id == 2) HAL_GPIO_TogglePin(LED2_GPIO_Port, LED2_Pin);
	if (id == 3) HAL_GPIO_TogglePin(LED3_GPIO_Port, LED3_Pin);
	if (id == 4) HAL_GPIO_TogglePin(LED4_GPIO_Port, LED4_Pin);
}
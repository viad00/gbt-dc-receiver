/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : can.c
  * @brief          : CAN low-level helpers and RX/TX wrappers
  ******************************************************************************
  */
/* USER CODE END Header */

#include "can.h"

#include "stm32f4xx_hal.h"
#include "main.h"

#include <string.h>
#include <stdio.h>

char TmpString[1024] = {0};
int TmpStringLen = 0;

static void DebugSendCanMsg(uint32_t id, uint8_t *buf, uint8_t len)
{
	if (!DebugCan_IsEnabled()) return;

	memset(TmpString, 0, sizeof(TmpString));
	TmpStringLen = snprintf(TmpString, sizeof(TmpString),
			"%lu,%08lX,true,Rx,0,%u,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X\n\r",
			(unsigned long)HAL_GetTick(),
			(unsigned long)id,
			(unsigned int)len,
			buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
	HAL_UART_Transmit_DMA(&huart4, (uint8_t *)TmpString, (uint16_t)TmpStringLen);
}

/* Strong implementation of the shared observer hook (weak default in
 * can_buffer.c): flash log + UART debug trace for every RX/TX frame. */
void CanFrameObserve(uint32_t id, uint8_t *buf, uint8_t len, bool is_tx)
{
	FlashLog_Write(id, buf, len, is_tx);
	DebugSendCanMsg(id, buf, len);
}

void CanSendMsg(uint32_t id, uint8_t *buf, uint8_t len)
{
	if (HAL_CAN_GetTxMailboxesFreeLevel(&hcan1) != 0) {
		CAN_TxHeaderTypeDef msgHeader;
		msgHeader.ExtId = id;
		msgHeader.DLC = len;
		msgHeader.TransmitGlobalTime = DISABLE;
		msgHeader.RTR = CAN_RTR_DATA;
		msgHeader.IDE = CAN_ID_EXT;

		uint32_t mailBoxNum = 0;
		HAL_CAN_AddTxMessage(&hcan1, &msgHeader, buf, &mailBoxNum);
		CanFrameObserve(id, buf, len, true);
	}
}

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
	CAN_RxHeaderTypeDef msgHeader;
	uint32_t msgId = 0;
	uint8_t msgData[8] = {0};

	HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &msgHeader, msgData);

	if (msgHeader.IDE == CAN_ID_EXT) {
		msgId = msgHeader.ExtId;
	} else {
		msgId = msgHeader.StdId;
	}
	CanRingBufferPush(msgId, msgData, msgHeader.DLC);
}

void CanInit(void)
{
	CAN_FilterTypeDef filter = {0};

	filter.FilterBank = 0;
	filter.FilterMode = CAN_FILTERMODE_IDMASK;
	filter.FilterScale = CAN_FILTERSCALE_32BIT;
	filter.FilterIdHigh = 0x0000;
	filter.FilterIdLow = 0x0000;
	filter.FilterMaskIdHigh = 0x0000;
	filter.FilterMaskIdLow = 0x0000;
	filter.FilterFIFOAssignment = CAN_FILTER_FIFO0;
	filter.FilterActivation = ENABLE;

	if (HAL_CAN_ConfigFilter(&hcan1, &filter) != HAL_OK) {
		Error_Handler();
	}

	if (HAL_CAN_Start(&hcan1) != HAL_OK) {
		Error_Handler();
	}
	if (HAL_CAN_ActivateNotification(&hcan1, CAN_IT_RX_FIFO0_MSG_PENDING) != HAL_OK) {
		Error_Handler();
	}
}
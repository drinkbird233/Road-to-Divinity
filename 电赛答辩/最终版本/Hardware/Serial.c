#include "stm32f10x.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "Motor.h"

char Serial_RxPacket[32];
uint8_t Serial_RxFlag;

extern float Target_RPM, Kp, Ki, Kd;
extern uint8_t MotorRun;
extern float ErrorInt;

void Serial_Printf(char *format, ...);

void ParseCmd(char *s)
{
	if (strcmp(s, "START") == 0)//完整匹配的指令
	{
		MotorRun = 1; ErrorInt = 0;
		Serial_Printf("OK START\r\n");
	}
	else if (strcmp(s, "STOP") == 0)
	{
		MotorRun = 0; Motor_SetPWM(0);
		Serial_Printf("OK STOP\r\n");
	}
	else if (strcmp(s, "UP") == 0)
	{
		Target_RPM += 10;
		if (Target_RPM > 3000) Target_RPM = 3000;
		Serial_Printf("OK TARGET=%.0f\r\n", Target_RPM);
	}
	else if (strcmp(s, "DOWN") == 0)
	{
		Target_RPM -= 10;
		if (Target_RPM < 0) Target_RPM = 0;
		Serial_Printf("OK TARGET=%.0f\r\n", Target_RPM);
	}
	else if (strncmp(s, "SPD=", 4) == 0)//只对比前四个字符
	{
		Target_RPM = atof(s + 4);//atof：把字符串数字转成浮点
		if (Target_RPM < 0) Target_RPM = 0;
		if (Target_RPM > 3000) Target_RPM = 3000;
		Serial_Printf("OK TARGET=%.0f\r\n", Target_RPM);
	}
	else if (strncmp(s, "KP=", 3) == 0)
	{
		Kp = atof(s + 3);
		Serial_Printf("OK KP=%.3f\r\n", Kp);
	}
	else if (strncmp(s, "KI=", 3) == 0)
	{
		Ki = atof(s + 3); ErrorInt = 0;
		Serial_Printf("OK KI=%.3f\r\n", Ki);
	}
	else if (strncmp(s, "KD=", 3) == 0)
	{
		Kd = atof(s + 3);
		Serial_Printf("OK KD=%.3f\r\n", Kd);
	}
}

void Serial_Init(void)
{
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);

	GPIO_InitTypeDef GPIO_InitStructure;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_9;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOA, &GPIO_InitStructure);

	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_10;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOA, &GPIO_InitStructure);

	USART_InitTypeDef USART_InitStructure;
	USART_InitStructure.USART_BaudRate = 115200;
	USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
	USART_InitStructure.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
	USART_InitStructure.USART_Parity = USART_Parity_No;
	USART_InitStructure.USART_StopBits = USART_StopBits_1;
	USART_InitStructure.USART_WordLength = USART_WordLength_8b;
	USART_Init(USART1, &USART_InitStructure);

	USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);

	NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);

	NVIC_InitTypeDef NVIC_InitStructure;
	NVIC_InitStructure.NVIC_IRQChannel = USART1_IRQn;
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
	NVIC_InitStructure.NVIC_IRQChannelSubPriority = 1;
	NVIC_Init(&NVIC_InitStructure);

	USART_Cmd(USART1, ENABLE);
}

static void Serial_SendByte(uint8_t Byte)
{
	USART_SendData(USART1, Byte);
	while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
}

static void Serial_SendString(char *String)
{
	uint8_t i;
	for (i = 0; String[i] != '\0'; i ++)
	{
		Serial_SendByte(String[i]);
	}
}

void Serial_Printf(char *format, ...)
{
	char String[100];
	va_list arg;
	va_start(arg, format);
	vsprintf(String, format, arg);
	va_end(arg);
	Serial_SendString(String);
}

uint8_t Serial_GetRxFlag(void)
{
	if (Serial_RxFlag)
	{
		Serial_RxFlag = 0;
		return 1;
	}
	return 0;
}

void USART1_IRQHandler(void)
{
	static uint8_t RxState = 0;
	static uint8_t pRxPacket = 0;

	if (USART_GetITStatus(USART1, USART_IT_RXNE) == SET)
	{
		uint8_t RxData = USART_ReceiveData(USART1);

		if (RxState == 0)
		{
			if (RxData == '@' && Serial_RxFlag == 0)
			{
				RxState = 1;
				pRxPacket = 0;
			}
		}
		else if (RxState == 1)
		{
			if (RxData == '#')
			{
				RxState = 0;
				Serial_RxPacket[pRxPacket] = '\0';
				Serial_RxFlag = 1;
			}
			else
			{
				Serial_RxPacket[pRxPacket] = RxData;
				pRxPacket ++;
			}
		}

		USART_ClearITPendingBit(USART1, USART_IT_RXNE);
	}
}

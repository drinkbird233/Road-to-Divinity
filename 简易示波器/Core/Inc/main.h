/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f1xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define KEYD_Pin GPIO_PIN_9
#define KEYD_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */
/* ---- Simple oscilloscope data model (used by tft.c UI and keys.c) ---- */
struct Oscilloscope
{
   uint8_t  showbit;          /* 1 = a fresh 300-sample frame is ready to draw */
   uint8_t  keyValue;         /* current key event */
   uint8_t  ouptputbit;       /* PWM square-wave output on/off */
   uint16_t outputFreq;       /* PWM square-wave frequency (Hz) */
   uint16_t pwmOut;           /* PWM compare value (duty) */
   uint32_t sampletime;       /* ADC sampling time (zoom) */
   uint32_t timerPeriod;      /* TIM2 ARR (PWM period) */
   __IO uint32_t gatherFreq;  /* measured input frequency (Hz) */
   float    vpp;              /* input peak-to-peak */
   float    voltageValue[300];/* converted voltages of one frame */
};
extern volatile struct Oscilloscope oscilloscope;

#define LED1       0x01
#define LED2       0x02
#define KEYPRESS   0x03
#define NoPRESS    0x04
#define KEY1PRESS  0x05
#define KEY2PRESS  0x06
#define KEY3PRESS  0x07
#define KEYAPRESS  0x08
#define KEYBPRESS  0x09
#define KEYDPRESS  0x10

void Key_Sacnf(volatile struct Oscilloscope *value);
void Key_Handle(volatile struct Oscilloscope *value);
void KEYD_SCAN(volatile struct Oscilloscope *value);
/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */

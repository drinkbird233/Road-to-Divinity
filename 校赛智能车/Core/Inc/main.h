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
#include "stm32f4xx_hal.h"

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
#define BUZZER_Pin GPIO_PIN_0
#define BUZZER_GPIO_Port GPIOC
#define KEY1_Pin GPIO_PIN_1
#define KEY1_GPIO_Port GPIOC
#define KEY2_Pin GPIO_PIN_2
#define KEY2_GPIO_Port GPIOC
#define KEY3_Pin GPIO_PIN_3
#define KEY3_GPIO_Port GPIOC
#define IMU_TX_Pin GPIO_PIN_2
#define IMU_TX_GPIO_Port GPIOA
#define IMU_RX_Pin GPIO_PIN_3
#define IMU_RX_GPIO_Port GPIOA
#define ENC_R_A_Pin GPIO_PIN_6
#define ENC_R_A_GPIO_Port GPIOA
#define ENC_R_B_Pin GPIO_PIN_7
#define ENC_R_B_GPIO_Port GPIOA
#define LED1_Pin GPIO_PIN_4
#define LED1_GPIO_Port GPIOC
#define LED2_Pin GPIO_PIN_5
#define LED2_GPIO_Port GPIOC
#define MOTOR_L_IN1_Pin GPIO_PIN_8
#define MOTOR_L_IN1_GPIO_Port GPIOE
#define MOTOR_L_PWM_Pin GPIO_PIN_9
#define MOTOR_L_PWM_GPIO_Port GPIOE
#define MOTOR_L_IN2_Pin GPIO_PIN_10
#define MOTOR_L_IN2_GPIO_Port GPIOE
#define MOTOR_R_PWM_Pin GPIO_PIN_11
#define MOTOR_R_PWM_GPIO_Port GPIOE
#define MOTOR_R_IN1_Pin GPIO_PIN_12
#define MOTOR_R_IN1_GPIO_Port GPIOE
#define MOTOR_R_IN2_Pin GPIO_PIN_13
#define MOTOR_R_IN2_GPIO_Port GPIOE
#define MOTOR_STBY_Pin GPIO_PIN_14
#define MOTOR_STBY_GPIO_Port GPIOE
#define BT_STATE_Pin GPIO_PIN_15
#define BT_STATE_GPIO_Port GPIOE
#define BT_TX_Pin GPIO_PIN_10
#define BT_TX_GPIO_Port GPIOB
#define BT_RX_Pin GPIO_PIN_11
#define BT_RX_GPIO_Port GPIOB
#define K230_TX_Pin GPIO_PIN_6
#define K230_TX_GPIO_Port GPIOC
#define K230_RX_Pin GPIO_PIN_7
#define K230_RX_GPIO_Port GPIOC
#define DBG_TX_Pin GPIO_PIN_9
#define DBG_TX_GPIO_Port GPIOA
#define DBG_RX_Pin GPIO_PIN_10
#define DBG_RX_GPIO_Port GPIOA
#define TRACK_SCL_Pin GPIO_PIN_3
#define TRACK_SCL_GPIO_Port GPIOD
#define TRACK_SDA_Pin GPIO_PIN_4
#define TRACK_SDA_GPIO_Port GPIOD
#define ENC_L_A_Pin GPIO_PIN_6
#define ENC_L_A_GPIO_Port GPIOB
#define ENC_L_B_Pin GPIO_PIN_7
#define ENC_L_B_GPIO_Port GPIOB
#define OLED_SCL_Pin GPIO_PIN_8
#define OLED_SCL_GPIO_Port GPIOB
#define OLED_SDA_Pin GPIO_PIN_9
#define OLED_SDA_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */

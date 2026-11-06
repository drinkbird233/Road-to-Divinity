/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
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

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "mp3_player.h" // 必须有，播放器核心
#include <stdio.h>
#include "fatfs.h"
#include "app_key.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

/* USER CODE END Variables */
/* Definitions for AudioTask */
osThreadId_t AudioTaskHandle;
const osThreadAttr_t AudioTask_attributes = {
  .name = "AudioTask",
  .stack_size = 6144 * 4,
  .priority = (osPriority_t) osPriorityAboveNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

void StartAudioTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of AudioTask */
  AudioTaskHandle = osThreadNew(StartAudioTask, NULL, &AudioTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartAudioTask */
/**
  * @brief  Function implementing the AudioTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartAudioTask */
void StartAudioTask(void *argument)
{
  /* USER CODE BEGIN StartAudioTask */
    FRESULT res;
 
    /* 2. 挂载 SD 卡 */
    res = f_mount(&SDFatFS, SDPath, 1);
    printf("mount = %d\r\n", res);
    if (res != FR_OK) {
        printf("AudioTask: mount failed, suspend\r\n");
        vTaskSuspend(NULL);
    }

    app_key_init();

	int8_t play_ret = mp3_player_play(&hi2s2,"0:/TEST.MP3");
	
	if (play_ret != MP3_PLAYER_OK) {
      printf("AudioTask: Play failed = %d\r\n", play_ret);
      vTaskSuspend(NULL); // 播放失败，把自己挂起（不占用 CPU）
  }
	printf("AudioTask: Playback started\r\n");

// 2. 疯狂喂数据（永动机引擎）
  while (1)
  {
      // mp3_player_poll() 返回 1 代表还在放，返回 0 代表放完了
      if (!mp3_player_poll()) {
          printf("AudioTask: Playback done\r\n");
          break; // 歌放完了，打破循环
      }

      app_key_poll();

      // 让出 CPU；半区 26ms，1ms 轮询余量充足
      osDelay(1);
  }

  // 3. 播放完毕，任务自裁（挂起自己）
  vTaskSuspend(NULL); 
  /* USER CODE END StartAudioTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */


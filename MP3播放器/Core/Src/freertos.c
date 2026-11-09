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
#include "playlist.h"
#include "settings.h"
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
static uint8_t audio_play_index(uint8_t index);
static void settings_store(void);
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
  app_key_queue_init();
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of AudioTask */
  AudioTaskHandle = osThreadNew(StartAudioTask, NULL, &AudioTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  {
    static const osThreadAttr_t KeyTask_attributes = {
      .name = "KeyTask",
      .stack_size = 1024,                               /* 字节 */
      .priority = (osPriority_t) osPriorityNormal,
    };
    (void)osThreadNew(app_key_task, NULL, &KeyTask_attributes);
  }
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
    uint8_t cmd;
    uint8_t started = 0;
    uint8_t saved_vol;
    uint8_t saved_track;

    (void)argument;

    /* 1. 挂载 SD 卡 */
    res = f_mount(&SDFatFS, SDPath, 1);
    printf("mount = %d\r\n", res);
    if (res != FR_OK) {
        printf("AudioTask: mount failed, suspend\r\n");
        vTaskSuspend(NULL);
    }

    /* 2. 扫描根目录 MP3，生成播放列表 */
    printf("playlist = %u\r\n", (unsigned)playlist_scan());
    if (playlist_count() == 0) {
        printf("AudioTask: no mp3 found\r\n");
    }

    /* 3. 恢复上电记忆：音量 + 曲目 */
    if (settings_load(&saved_vol, &saved_track)) {
        if (saved_vol < APP_VOL_LEVELS) {
            app_key_volume_set(saved_vol);
        }
        if (saved_track < playlist_count()) {
            playlist_set_current(saved_track);
        }
    }

    if (playlist_count() > 0) {
        started = audio_play_index(playlist_current());
    }

    for (;;)
    {
        /* 3. 待机：阻塞等命令，不占 CPU */
        if (!started) {
            if (!app_key_wait_cmd(0xFFFFFFFFu, &cmd)) continue;

            if (cmd == KEY_CMD_TOGGLE_PLAY || cmd == KEY_CMD_REPLAY) {
                if (playlist_count() > 0) {
                    started = audio_play_index(playlist_current());
                } else {
                    printf("AudioTask: no mp3 found\r\n");
                }
            } else if (cmd == KEY_CMD_NEXT) {
                started = audio_play_index(playlist_next());
            } else if (cmd == KEY_CMD_PREV) {
                started = audio_play_index(playlist_prev());
            } else if (cmd == KEY_CMD_VOL_UP) {
                app_key_volume_up();
                settings_store();
            } else if (cmd == KEY_CMD_VOL_DOWN) {
                app_key_volume_down();
                settings_store();
            }
            continue;
        }

        /* 4. 播放中：喂数据 */
        if (!mp3_player_poll()) {
            printf("AudioTask: Playback done\r\n");
            /* 自动下一首，播完一圈回到第一首 */
            started = audio_play_index(playlist_next());
            continue;
        }

        /* 5. 顺手处理命令，最多等 5ms，保证喂数据节奏 */
        if (app_key_wait_cmd(5, &cmd)) {
            if (cmd == KEY_CMD_TOGGLE_PLAY) {
                if (mp3_player_is_paused()) {
                    mp3_player_resume();
                    printf("resume\r\n");
                } else {
                    mp3_player_pause();
                    printf("pause\r\n");
                }
            } else if (cmd == KEY_CMD_REPLAY) {
                started = audio_play_index(playlist_current());
            } else if (cmd == KEY_CMD_NEXT) {
                started = audio_play_index(playlist_next());
            } else if (cmd == KEY_CMD_PREV) {
                started = audio_play_index(playlist_prev());
            } else if (cmd == KEY_CMD_VOL_UP) {
                app_key_volume_up();
                settings_store();
            } else if (cmd == KEY_CMD_VOL_DOWN) {
                app_key_volume_down();
                settings_store();
            }
        } else {
            osDelay(1);
        }
    }
  /* USER CODE END StartAudioTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
static void settings_store(void)
{
    if (!settings_save(app_key_volume_get(), playlist_current())) {
        printf("AudioTask: settings save failed\r\n");
    }
}

static uint8_t audio_play_index(uint8_t index)
{
    char path[20];
    int8_t r;
    uint8_t tries;
    uint8_t total = playlist_count();

    if (total == 0) return 0;

    /* 当前曲目打不开时自动跳到下一首，最多绕一圈 */
    for (tries = 0; tries < total; tries++) {
        playlist_set_current(index);
        playlist_path(index, path, sizeof(path));

        mp3_player_stop();              /* 关掉上一首，避免重复打开文件 */
        r = mp3_player_play(&hi2s2, path);
        if (r == MP3_PLAYER_OK) {
            printf("AudioTask: track %u/%u %s\r\n",
                   (unsigned)(index + 1), (unsigned)total,
                   playlist_name(index));
            settings_store();
            return 1;
        }

        printf("AudioTask: play %u failed = %d, try next\r\n",
               (unsigned)(index + 1), (int)r);
        index = playlist_next();
    }

    printf("AudioTask: no playable track\r\n");
    return 0;
}
/* USER CODE END Application */


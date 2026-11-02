/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "main.h"
#include "dma.h"
#include "fatfs.h"
#include "i2c.h"
#include "i2s.h"
#include "sdio.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include "es8388.h"
#include <string.h>
#include "minimp3.h"
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

/* USER CODE BEGIN PV */
#define AUDIO_HALF_BYTES     4608                       /* 一帧 PCM：1152 采样 × 2 声道 × 2 字节 */
#define AUDIO_HALF_HALFWORDS (AUDIO_HALF_BYTES / 2)     /* 每个半区 2304 个半字 */
#define AUDIO_BUF_HALFWORDS  (AUDIO_HALF_HALFWORDS * 2) /* 总长 4608 个半字 = 9216 字节 */
#define MP3_BUF_BYTES        4096                       /* 压缩数据缓冲区大小 */
#define MP3_FRAME_MAX        2400                       /* 单帧压缩数据最大字节数（留余量） */

static uint16_t Audio_Buf[AUDIO_BUF_HALFWORDS];

volatile uint32_t data_remaining = 0;   /* 剩余未读的音频字节数 */
volatile uint8_t play_flag = 0;         /* 1 = 播放中 */
volatile uint8_t half_flag = 0;         /* 前半区播完 */
volatile uint8_t full_flag = 0;         /* 后半区播完 */

static FRESULT res;
static mp3dec_t mp3d;
static mp3dec_frame_info_t info;        /* 当前帧的报告单 */
static uint8_t mp3_buf[MP3_BUF_BYTES];
static int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
static int mp3_len = 0;                 /* 压缩缓冲里的有效字节数 */
static int mp3_pos = 0;                 /* 下一次解码在缓冲里的位置 */
static uint8_t mp3_eof = 0;             /* 1 = 文件已读完 */

#pragma pack(push, 1)                   /* 按 1 字节对齐，和 WAV 文件布局一致 */
typedef struct {
    char     ChunkID[4];
    uint32_t ChunkSize;
    char     Format[4];
    char     Subchunk1ID[4];
    uint32_t Subchunk1Size;
    uint16_t AudioFormat;
    uint16_t NumChannels;
    uint32_t SampleRate;
    uint32_t ByteRate;
    uint16_t BlockAlign;
    uint16_t BitsPerSample;
    char     Subchunk2ID[4];
    uint32_t Subchunk2Size;
} WAV_Header;
#pragma pack(pop)

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
// MP3 播放专用的补货函数
static uint8_t audio_refill_mp3(uint8_t half)
{
    // 1. 确定要填的缓冲区地址（前半区还是后半区）
    uint8_t *p = (uint8_t *)Audio_Buf + half * AUDIO_HALF_BYTES;
    uint32_t bytes_filled = 0; // 当前这个半区已经装了多少个字节的PCM数据
    int samples;
    int left;
    UINT r;

    // 只要还没填满半区，就继续解压
    while (bytes_filled < AUDIO_HALF_BYTES)
    {
        // 2. 缓冲里的数据解完了：先看是否文件结束，否则读新数据
        if (mp3_pos >= mp3_len) {
            if (mp3_eof) {
                memset(p + bytes_filled, 0, AUDIO_HALF_BYTES - bytes_filled);
                return 0;
            }
            mp3_pos = 0;
            mp3_len = 0;
            r = 0;
            f_read(&SDFile, mp3_buf, MP3_BUF_BYTES, &r);
            if (r == 0) {
                // 卡里真的一点数据都没了，剩下的半区填 0（静音）并返回 0
                mp3_eof = 1;
                memset(p + bytes_filled, 0, AUDIO_HALF_BYTES - bytes_filled);
                return 0;
            }
            mp3_len = (int)r;
        }

        // 3. 剩余数据不够一整帧：把尾部挪到头部，再补读新数据
        //    （不这么做的话，缓冲交界处的半帧会被当成垃圾丢掉）
        if (!mp3_eof && (mp3_len - mp3_pos) < MP3_FRAME_MAX) {
            left = mp3_len - mp3_pos;
            memmove(mp3_buf, mp3_buf + mp3_pos, left);
            mp3_len = left;
            mp3_pos = 0;

            r = 0;
            f_read(&SDFile, mp3_buf + mp3_len,
                   MP3_BUF_BYTES - mp3_len, &r);
            if (r == 0) {
                mp3_eof = 1;    // 读空了，下一圈按文件末尾处理
            } else {
                mp3_len += (int)r;
            }
            continue;
        }

        // 4. 尝试解码一帧（只看缓冲里还没用过的部分）
        samples = mp3dec_decode_frame(&mp3d, mp3_buf + mp3_pos,
                                      mp3_len - mp3_pos, pcm, &info);

        if (samples > 0) {
            // samples 是单声道采样数，立体声 16bit 所以要 ×4
            uint32_t pcm_bytes = samples * info.channels * 2;

            // 防溢出：正常情况下半区 4608 正好一帧，这里只是兜底
            if (bytes_filled + pcm_bytes > AUDIO_HALF_BYTES) {
                pcm_bytes = AUDIO_HALF_BYTES - bytes_filled;
            }

            memcpy(p + bytes_filled, pcm, pcm_bytes);
            bytes_filled += pcm_bytes;
            mp3_pos += info.frame_bytes;
        }
        else if (info.frame_bytes > 0) {
            // 没解出采样但消耗了数据（Info 帧、或跳过的无效字节），接着来
            mp3_pos += info.frame_bytes;
        }
        else {
            // 剩下的连一个整帧都凑不出来了，收尾静音
            mp3_eof = 1;
            memset(p + bytes_filled, 0, AUDIO_HALF_BYTES - bytes_filled);
            return 0;
        }
    }

    return 1; // 补给成功，半区已装满
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
	uint8_t ret;

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_I2C1_Init();
  MX_USART1_UART_Init();
  MX_I2S2_Init();
  MX_SDIO_SD_Init();
  MX_FATFS_Init();
  /* USER CODE BEGIN 2 */
    /* 1. ES8388 初始化 */
    ret = es8388_init(&hi2c1);
    printf("es8388 init = %d\r\n", ret);
    es8388_adda_cfg(1, 0);
    es8388_output_cfg(1, 0);
    es8388_hpvol_set(10);
    es8388_spkvol_set(0);

    /* 2. 挂载 SD 卡 */
    res = f_mount(&SDFatFS, SDPath, 1);
    printf("mount = %d\r\n", res);
    if (res != FR_OK) while (1);

//    /* 3. 打开并解析 WAV */
//    res = f_open(&SDFile, "0:/TEST.WAV", FA_READ);
//    printf("open wav = %d\r\n", res);
//    if (res != FR_OK) while (1);

//    WAV_Header wav;
//    res = f_read(&SDFile, &wav, sizeof(WAV_Header), &br);
//    if (res != FR_OK || br != sizeof(WAV_Header))
//    {
//        printf("header read failed\r\n");
//        while (1);
//    }

//    printf("wav: %u Hz, %u ch, %u bit, %u bytes\r\n",
//           (unsigned)wav.SampleRate, (unsigned)wav.NumChannels,
//           (unsigned)wav.BitsPerSample, (unsigned)wav.Subchunk2Size);

//    if (wav.AudioFormat != 1 || wav.SampleRate != 44100 ||
//        wav.NumChannels != 2 || wav.BitsPerSample != 16)
//    {
//        printf("unsupported wav format\r\n");
//        f_close(&SDFile);
//        while (1);
//    }

//    data_remaining = wav.Subchunk2Size;

//    /* 4. 预装两个半区 */
//    {
//        UINT r = 0;
//        uint32_t want = (data_remaining > sizeof(Audio_Buf)) ? sizeof(Audio_Buf) : data_remaining;
//        f_read(&SDFile, (uint8_t *)Audio_Buf, want, &r);
//        if (r < sizeof(Audio_Buf))
//        {
//            memset((uint8_t *)Audio_Buf + r, 0, sizeof(Audio_Buf) - r);
//        }
//        data_remaining -= r;
//    }

//    /* 5. 启动 DMA，循环播放整个 8KB */
//    half_flag = 0;
//    full_flag = 0;
//    if (HAL_I2S_Transmit_DMA(&hi2s2, Audio_Buf, AUDIO_BUF_HALFWORDS) == HAL_OK)
//    {
//        printf("playback started\r\n");
//        play_flag = 1;
//    }
//    else
//    {
//        printf("dma start failed\r\n");
//    }

//    /* 6. 主循环：哪半区播完就补哪半区 */
//    while (play_flag)
//    {
//        if (half_flag)
//        {
//            half_flag = 0;
//            if (!audio_refill(0)) play_flag = 0;
//        }
//        if (full_flag)
//        {
//            full_flag = 0;
//            if (!audio_refill(1)) play_flag = 0;
//        }
//    }

//    HAL_I2S_DMAStop(&hi2s2);
//    f_close(&SDFile);
//    printf("playback done\r\n");

// 3. 打开 MP3 文件（卡里必须是 TEST.MP3）
  res = f_open(&SDFile, "0:/TEST.MP3", FA_READ);
  if (res != FR_OK) {
      printf("Open MP3 failed! code = %d\r\n", res);
      while(1); 
  }

  mp3dec_init(&mp3d); // 初始化翻译官

  printf("Start MP3 DMA Playback...\r\n");

  // 1. 预装两节车厢（前半区和后半区）
  // 如果文件很短，一开始就返回0了，直接不播
  if (!audio_refill_mp3(0)) {
      printf("File too short!\r\n");
      while(1);
  }
  audio_refill_mp3(1);

  // 3. 启动 I2S DMA 播放（还是熟悉的配方，还是原来的味道）
  half_flag = 0;
  full_flag = 0;
  if (HAL_I2S_Transmit_DMA(&hi2s2, Audio_Buf, AUDIO_BUF_HALFWORDS) == HAL_OK)
  {
      printf("Playback started!\r\n");
      play_flag = 1;
  }
  else
  {
      printf("DMA start failed!\r\n");
      while(1);
  }

  // 4. 主循环查旗子
  while (play_flag)
  {
      if (half_flag) {
          half_flag = 0;
          if (!audio_refill_mp3(0)) play_flag = 0; // 前半区播完，补充前半区
      }
      if (full_flag) {
          full_flag = 0;
          if (!audio_refill_mp3(1)) play_flag = 0; // 后半区播完，补充后半区
      }
  }

  // 5. 播放结束收尾
  HAL_I2S_DMAStop(&hi2s2);
  f_close(&SDFile);
  printf("Playback done!\r\n");

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
// 重定向 C 库的 printf 到 USART1
int fputc(int ch, FILE *f)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)&ch, 1, HAL_MAX_DELAY);
    return ch;
}

void HAL_I2S_TxHalfCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s->Instance == SPI2) 
		{
			half_flag = 1;
        }
    
}

void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s->Instance == SPI2) {
        full_flag = 1; // 插旗子：主循环，赶紧把后半段填满！
    }
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

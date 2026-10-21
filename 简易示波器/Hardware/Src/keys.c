/* keys.c - key / rotary-encoder handling for the simple oscilloscope
 * Ported from the reference project (basemodel). No non-ASCII text here on
 * purpose (the display layer keeps its GBK strings; this file stays UTF-8-safe).
 */
#include "main.h"
#include "tft.h"
#include "tim.h"
#include "adc.h"

static uint8_t key1_state = 0;
static uint8_t key2_state = 0;
static uint8_t key3_state = 0;
static uint8_t A_cnt = 0;
static uint8_t B_Value = 0;

void Open_Led(uint8_t value)
{
  switch (value)
  {
    case LED1: HAL_GPIO_WritePin(GPIOC, GPIO_PIN_14, GPIO_PIN_RESET); break;
    case LED2: HAL_GPIO_WritePin(GPIOC, GPIO_PIN_15, GPIO_PIN_RESET); break;
    default: break;
  }
}

void CLose_LED(uint8_t value)
{
  switch (value)
  {
    case LED1: HAL_GPIO_WritePin(GPIOC, GPIO_PIN_14, GPIO_PIN_SET); break;
    case LED2: HAL_GPIO_WritePin(GPIOC, GPIO_PIN_15, GPIO_PIN_SET); break;
    default: break;
  }
}

void Toggle_Led(uint8_t value)
{
  switch (value)
  {
    case LED1: HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_14); break;
    case LED2: HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_15); break;
    default: break;
  }
}

void Key_Handle(volatile struct Oscilloscope *value)
{
  ADC_ChannelConfTypeDef sConfig = {0};
  float tempValue = 0;
  switch ((*value).keyValue)
  {
    case KEY1PRESS:  /* PWM duty +4% */
    {
      (*value).pwmOut = ((uint32_t)((*value).timerPeriod * 0.04f)) + (*value).pwmOut;
      if ((*value).pwmOut > (*value).timerPeriod)
      {
        (*value).pwmOut = 0;
      }
      __HAL_TIM_SetCompare(&htim2, TIM_CHANNEL_3, (uint16_t)(*value).pwmOut);
    }
    break;

    case KEY2PRESS:  /* PWM output on/off */
    {
      if ((*value).ouptputbit == 0)
      {
        (*value).ouptputbit = 1;
        HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_3);
      }
      else
      {
        (*value).ouptputbit = 0;
        HAL_TIM_PWM_Stop(&htim2, TIM_CHANNEL_3);
      }
    }
    break;

    case KEY3PRESS:  /* output frequency /2 (1000 -> 500 -> 250 -> 1000) */
    {
      tempValue = (*value).pwmOut / ((*value).timerPeriod + 0.0f);
      (*value).timerPeriod = (*value).timerPeriod / 2;
      if ((*value).timerPeriod < 250)
      {
        (*value).timerPeriod = 1000;
      }
      (*value).outputFreq = 1000000 / (*value).timerPeriod;
      (*value).pwmOut = (uint16_t)((*value).timerPeriod * tempValue);
      __HAL_TIM_SetCompare(&htim2, TIM_CHANNEL_3, (uint16_t)(*value).pwmOut);
      __HAL_TIM_SetAutoreload(&htim2, (uint16_t)((*value).timerPeriod - 1));
    }
    break;

    case KEYAPRESS:  /* zoom: step ADC sampling time */
    {
      switch ((*value).sampletime)
      {
        case ADC_SAMPLETIME_239CYCLES_5: (*value).sampletime = ADC_SAMPLETIME_71CYCLES_5;  break;
        case ADC_SAMPLETIME_71CYCLES_5:  (*value).sampletime = ADC_SAMPLETIME_55CYCLES_5;  break;
        case ADC_SAMPLETIME_55CYCLES_5:  (*value).sampletime = ADC_SAMPLETIME_41CYCLES_5;  break;
        case ADC_SAMPLETIME_41CYCLES_5:  (*value).sampletime = ADC_SAMPLETIME_28CYCLES_5;  break;
        case ADC_SAMPLETIME_28CYCLES_5:  (*value).sampletime = ADC_SAMPLETIME_239CYCLES_5; break;
        default:                         (*value).sampletime = ADC_SAMPLETIME_239CYCLES_5; break;
      }
      sConfig.Channel = ADC_CHANNEL_3;
      sConfig.Rank = ADC_REGULAR_RANK_1;
      sConfig.SamplingTime = (*value).sampletime;
      if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
      {
        Error_Handler();
      }
    }
    break;

    case KEYBPRESS:  /* zoom: step ADC sampling time (other direction) */
    {
      switch ((*value).sampletime)
      {
        case ADC_SAMPLETIME_239CYCLES_5: (*value).sampletime = ADC_SAMPLETIME_28CYCLES_5;  break;
        case ADC_SAMPLETIME_71CYCLES_5:  (*value).sampletime = ADC_SAMPLETIME_239CYCLES_5; break;
        case ADC_SAMPLETIME_55CYCLES_5:  (*value).sampletime = ADC_SAMPLETIME_71CYCLES_5;  break;
        case ADC_SAMPLETIME_41CYCLES_5:  (*value).sampletime = ADC_SAMPLETIME_55CYCLES_5;  break;
        case ADC_SAMPLETIME_28CYCLES_5:  (*value).sampletime = ADC_SAMPLETIME_41CYCLES_5;  break;
        default:                         (*value).sampletime = ADC_SAMPLETIME_239CYCLES_5; break;
      }
      sConfig.Channel = ADC_CHANNEL_3;
      sConfig.Rank = ADC_REGULAR_RANK_1;
      sConfig.SamplingTime = (*value).sampletime;
      if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
      {
        Error_Handler();
      }
    }
    break;

    default:
      break;
  }
  (*value).keyValue = NoPRESS;
  TFT_ShowUI(value);
}

void Key_Sacnf(volatile struct Oscilloscope *value)
{
  if (key1_state == KEYPRESS)
  {
    HAL_Delay(20);
    if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_13) == GPIO_PIN_RESET)
    {
      (*value).keyValue = KEY1PRESS;
      key1_state = NoPRESS;
    }
  }
  else
  {
    key1_state = NoPRESS;
  }

  if (key2_state == KEYPRESS)
  {
    HAL_Delay(20);
    if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_14) == GPIO_PIN_RESET)
    {
      Toggle_Led(LED1);
      (*value).keyValue = KEY2PRESS;
      key2_state = NoPRESS;
    }
  }
  else
  {
    key2_state = NoPRESS;
  }

  if (key3_state == KEYPRESS)
  {
    HAL_Delay(20);
    if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_15) == GPIO_PIN_RESET)
    {
      (*value).keyValue = KEY3PRESS;
      key3_state = NoPRESS;
    }
  }
  else
  {
    key3_state = NoPRESS;
  }
}

void KEYD_SCAN(volatile struct Oscilloscope *value)
{
  if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_9) == GPIO_PIN_RESET)
  {
    HAL_Delay(20);
    if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_9) == GPIO_PIN_RESET)
    {
      while (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_9) == GPIO_PIN_RESET);
      (*value).keyValue = KEYDPRESS;
    }
  }
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == GPIO_PIN_4)  /* rotary encoder A (PB4), B = PB3 */
  {
    if ((HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_4) == GPIO_PIN_RESET) && (A_cnt == 0))
    {
      A_cnt++;
      B_Value = 0;
      if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_3) == GPIO_PIN_SET)
      {
        B_Value = 1;
      }
    }
    else if ((HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_4) == GPIO_PIN_SET) && (A_cnt == 1))
    {
      A_cnt = 0;
      if ((B_Value == 1) && (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_3) == GPIO_PIN_RESET))
      {
        oscilloscope.keyValue = KEYBPRESS;
      }
      if ((B_Value == 0) && (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_3) == GPIO_PIN_SET))
      {
        oscilloscope.keyValue = KEYAPRESS;
      }
    }
  }
  if (GPIO_Pin == GPIO_PIN_13)
  {
    if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_13) == GPIO_PIN_RESET) key1_state = KEYPRESS;
  }
  if (GPIO_Pin == GPIO_PIN_14)
  {
    if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_14) == GPIO_PIN_RESET) key2_state = KEYPRESS;
  }
  if (GPIO_Pin == GPIO_PIN_15)
  {
    if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_15) == GPIO_PIN_RESET) key3_state = KEYPRESS;
  }
}

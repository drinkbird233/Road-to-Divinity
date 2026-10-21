#include "stm32f10x.h"                  // Device header
#include "PWM.h"

/*
 * PA0 (TIM2_CH1 PWM) → ENA    速度控制
 * PB12 (GPIO)         → IN1    方向控制
 * PB13 (GPIO)         → IN2    方向控制
 * ENA 不戴跳线帽
 */

void Motor_Init(void)
{
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

	GPIO_InitTypeDef GPIO_InitStructure;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_12 | GPIO_Pin_13;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOB, &GPIO_InitStructure);

	PWM_Init();
}

void Motor_SetPWM(int8_t Speed)
{
	if (Speed >= 0)
	{
		GPIO_SetBits(GPIOB, GPIO_Pin_12);		// IN1 = 高
		GPIO_ResetBits(GPIOB, GPIO_Pin_13);		// IN2 = 低
	}
	else
	{
		GPIO_ResetBits(GPIOB, GPIO_Pin_12);		// IN1 = 低
		GPIO_SetBits(GPIOB, GPIO_Pin_13);		// IN2 = 高
		Speed = -Speed;
	}

	if (Speed > 100) Speed = 100;
	PWM_SetCompare1(Speed);					// ENA PWM
}

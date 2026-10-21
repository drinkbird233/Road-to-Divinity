#include "stm32f10x.h"                  // Device header

/*
 * 测速传感器：槽型光电开关，电机每转 1 个脉冲
 * 输出经 8050 NPN 三极管开漏 → 10K 上拉 → J2 Pin1
 * 接 STM32 PA6 (TIM3_CH1)，输入捕获测周期
 *
 * TIM3: 72MHz / 720 = 100kHz (10us tick)
 * 2 秒无脉冲判为 0 RPM，约 30 RPM 以下视为 0
 */

static volatile uint16_t Period;
static volatile uint8_t  Updated;
static volatile uint16_t Timeout;	// 无脉冲计数器，TIM1 1ms 递增

void Encoder_Init(void)
{
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3, ENABLE);
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);

	GPIO_InitTypeDef GPIO_InitStructure;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_6;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOA, &GPIO_InitStructure);

	TIM_TimeBaseInitTypeDef TIM_TimeBaseInitStructure;
	TIM_TimeBaseInitStructure.TIM_ClockDivision = TIM_CKD_DIV1;
	TIM_TimeBaseInitStructure.TIM_CounterMode = TIM_CounterMode_Up;
	TIM_TimeBaseInitStructure.TIM_Period = 65536 - 1;
	TIM_TimeBaseInitStructure.TIM_Prescaler = 720 - 1;		// 100kHz
	TIM_TimeBaseInitStructure.TIM_RepetitionCounter = 0;
	TIM_TimeBaseInit(TIM3, &TIM_TimeBaseInitStructure);

	TIM_ICInitTypeDef TIM_ICInitStructure;
	TIM_ICInitStructure.TIM_Channel = TIM_Channel_1;
	TIM_ICInitStructure.TIM_ICFilter = 0x0F;
	TIM_ICInitStructure.TIM_ICPolarity = TIM_ICPolarity_Rising;
	TIM_ICInitStructure.TIM_ICPrescaler = TIM_ICPSC_DIV1;
	TIM_ICInitStructure.TIM_ICSelection = TIM_ICSelection_DirectTI;
	TIM_ICInit(TIM3, &TIM_ICInitStructure);

	TIM_ITConfig(TIM3, TIM_IT_CC1, ENABLE);

	NVIC_InitTypeDef NVIC_InitStructure;
	NVIC_InitStructure.NVIC_IRQChannel = TIM3_IRQn;
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
	NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
	NVIC_Init(&NVIC_InitStructure);

	TIM_Cmd(TIM3, ENABLE);
}

int16_t Encoder_Get(void)
{
	int16_t Speed;
	uint16_t p;

	if (Timeout > 2000)			// 2 秒无脉冲 → 0 RPM
	{
		return 0;
	}

	if (Updated == 0)
	{
		return -1;//代表无有效转速
	}
	Updated = 0;

	p = Period;
	if (p == 0) return 0;

	Speed = 60 * 100000 / p;		// RPM = 60 / (period_seconds)

	return Speed;
}

void Encoder_Tick(void)				// 由 TIM1 每 1ms 调用
{
	Timeout++;
}

void TIM3_IRQHandler(void)
{
	static uint16_t LastCap;
	uint16_t CurCap;

	if (TIM_GetITStatus(TIM3, TIM_IT_CC1) == SET)
	{
		CurCap = TIM_GetCapture1(TIM3);//读取本次捕获定时器的CNT值
		Period = CurCap - LastCap;//算两次脉冲的间隔
		LastCap = CurCap;//更新上次捕获值为当前
		Updated = 1;//可计算转速
		Timeout = 0;				// 收到脉冲，清超时
		TIM_ClearITPendingBit(TIM3, TIM_IT_CC1);
	}
}

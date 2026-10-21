#include "stm32f10x.h"
#include "Delay.h"
#include "OLED.h"
#include "Timer.h"
#include "Key.h"
#include "Motor.h"
#include "Encoder.h"
#include "Serial.h"

float Target_RPM, Actual_RPM, PID_Out;
float Kp, Ki, Kd;
uint8_t MotorRun;
float Error0, Error1, ErrorInt;
volatile uint8_t TxFlag;

int main(void)
{
	OLED_Init();
	Key_Init();
	Motor_Init();
	Encoder_Init();
	Serial_Init();
	Timer_Init();

	Kp = 0.1;
	Ki = 0.005;
	Kd = 0.4;
	Target_RPM = 1500;
	MotorRun = 0;

	OLED_Printf(0, 0, OLED_8X16, "A-Ti Motor Ctrl");
	OLED_Update();
	Serial_Printf("Target,Actual,Output\r\n");

	while (1)
	{
		uint8_t kn = Key_GetNum();
		if (kn == 1)
		{
			MotorRun = !MotorRun;
			if (MotorRun) ErrorInt = 0;
			else Motor_SetPWM(0);
		}
		if (kn == 2) { Target_RPM += 10; if (Target_RPM > 3000) Target_RPM = 3000; }
		if (kn == 3) { Target_RPM -= 10; if (Target_RPM < 0) Target_RPM = 0; }

		/* ── 串口接收 ── */
		if (Serial_GetRxFlag())
		{
			ParseCmd(Serial_RxPacket);
		}

		/* ── 串口发送 ── */
		if (TxFlag)
		{
			TxFlag = 0;
			Serial_Printf("%.1f,%.1f,%.1f\r\n", Target_RPM, Actual_RPM, PID_Out);
		}

		OLED_Printf(0, 16, OLED_8X16, "Tar:%5.0f RPM", Target_RPM);
		OLED_Printf(0, 32, OLED_8X16, "Act:%5.0f RPM", Actual_RPM);
		OLED_Printf(0, 48, OLED_8X16, "%s %s",
			MotorRun ? "RUN " : "STOP", "");
		OLED_Update();
	}
}

void TIM1_UP_IRQHandler(void)
{
	static uint16_t Count;

	if (TIM_GetITStatus(TIM1, TIM_IT_Update) == SET)
	{
		Key_Tick();
		Encoder_Tick();

		Count++;
		if (Count >= 20)
		{
			Count = 0;

			int16_t Speed = Encoder_Get();
			if (Speed >= 0) Actual_RPM = Speed;

			if (MotorRun)
			{
				Error1 = Error0;
				Error0 = Target_RPM - Actual_RPM;

				if (Ki != 0)
				{
					ErrorInt += Error0;
					if (ErrorInt > 90/Ki) ErrorInt = 90/Ki;
					if (ErrorInt < -90/Ki) ErrorInt = -90/Ki;
				}

				PID_Out = Kp * Error0 + Ki * ErrorInt + Kd * (Error0 - Error1);

				if (PID_Out > 100)  PID_Out = 100;
				if (PID_Out < -100) PID_Out = -100;

				Motor_SetPWM((int8_t)PID_Out);
			}
			else
			{
				Motor_SetPWM(0);
			}
		}

		{
			static uint16_t TxCount;//串口发送
			TxCount++;
			if (TxCount >= 100)
			{
				TxCount = 0;
				TxFlag = 1;
			}
		}

		TIM_ClearITPendingBit(TIM1, TIM_IT_Update);
	}
}

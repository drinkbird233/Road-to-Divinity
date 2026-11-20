/**
 * @file  app_main.h
 * @brief 给 CubeMX 生成的 main.c / freertos.c 用的入口。
 *
 * 接入方式（三处，全部在 USER CODE 区，重新生成不会丢）：
 *   1) main.c 头部：      #include "app_main.h"
 *   2) main.c USER CODE 2：      app_init();
 *   3) main.c USER CODE Callback 0：
 *        else if (htim->Instance == TIM6) { app_tim6_isr(); }
 *   4) freertos.c USER CODE RTOS_THREADS：
 *        app_start();
 *
 * 不要在 app_main.c 里定义 HAL_TIM_PeriodElapsedCallback —— main.c 里已经有一个
 * （负责 TIM7 → HAL_IncTick），重复定义会报 multiply defined。
 */
#ifndef APP_MAIN_H
#define APP_MAIN_H

void app_init(void);
void app_start(void);
void app_log(const char *fmt, ...);
void app_tim6_isr(void);

#endif /* APP_MAIN_H */

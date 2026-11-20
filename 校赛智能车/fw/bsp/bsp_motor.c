/**
 * @file  bsp_motor.c
 *
 * 【CubeMX 引脚标签】MOTOR_L_PWM / MOTOR_L_IN1 / MOTOR_L_IN2 /
 *                   MOTOR_R_PWM / MOTOR_R_IN1 / MOTOR_R_IN2 / MOTOR_STBY
 */
#include "bsp_motor.h"
#include "app_config.h"
#include "pid.h"
#include "main.h"

extern TIM_HandleTypeDef htim1;

static float   s_cur_l;
static float   s_cur_r;
static uint8_t s_enabled;
static uint8_t s_brake;

/* 死区补偿：把 [0, PWM_MAX] 线性映射到 [DEADBAND, PWM_MAX]，0 仍为 0。
 * 保证"给一点就转"，同时不改变 0 附近的可停性。 */
static float deadband_map(float d)
{
    float a;
    if (CFG_MOTOR_DEADBAND <= 0.0f) return d;
    a = (d < 0.0f) ? -d : d;
    if (a < 0.5f) return 0.0f;
    if (a > CFG_PWM_MAX) a = CFG_PWM_MAX;
    a = CFG_MOTOR_DEADBAND + a * (CFG_PWM_MAX - CFG_MOTOR_DEADBAND) / CFG_PWM_MAX;
    return (d < 0.0f) ? -a : a;
}

static void apply_left(float d)
{
    uint32_t pwm;

    if (s_brake) {
        HAL_GPIO_WritePin(MOTOR_L_IN1_GPIO_Port, MOTOR_L_IN1_Pin, GPIO_PIN_SET);
        HAL_GPIO_WritePin(MOTOR_L_IN2_GPIO_Port, MOTOR_L_IN2_Pin, GPIO_PIN_SET);
        __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0u);
        return;
    }

    if (d > 0.0f) {
        HAL_GPIO_WritePin(MOTOR_L_IN1_GPIO_Port, MOTOR_L_IN1_Pin, GPIO_PIN_SET);
        HAL_GPIO_WritePin(MOTOR_L_IN2_GPIO_Port, MOTOR_L_IN2_Pin, GPIO_PIN_RESET);
    } else if (d < 0.0f) {
        HAL_GPIO_WritePin(MOTOR_L_IN1_GPIO_Port, MOTOR_L_IN1_Pin, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(MOTOR_L_IN2_GPIO_Port, MOTOR_L_IN2_Pin, GPIO_PIN_SET);
        d = -d;
    } else {
        HAL_GPIO_WritePin(MOTOR_L_IN1_GPIO_Port, MOTOR_L_IN1_Pin, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(MOTOR_L_IN2_GPIO_Port, MOTOR_L_IN2_Pin, GPIO_PIN_RESET);
        d = 0.0f;
    }

    pwm = (uint32_t)(d + 0.5f);
    if (pwm > (uint32_t)CFG_PWM_MAX) pwm = (uint32_t)CFG_PWM_MAX;
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, pwm);
}

static void apply_right(float d)
{
    uint32_t pwm;

    if (s_brake) {
        HAL_GPIO_WritePin(MOTOR_R_IN1_GPIO_Port, MOTOR_R_IN1_Pin, GPIO_PIN_SET);
        HAL_GPIO_WritePin(MOTOR_R_IN2_GPIO_Port, MOTOR_R_IN2_Pin, GPIO_PIN_SET);
        __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, 0u);
        return;
    }

    if (d > 0.0f) {
        HAL_GPIO_WritePin(MOTOR_R_IN1_GPIO_Port, MOTOR_R_IN1_Pin, GPIO_PIN_SET);
        HAL_GPIO_WritePin(MOTOR_R_IN2_GPIO_Port, MOTOR_R_IN2_Pin, GPIO_PIN_RESET);
    } else if (d < 0.0f) {
        HAL_GPIO_WritePin(MOTOR_R_IN1_GPIO_Port, MOTOR_R_IN1_Pin, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(MOTOR_R_IN2_GPIO_Port, MOTOR_R_IN2_Pin, GPIO_PIN_SET);
        d = -d;
    } else {
        HAL_GPIO_WritePin(MOTOR_R_IN1_GPIO_Port, MOTOR_R_IN1_Pin, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(MOTOR_R_IN2_GPIO_Port, MOTOR_R_IN2_Pin, GPIO_PIN_RESET);
        d = 0.0f;
    }

    pwm = (uint32_t)(d + 0.5f);
    if (pwm > (uint32_t)CFG_PWM_MAX) pwm = (uint32_t)CFG_PWM_MAX;
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, pwm);
}

void bsp_motor_init(void)
{
    s_cur_l = 0.0f;
    s_cur_r = 0.0f;
    s_enabled = 0u;
    s_brake = 0u;

    /* 先给安全态，再开 STBY */
    HAL_GPIO_WritePin(MOTOR_STBY_GPIO_Port, MOTOR_STBY_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(MOTOR_L_IN1_GPIO_Port, MOTOR_L_IN1_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(MOTOR_L_IN2_GPIO_Port, MOTOR_L_IN2_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(MOTOR_R_IN1_GPIO_Port, MOTOR_R_IN1_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(MOTOR_R_IN2_GPIO_Port, MOTOR_R_IN2_Pin, GPIO_PIN_RESET);

    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0u);
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, 0u);
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_2);
}

void bsp_motor_enable(bool en)
{
    s_enabled = en ? 1u : 0u;
    if (!en) {
        s_cur_l = 0.0f;
        s_cur_r = 0.0f;
        apply_left(0.0f);
        apply_right(0.0f);
    }
    HAL_GPIO_WritePin(MOTOR_STBY_GPIO_Port, MOTOR_STBY_Pin, en ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

bool bsp_motor_is_enabled(void)
{
    return s_enabled != 0u;
}

void bsp_motor_set(float duty_l, float duty_r)
{
    float l, r;

    if (!s_enabled) {
        apply_left(0.0f);
        apply_right(0.0f);
        return;
    }

    l = rate_limit(s_cur_l, clampf(duty_l, -CFG_PWM_MAX, CFG_PWM_MAX), CFG_DUTY_SLEW);
    r = rate_limit(s_cur_r, clampf(duty_r, -CFG_PWM_MAX, CFG_PWM_MAX), CFG_DUTY_SLEW);
    s_cur_l = l;
    s_cur_r = r;

    if (CFG_MOTOR_L_INVERT) l = -l;
    if (CFG_MOTOR_R_INVERT) r = -r;

    /* 电机通道对调：如果 TIM1_CH1 实际驱动的是右轮，置 1。
     * 斜坡作用在逻辑侧（s_cur_l/r）之上，所以交换放在最后，不影响限速。 */
#if CFG_MOTOR_SWAP_LR
    {
        float t = l; l = r; r = t;
    }
#endif

    apply_left(deadband_map(l));
    apply_right(deadband_map(r));
}

void bsp_motor_force_stop(void)
{
    s_enabled = 0u;
    s_brake = 0u;
    s_cur_l = 0.0f;
    s_cur_r = 0.0f;
    apply_left(0.0f);
    apply_right(0.0f);
    HAL_GPIO_WritePin(MOTOR_STBY_GPIO_Port, MOTOR_STBY_Pin, GPIO_PIN_RESET);
}

void bsp_motor_brake(bool on)
{
    s_brake = on ? 1u : 0u;
    if (!on) { apply_left(0.0f); apply_right(0.0f); }
}

float bsp_motor_duty_l(void) { return s_cur_l; }
float bsp_motor_duty_r(void) { return s_cur_r; }

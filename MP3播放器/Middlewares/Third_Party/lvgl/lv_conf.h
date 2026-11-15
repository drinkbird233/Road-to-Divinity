/* SPDX-License-Identifier: MIT */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
#define LV_USE_OS LV_OS_NONE
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (48U * 1024U)
#define LV_MEM_ADR 0x10000000
#define LV_DEF_REFR_PERIOD 10
#define LV_DPI_DEF 130
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_STYLE 0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ 0
#ifdef UI_HOST_TEST
#include <stdlib.h>
#include <stdio.h>
#define LV_ASSERT_HANDLER fprintf(stderr, "LVGL assertion at %s:%d\n", __FILE__, __LINE__); abort();
#endif
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_CUSTOM_DECLARE LV_FONT_DECLARE(ui_font_cn_16)
#define LV_FONT_DEFAULT &ui_font_cn_16
#define LV_USE_FONT_COMPRESSED 0
#define LV_USE_THEME_DEFAULT 0
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_FLEX 1
#define LV_USE_GRID 0
#define LV_USE_BAR 1
#define LV_USE_BUTTON 1
#define LV_USE_LABEL 1
#define LV_USE_SLIDER 1
#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0

#endif

/* SPDX-License-Identifier: MIT */
#ifndef UI_THEME_H
#define UI_THEME_H
#include "lvgl/lvgl.h"

/* EEZ always emits default-theme initialization. This UI uses explicit styles. */
#if !LV_USE_THEME_DEFAULT
#define lv_theme_default_init(display, primary, secondary, dark, font) ((lv_theme_t *)0)
#endif
#endif

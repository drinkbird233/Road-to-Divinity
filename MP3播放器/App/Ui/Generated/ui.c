#include "ui.h"
#include "ui_controller.h"
#include "screens.h"
#include "images.h"
#include "actions.h"
#include "vars.h"

#include <string.h>

static int16_t currentScreen = -1;

static lv_obj_t *getLvglObjectFromIndex(int32_t index) {
    if (index == -1) {
        return 0;
    }
    return ((lv_obj_t **)&objects)[index];
}

void loadScreen(enum ScreensEnum screenId) {
    currentScreen = screenId - 1;
    lv_obj_t *screen = getLvglObjectFromIndex(currentScreen);
    lv_scr_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}

void ui_init() {
    create_screens();
    ui_controller_init();
    loadScreen(SCREEN_ID_SCREEN_LIBRARY);

}

void ui_tick() {
    tick_screen(currentScreen);
    ui_controller_tick();
}
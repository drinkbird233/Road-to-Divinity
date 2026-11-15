#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Screens

enum ScreensEnum {
    _SCREEN_ID_FIRST = 1,
    SCREEN_ID_SCREEN_LIBRARY = 1,
    SCREEN_ID_SCREEN_PLAYER = 2,
    SCREEN_ID_OVERLAY_PLAY_MODE = 3,
    SCREEN_ID_SCREEN_SETTINGS = 4,
    _SCREEN_ID_LAST = 4
};

typedef struct _objects_t {
    lv_obj_t *screen_library;
    lv_obj_t *screen_player;
    lv_obj_t *overlay_play_mode;
    lv_obj_t *screen_settings;
    lv_obj_t *screen_library_header;
    lv_obj_t *screen_library_heading;
    lv_obj_t *library_status;
    lv_obj_t *library_settings;
    lv_obj_t *library_settings_label;
    lv_obj_t *library_list;
    lv_obj_t *library_mini;
    lv_obj_t *library_open_player;
    lv_obj_t *library_open_player_label;
    lv_obj_t *library_previous;
    lv_obj_t *library_previous_label;
    lv_obj_t *library_play;
    lv_obj_t *library_play_label;
    lv_obj_t *library_next;
    lv_obj_t *library_next_label;
    lv_obj_t *library_mode;
    lv_obj_t *library_mode_label;
    lv_obj_t *screen_player_header;
    lv_obj_t *screen_player_heading;
    lv_obj_t *screen_player_back;
    lv_obj_t *screen_player_back_label;
    lv_obj_t *player_settings;
    lv_obj_t *player_settings_label;
    lv_obj_t *player_cover;
    lv_obj_t *player_cover_icon;
    lv_obj_t *player_title;
    lv_obj_t *player_artist;
    lv_obj_t *player_format;
    lv_obj_t *player_seek;
    lv_obj_t *player_elapsed;
    lv_obj_t *player_duration;
    lv_obj_t *player_mode;
    lv_obj_t *player_mode_label;
    lv_obj_t *player_previous;
    lv_obj_t *player_previous_label;
    lv_obj_t *player_play;
    lv_obj_t *player_play_label;
    lv_obj_t *player_next;
    lv_obj_t *player_next_label;
    lv_obj_t *player_library;
    lv_obj_t *player_library_label;
    lv_obj_t *player_volume_icon;
    lv_obj_t *player_volume;
    lv_obj_t *player_volume_value;
    lv_obj_t *mode_dialog;
    lv_obj_t *mode_heading;
    lv_obj_t *mode_cancel;
    lv_obj_t *mode_cancel_label;
    lv_obj_t *mode_shuffle;
    lv_obj_t *mode_shuffle_label;
    lv_obj_t *mode_one;
    lv_obj_t *mode_one_label;
    lv_obj_t *mode_all;
    lv_obj_t *mode_all_label;
    lv_obj_t *mode_done;
    lv_obj_t *mode_done_label;
    lv_obj_t *screen_settings_header;
    lv_obj_t *screen_settings_heading;
    lv_obj_t *screen_settings_back;
    lv_obj_t *screen_settings_back_label;
    lv_obj_t *settings_volume_card;
    lv_obj_t *settings_volume_heading;
    lv_obj_t *settings_volume_value;
    lv_obj_t *settings_volume;
    lv_obj_t *settings_brightness_card;
    lv_obj_t *settings_brightness_heading;
    lv_obj_t *settings_brightness_value;
    lv_obj_t *settings_brightness;
    lv_obj_t *settings_output;
    lv_obj_t *settings_output_label;
    lv_obj_t *settings_sleep;
    lv_obj_t *settings_sleep_label;
    lv_obj_t *settings_rescan;
    lv_obj_t *settings_rescan_label;
} objects_t;

extern objects_t objects;

void create_screen_screen_library();
void tick_screen_screen_library();

void create_screen_screen_player();
void tick_screen_screen_player();

void create_screen_overlay_play_mode();
void tick_screen_overlay_play_mode();

void create_screen_screen_settings();
void tick_screen_screen_settings();

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/
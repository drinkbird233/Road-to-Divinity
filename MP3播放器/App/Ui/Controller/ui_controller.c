/* SPDX-License-Identifier: MIT */
#include "ui_controller.h"
#include "ui.h"
#include "screens.h"
#include "lvgl_port.h"
#include "player.h"
#include <stdio.h>
#include <string.h>

#define COLOR_BG 0x101827
#define COLOR_CARD 0x1b2638
#define COLOR_ACTIVE 0x244763
#define COLOR_ACCENT 0x38bdf8
#define ROW_HEIGHT 40
#define VISIBLE_ROWS 5

struct list_slot {
	lv_obj_t *row;
	lv_obj_t *label;
	int index;
};

struct virtual_list {
	lv_obj_t *container;
	lv_obj_t *spacer;
	struct list_slot slots[VISIBLE_ROWS];
	uint16_t count;
	uint16_t current;
	int first;
	bool has_current;
};

static struct virtual_list library;
static struct player_snapshot snapshot;
static uint32_t previous_version = UINT32_MAX;
static uint32_t last_update;
static enum ScreensEnum source_page = SCREEN_ID_SCREEN_LIBRARY;
static enum ScreensEnum mode_source = SCREEN_ID_SCREEN_PLAYER;
static enum ScreensEnum settings_source = SCREEN_ID_SCREEN_LIBRARY;
static enum player_mode pending_mode;
static uint8_t brightness = 65;
static unsigned int sleep_index = 2;
static const uint32_t sleep_times[] = {0, 30000, 60000, 120000};

static void text_if_changed(lv_obj_t *label, const char *text)
{
	if (strcmp(lv_label_get_text(label), text))
		lv_label_set_text(label, text);
}

static void send_command(enum player_command command, uint32_t value)
{
	if (player_send(command, value))
		text_if_changed(objects.library_status, "BUSY");
}

static void select_row(lv_event_t *event)
{
	struct list_slot *slot = lv_event_get_user_data(event);

	if (slot->index < 0)
		return;
	send_command(PLAYER_SELECT, slot->index);
	source_page = SCREEN_ID_SCREEN_PLAYER;
	loadScreen(SCREEN_ID_SCREEN_PLAYER);
}

static void render_list(struct virtual_list *list, bool force)
{
	int first = lv_obj_get_scroll_y(list->container) / ROW_HEIGHT;

	if (first < 0)
		first = 0;
	if (!force && first == list->first)
		return;
	list->first = first;
	for (unsigned int i = 0; i < VISIBLE_ROWS; i++) {
		struct list_slot *slot = &list->slots[i];
		int index = first + i;
		char name[96];

		if (index >= list->count) {
			lv_obj_add_flag(slot->row, LV_OBJ_FLAG_HIDDEN);
			slot->index = -1;
			continue;
		}
		if (player_get_track_name(index, name, sizeof(name)))
			continue;
		slot->index = index;
		lv_obj_remove_flag(slot->row, LV_OBJ_FLAG_HIDDEN);
		lv_obj_set_pos(slot->row, 4, index * ROW_HEIGHT + 2);
		text_if_changed(slot->label, name);
		lv_obj_set_style_bg_color(slot->row, lv_color_hex(list->has_current &&
			list->current == index ? COLOR_ACTIVE : COLOR_CARD), 0);
	}
	if (!list->count) {
		struct list_slot *slot = &list->slots[0];

		lv_obj_remove_flag(slot->row, LV_OBJ_FLAG_HIDDEN);
		lv_obj_set_pos(slot->row, 4, 2);
		text_if_changed(slot->label, snapshot.scanning ? "\346\211\253\346\217\217\344\270\255..." :
				snapshot.card_ready ? "\346\227\240\351\237\263\344\271\220\346\226\207\344\273\266" :
				snapshot.message[0] ? snapshot.message : "\346\234\252\346\217\222 SD \345\215\241");
	}
}

static void scroll_list(lv_event_t *event)
{
	render_list(lv_event_get_user_data(event), false);
}

static void create_list(struct virtual_list *list, lv_obj_t *container)
{
	list->container = container;
	list->first = -1;
	lv_obj_add_flag(container, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scroll_dir(container, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(container, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_set_style_bg_color(container, lv_color_hex(COLOR_ACCENT), LV_PART_SCROLLBAR);
	lv_obj_set_style_width(container, 3, LV_PART_SCROLLBAR);
	lv_obj_add_event_cb(container, scroll_list, LV_EVENT_SCROLL, list);
	list->spacer = lv_obj_create(container);
	lv_obj_remove_style_all(list->spacer);
	lv_obj_remove_flag(list->spacer, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_size(list->spacer, 1, 1);
	for (unsigned int i = 0; i < VISIBLE_ROWS; i++) {
		struct list_slot *slot = &list->slots[i];

		slot->index = -1;
		slot->row = lv_button_create(container);
		lv_obj_set_size(slot->row, 298, 36);
		lv_obj_remove_flag(slot->row, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_set_style_pad_all(slot->row, 0, 0);
		lv_obj_set_style_radius(slot->row, 6, 0);
		lv_obj_set_style_bg_opa(slot->row, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(slot->row, lv_color_hex(COLOR_CARD), 0);
		lv_obj_set_style_bg_color(slot->row, lv_color_hex(COLOR_ACTIVE), LV_STATE_PRESSED);
		slot->label = lv_label_create(slot->row);
		lv_obj_set_pos(slot->label, 12, 7);
		lv_obj_set_size(slot->label, 274, 22);
		lv_obj_set_style_text_color(slot->label, lv_color_hex(0xeef4fc), 0);
		lv_label_set_long_mode(slot->label, LV_LABEL_LONG_DOT);
		lv_obj_add_event_cb(slot->row, select_row, LV_EVENT_CLICKED, slot);
	}
}

static void navigate(lv_event_t *event)
{
	enum ScreensEnum id = (uintptr_t)lv_event_get_user_data(event);

	if (id == SCREEN_ID_SCREEN_SETTINGS)
		settings_source = source_page;
	source_page = id;
	loadScreen(id);
}

static void settings_back(lv_event_t *event)
{
	(void)event;
	source_page = settings_source;
	loadScreen(source_page);
}

static void playback_action(lv_event_t *event)
{
	send_command((uintptr_t)lv_event_get_user_data(event), 0);
}

static void volume_changed(lv_event_t *event)
{
	int value = lv_slider_get_value(lv_event_get_target_obj(event));

	send_command(PLAYER_SET_VOLUME, value);
}

static void seek_released(lv_event_t *event)
{
	int percent = lv_slider_get_value(lv_event_get_target_obj(event));

	send_command(PLAYER_SEEK, (uint64_t)snapshot.duration_ms * percent / 100);
}

static void brightness_changed(lv_event_t *event)
{
	char value[8];

	brightness = lv_slider_get_value(lv_event_get_target_obj(event));
	ui_port_set_brightness(brightness);
	snprintf(value, sizeof(value), "%u%%", brightness);
	text_if_changed(objects.settings_brightness_value, value);
}

static void set_output(lv_event_t *event)
{
	(void)event;
	send_command(PLAYER_SET_OUTPUT, !snapshot.output);
}

static void set_sleep(lv_event_t *event)
{
	char value[32];

	(void)event;
	sleep_index = (sleep_index + 1) % 4;
	ui_port_set_sleep_timeout(sleep_times[sleep_index]);
	if (sleep_times[sleep_index])
		snprintf(value, sizeof(value), "\346\201\257\345\261\217: %lus", (unsigned long)sleep_times[sleep_index] / 1000);
	else
		snprintf(value, sizeof(value), "\346\201\257\345\261\217: \345\201\234\346\255\242");
	text_if_changed(objects.settings_sleep_label, value);
}

static void style_modes(void)
{
	lv_obj_t *buttons[] = {objects.mode_all, objects.mode_one, objects.mode_shuffle};

	for (unsigned int i = 0; i < 3; i++)
		lv_obj_set_style_bg_color(buttons[i], lv_color_hex(pending_mode == i ?
			COLOR_ACCENT : COLOR_ACTIVE), 0);
}

static void open_mode(lv_event_t *event)
{
	(void)event;
	mode_source = source_page;
	pending_mode = snapshot.mode;
	style_modes();
	loadScreen(SCREEN_ID_OVERLAY_PLAY_MODE);
}

static void choose_mode(lv_event_t *event)
{
	pending_mode = (uintptr_t)lv_event_get_user_data(event);
	style_modes();
}

static void close_mode(lv_event_t *event)
{
	if ((uintptr_t)lv_event_get_user_data(event))
		send_command(PLAYER_SET_MODE, pending_mode);
	loadScreen(mode_source);
}

static void bind_click(lv_obj_t *button, lv_event_cb_t callback, uintptr_t argument)
{
	lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, (void *)argument);
}

void ui_controller_init(void)
{
	create_list(&library, objects.library_list);
	bind_click(objects.library_settings, navigate, SCREEN_ID_SCREEN_SETTINGS);
	bind_click(objects.player_settings, navigate, SCREEN_ID_SCREEN_SETTINGS);
	bind_click(objects.library_open_player, navigate, SCREEN_ID_SCREEN_PLAYER);
	bind_click(objects.player_library, navigate, SCREEN_ID_SCREEN_LIBRARY);
	bind_click(objects.screen_player_back, navigate, SCREEN_ID_SCREEN_LIBRARY);
	bind_click(objects.screen_settings_back, settings_back, 0);
	bind_click(objects.library_play, playback_action, PLAYER_TOGGLE);
	bind_click(objects.player_play, playback_action, PLAYER_TOGGLE);
	bind_click(objects.library_previous, playback_action, PLAYER_PREVIOUS);
	bind_click(objects.player_previous, playback_action, PLAYER_PREVIOUS);
	bind_click(objects.library_next, playback_action, PLAYER_NEXT);
	bind_click(objects.player_next, playback_action, PLAYER_NEXT);
	bind_click(objects.settings_rescan, playback_action, PLAYER_RESCAN);
	bind_click(objects.library_mode, open_mode, 0);
	bind_click(objects.player_mode, open_mode, 0);
	bind_click(objects.mode_all, choose_mode, PLAYER_REPEAT_ALL);
	bind_click(objects.mode_one, choose_mode, PLAYER_REPEAT_ONE);
	bind_click(objects.mode_shuffle, choose_mode, PLAYER_SHUFFLE);
	bind_click(objects.mode_done, close_mode, 1);
	bind_click(objects.mode_cancel, close_mode, 0);
	bind_click(objects.settings_output, set_output, 0);
	bind_click(objects.settings_sleep, set_sleep, 0);
	lv_obj_add_event_cb(objects.player_volume, volume_changed, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(objects.settings_volume, volume_changed, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(objects.settings_brightness, brightness_changed, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(objects.player_seek, seek_released, LV_EVENT_RELEASED, NULL);
	lv_slider_set_range(objects.settings_brightness, 10, 100);
}

static void slider_if_idle(lv_obj_t *slider, int value)
{
	if (!lv_obj_has_state(slider, LV_STATE_PRESSED) && lv_slider_get_value(slider) != value)
		lv_slider_set_value(slider, value, LV_ANIM_OFF);
}

void ui_controller_tick(void)
{
	char buffer[96];
	static uint16_t old_current = UINT16_MAX;
	static bool old_has_current;
	bool has_current, changed;

	if (lv_tick_get() - last_update < 200)
		return;
	last_update = lv_tick_get();
	player_get_snapshot(&snapshot);
	changed = previous_version != snapshot.library_version;
	has_current = snapshot.title[0] != '\0';
	if (changed) {
		library.count = snapshot.count;
		lv_obj_set_pos(library.spacer, 0, snapshot.count ? snapshot.count * ROW_HEIGHT - 1 : 0);
		lv_obj_scroll_to_y(library.container, 0, LV_ANIM_OFF);
		previous_version = snapshot.library_version;
	}
	if (changed || old_current != snapshot.current || old_has_current != has_current || !snapshot.count) {
		library.current = snapshot.current;
		library.has_current = has_current;
		render_list(&library, true);
		old_current = snapshot.current;
		old_has_current = has_current;
	}
	text_if_changed(objects.library_status, snapshot.scanning ? "..." :
			snapshot.card_ready ? "SD" :
			snapshot.message[0] && strcmp(snapshot.message, "\346\234\252\346\217\222 SD \345\215\241") ? "ERR" : "NO SD");
	text_if_changed(objects.player_title, has_current ? snapshot.title : "\350\257\267\351\200\211\346\213\251\351\237\263\344\271\220");
	text_if_changed(objects.library_open_player_label, has_current ? snapshot.title : "\350\257\267\351\200\211\346\213\251\351\237\263\344\271\220");
	text_if_changed(objects.player_artist, snapshot.message[0] ? snapshot.message :
			    snapshot.playing ? "SD \345\215\241 / \346\222\255\346\224\276" : "SD \345\215\241 / \346\232\202\345\201\234");
	snprintf(buffer, sizeof(buffer), "%lu Hz / %s", (unsigned long)snapshot.sample_rate,
		 snapshot.is_mp3 ? "MP3" : "WAV");
	text_if_changed(objects.player_format, buffer);
	text_if_changed(objects.library_play_label, snapshot.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
	text_if_changed(objects.player_play_label, snapshot.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
	text_if_changed(objects.library_mode_label, snapshot.mode == PLAYER_SHUFFLE ? LV_SYMBOL_SHUFFLE : LV_SYMBOL_LOOP);
	text_if_changed(objects.player_mode_label, snapshot.mode == PLAYER_SHUFFLE ? LV_SYMBOL_SHUFFLE : LV_SYMBOL_LOOP);
	snprintf(buffer, sizeof(buffer), "%02lu:%02lu", (unsigned long)snapshot.elapsed_ms / 60000,
		 (unsigned long)snapshot.elapsed_ms / 1000 % 60);
	text_if_changed(objects.player_elapsed, buffer);
	snprintf(buffer, sizeof(buffer), "%02lu:%02lu", (unsigned long)snapshot.duration_ms / 60000,
		 (unsigned long)snapshot.duration_ms / 1000 % 60);
	text_if_changed(objects.player_duration, buffer);
	if (snapshot.duration_ms)
		slider_if_idle(objects.player_seek, (uint64_t)snapshot.elapsed_ms * 100 / snapshot.duration_ms);
	else
		slider_if_idle(objects.player_seek, 0);
	slider_if_idle(objects.player_volume, snapshot.volume);
	slider_if_idle(objects.settings_volume, snapshot.volume);
	snprintf(buffer, sizeof(buffer), "%u%%", snapshot.volume);
	text_if_changed(objects.player_volume_value, buffer);
	text_if_changed(objects.settings_volume_value, buffer);
	text_if_changed(objects.settings_output_label, snapshot.output ? "\350\276\223\345\207\272: \346\211\254\345\243\260\345\231\250" : "\350\276\223\345\207\272: \350\200\263\346\234\272");
}

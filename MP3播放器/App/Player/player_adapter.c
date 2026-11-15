#include "player.h"
#include "playlist.h"
#include "app_key.h"
#include "mp3_player.h"
#include "main.h"
#include <stdio.h>
#include <string.h>

int player_start(void)
{
    return 0;
}

int player_send(enum player_command command, uint32_t value)
{
    switch (command) {
    case PLAYER_SELECT:
        return app_key_send_cmd(KEY_CMD_SELECT, value);
    case PLAYER_TOGGLE:
        return app_key_send_cmd(KEY_CMD_TOGGLE_PLAY, 0);
    case PLAYER_NEXT:
        return app_key_send_cmd(KEY_CMD_NEXT, 0);
    case PLAYER_PREVIOUS:
        return app_key_send_cmd(KEY_CMD_PREV, 0);
    case PLAYER_SET_MODE:
        return app_key_send_cmd(KEY_CMD_SET_MODE, value);
    case PLAYER_SET_VOLUME: {
        static int last_index = -1;
        uint8_t index = (uint8_t)((value * (APP_VOL_LEVELS - 1) + 50) / 100);

        if ((int)index == last_index) return 0;
        last_index = index;
        return app_key_send_cmd(KEY_CMD_SET_VOLUME, index);
    }
    case PLAYER_RESCAN:
        return app_key_send_cmd(KEY_CMD_RESCAN, 0);
    case PLAYER_SEEK:
        return app_key_send_cmd(KEY_CMD_SEEK, value);
    case PLAYER_SET_OUTPUT:
        return app_key_send_cmd(KEY_CMD_SET_OUTPUT, value);
    default:
        return -1;
    }
}

void player_get_snapshot(struct player_snapshot *snapshot)
{
    uint8_t vol_index;

    if (snapshot == NULL) return;

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->count = playlist_count();
    snapshot->current = playlist_current();
    snapshot->library_version = playlist_version();
    snapshot->elapsed_ms = mp3_player_position_ms();
    snapshot->duration_ms = mp3_player_duration_ms();
    snapshot->sample_rate = mp3_player_sample_rate();
    snapshot->mode = (enum player_mode)playlist_mode();
    snapshot->output = 1;
    snapshot->is_mp3 = true;
    snapshot->card_ready = (playlist_count() > 0);
    snapshot->playing = mp3_player_is_playing() && !mp3_player_is_paused();

    vol_index = app_key_volume_get();
    snapshot->volume = (uint8_t)(vol_index * 100 / (APP_VOL_LEVELS - 1));

    if (snapshot->count > 0) {
        strncpy(snapshot->title, playlist_name(snapshot->current),
                sizeof(snapshot->title) - 1);
    }
}

int player_get_track_name(uint16_t index, char *name, size_t size)
{
    if (name == NULL || size == 0 || index >= playlist_count()) return -1;

    strncpy(name, playlist_name(index), size - 1);
    name[size - 1] = '\0';
    return 0;
}

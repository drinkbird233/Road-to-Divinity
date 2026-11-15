/* SPDX-License-Identifier: MIT */
#ifndef PLAYER_H
#define PLAYER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum player_mode { PLAYER_REPEAT_ALL, PLAYER_REPEAT_ONE, PLAYER_SHUFFLE };
enum player_command {
	PLAYER_SELECT, PLAYER_TOGGLE, PLAYER_NEXT, PLAYER_PREVIOUS,
	PLAYER_SET_MODE, PLAYER_SET_VOLUME, PLAYER_SET_OUTPUT, PLAYER_RESCAN,
	PLAYER_SEEK,
};

struct player_snapshot {
	uint16_t count;
	uint16_t current;
	uint32_t library_version;
	uint32_t elapsed_ms;
	uint32_t duration_ms;
	uint32_t sample_rate;
	uint32_t underruns;
	enum player_mode mode;
	uint8_t volume;
	uint8_t output; /* 0 = headphones, 1 = speakers. */
	bool playing;
	bool scanning;
	bool card_ready;
	bool is_mp3;
	char title[96];
	char message[64];
};

int player_start(void);
int player_send(enum player_command command, uint32_t value);
void player_get_snapshot(struct player_snapshot *snapshot);
int player_get_track_name(uint16_t index, char *name, size_t size);
#endif

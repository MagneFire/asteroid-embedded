/* The media service as asteroid-btsyncd has it: the phone writes title
 * (7001), album (7002), artist (7003) as UTF-8, playing (7004) and volume
 * 0-100 (7006) as one byte; the watch asks for previous, next, play, pause
 * (one byte) or a volume (command 4 and the new volume) by notifying 7005.
 * Volume up and down step the last known volume by 10. */
#include <string.h>
#include "asteroid.h"
#include "bt/bt.h"
#include "media.h"

#define VOLUME_STEP 10

static struct media state = { .volume = -1 };
static uint32_t serial;

static void text(char *out, unsigned size, const uint8_t *p, unsigned n)
{
	unsigned k = 0;
	while (k < n && k < size - 1 && p[k])
		k++;
	memcpy(out, p, k);
	out[k] = 0;
}

void media_write(uint16_t chr, const uint8_t *p, unsigned n)
{
	switch (chr) {
	case 0x7001:
		text(state.title, sizeof state.title, p, n);
		break;
	case 0x7002:
		text(state.album, sizeof state.album, p, n);
		break;
	case 0x7003:
		text(state.artist, sizeof state.artist, p, n);
		break;
	case 0x7004:
		if (!n)
			return;
		state.playing = p[0] != 0;
		break;
	case 0x7006:
		if (!n)
			return;
		state.volume = p[0] > 100 ? 100 : p[0];
		break;
	default:
		return;
	}
	serial++;
	log_printf("media: \"%s\" by \"%s\", %s, volume %d\n", state.title, state.artist, state.playing ? "playing" : "paused",
		   state.volume);
}

const struct media *media_get(void)
{
	return &state;
}

uint32_t media_serial(void)
{
	return serial;
}

bool media_command(unsigned command)
{
	uint8_t v[2] = { (uint8_t)command, 0 };
	if (command == MEDIA_VOLUME_UP || command == MEDIA_VOLUME_DOWN) {
		int vol = state.volume < 0 ? 50 : state.volume;
		vol += command == MEDIA_VOLUME_UP ? VOLUME_STEP : -VOLUME_STEP;
		state.volume = vol < 0 ? 0 : vol > 100 ? 100 : vol;
		serial++;
		v[0] = MEDIA_VOLUME;
		v[1] = (uint8_t)state.volume;
	} else if (command > MEDIA_PAUSE) {
		return false;
	}
	bool sent = !bt_gatt_media_command(v, v[0] == MEDIA_VOLUME ? 2 : 1);
	log_printf("media: command %u%s\n", v[0], sent ? "" : " not sent (not subscribed)");
	return sent;
}

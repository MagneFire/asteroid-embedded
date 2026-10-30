/* The weather service as asteroid-btsyncd has it: the city name (8001,
 * UTF-8) and, for five days starting today, OpenWeatherMap condition ids
 * (8002), minimum (8003) and maximum (8004) temperatures in kelvin, each a
 * big-endian 16-bit value per day. The ids write marks the time of day 0.
 * The last forecast is kept in the key/value store under "weather", saved
 * a few seconds after the phone's last write. */
#include <string.h>
#include "asteroid.h"
#include "storage/storage.h"
#include "weather.h"

#define SAVE_DELAY_MS 3000

static struct weather state;
static uint32_t serial, save_at;
static bool dirty;

static unsigned values(uint16_t *out, const uint8_t *p, unsigned n)
{
	unsigned k = n / 2 < WEATHER_DAYS ? n / 2 : WEATHER_DAYS;
	for (unsigned i = 0; i < k; i++)
		out[i] = (uint16_t)(p[2 * i] << 8 | p[2 * i + 1]);
	return k;
}

void weather_write(uint16_t chr, const uint8_t *p, unsigned n)
{
	switch (chr) {
	case 0x8001: {
		unsigned k = 0;
		while (k < n && k < sizeof state.city - 1 && p[k])
			k++;
		memcpy(state.city, p, k);
		state.city[k] = 0;
		break;
	}
	case 0x8002:
		memset(state.id, 0, sizeof state.id);
		state.days = (uint16_t)values(state.id, p, n);
		state.time = pmic_rtc_read();
		break;
	case 0x8003:
		values(state.min, p, n);
		break;
	case 0x8004:
		values(state.max, p, n);
		break;
	default:
		return;
	}
	serial++;
	dirty = true;
	save_at = time_ms() + SAVE_DELAY_MS;
	log_printf("weather: %s, %u days, day 0 id %u, %u-%u K\n", state.city, state.days, state.id[0], state.min[0],
		   state.max[0]);
}

const struct weather *weather_get(void)
{
	return &state;
}

uint32_t weather_serial(void)
{
	return serial;
}

void weather_load(void)
{
	struct weather w;
	if (kv_get("weather", &w, sizeof w) != (int)sizeof w || w.days > WEATHER_DAYS)
		return;
	w.city[sizeof w.city - 1] = 0;
	state = w;
	serial++;
}

void weather_poll(void)
{
	if (!dirty || (int32_t)(time_ms() - save_at) < 0)
		return;
	dirty = false;
	int r = kv_set("weather", &state, sizeof state);
	if (r)
		log_printf("weather: not saved (%d)\n", r);
}

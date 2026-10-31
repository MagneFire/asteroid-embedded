/* User settings, one byte each in the key-value store under their own
 * key, read on first use. Changing one only marks it; main applies it,
 * and settings_poll writes the changed ones to the store once they have
 * been quiet for FLUSH_MS (a brightness drag is many changes), or
 * settings_flush right away before a sleep or a reboot. */
#include "asteroid.h"
#include "storage/storage.h"

static const struct {
	const char *key;
	uint8_t init;
} table[SETTINGS] = {
	[SETTING_BRIGHTNESS] = { "brightness", 100 },
	[SETTING_BLUETOOTH] = { "bt", 1 },
	[SETTING_VIBRATION] = { "vibrate", 1 },
	[SETTING_AOD] = { "aod", 1 },
	[SETTING_CLOCK12] = { "clock12", 0 },
	[SETTING_WATCHFACE] = { "watchface", 0 },
	[SETTING_NIGHTSTAND] = { "nightstand", 1 },
	[SETTING_LAUNCHER] = { "launcher", 0 },
	[SETTING_TILT] = { "tilt", 1 },
	[SETTING_TAP] = { "tap", 1 },
	[SETTING_FAHRENHEIT] = { "fahrenheit", 0 },
	[SETTING_NIGHTSTAND_BRIGHTNESS] = { "nightstand.brightness", 30 },
	[SETTING_NIGHTSTAND_DELAY] = { "nightstand.delay", 5 },
	[SETTING_NIGHTSTAND_AOD] = { "nightstand.aod", 1 },
	[SETTING_BURN_IN] = { "burnin", 1 },
	[SETTING_CINEMA_AOD] = { "cinema.aod", 1 },
};

#define FLUSH_MS 2000

static uint8_t values[SETTINGS];
static bool loaded;
static uint32_t dirty, last_set;
_Static_assert(SETTINGS <= 32, "dirty mask too small");

const char *setting_name(enum setting s)
{
	return table[s].key;
}

int setting_get(enum setting s)
{
	if (!loaded) {
		for (unsigned i = 0; i < SETTINGS; i++)
			if (kv_get(table[i].key, &values[i], 1) != 1)
				values[i] = table[i].init;
		loaded = true;
	}
	return values[s];
}

void setting_set(enum setting s, int v)
{
	setting_get(s);
	if (values[s] == (uint8_t)v)
		return;
	values[s] = (uint8_t)v;
	dirty |= 1u << s;
	last_set = time_ms();
}

void settings_flush(void)
{
	for (unsigned i = 0; dirty && i < SETTINGS; i++)
		if (dirty & 1u << i) {
			dirty &= ~(1u << i);
			kv_set(table[i].key, &values[i], 1);
		}
}

void settings_poll(uint32_t now)
{
	if (dirty && now - last_set >= FLUSH_MS)
		settings_flush();
}

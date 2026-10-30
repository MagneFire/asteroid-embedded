/* User settings, one byte each in the key-value store under their own
 * key, read on first use. Changing one only stores it; main applies it. */
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
};

static uint8_t values[SETTINGS];
static bool loaded;

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
	values[s] = (uint8_t)v;
	kv_set(table[s].key, &values[s], 1);
}

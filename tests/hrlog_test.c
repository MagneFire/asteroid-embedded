/* The heart rate log on the host: measuring at whole intervals, the
 * timeout, the interval off and changed, the clock set back, the ring
 * and a reload from the store. */
#include <stdio.h>
#include <string.h>
#include "asteroid.h"
#include "storage/storage.h"
#include "hrlog.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static uint8_t stored[1024];
static int stored_len = -2, writes;

int kv_get(const char *key, void *buf, unsigned size)
{
	if (stored_len > 0)
		memcpy(buf, stored, (unsigned)stored_len < size ? (unsigned)stored_len : size);
	return stored_len;
}
int kv_set(const char *key, const void *val, unsigned len)
{
	memcpy(stored, val, len);
	stored_len = (int)len;
	writes++;
	return 0;
}

int main(void)
{
	uint32_t t[HRLOG_SAMPLES];
	uint8_t bpm[HRLOG_SAMPLES];
	hrlog_init();

	/* Off: never on, nothing to wake for. */
	CHECK(!hrlog_poll(1000, 0, 0));
	CHECK(hrlog_next_ms(1000, 0) == UINT32_MAX);

	/* Every 10 min: on at the next whole 10 min, off with a reading. */
	CHECK(!hrlog_poll(1000, 10, 0));
	CHECK(hrlog_next_ms(1000, 10) == 200000);
	CHECK(!hrlog_poll(1199, 10, 0));
	CHECK(hrlog_poll(1200, 10, 0));
	CHECK(hrlog_next_ms(1200, 10) == HRLOG_TIMEOUT_S * 1000);
	CHECK(hrlog_poll(1230, 10, 0));
	CHECK(!hrlog_poll(1240, 10, 72));
	CHECK(writes == 1);
	CHECK(hrlog_next_ms(1240, 10) == 560000);

	/* No reading in time: off, nothing kept, on again at the next slot. */
	CHECK(hrlog_poll(1800, 10, 0));
	CHECK(!hrlog_poll(1800 + HRLOG_TIMEOUT_S, 10, 0));
	CHECK(writes == 1);
	CHECK(!hrlog_poll(2000, 10, 0));
	CHECK(hrlog_poll(2400, 10, 0));
	CHECK(!hrlog_poll(2410, 10, 80));

	/* A longer interval: the next slot moves out. The clock set back:
	 * the slot comes back with it. */
	CHECK(!hrlog_poll(2500, 60, 0));
	CHECK(hrlog_next_ms(2500, 60) == (3600 - 2500) * 1000);
	CHECK(!hrlog_poll(100, 60, 0));
	CHECK(hrlog_next_ms(100, 60) == 3500000);

	CHECK(hrlog_get(0, t, bpm, HRLOG_SAMPLES) == 2);
	CHECK(t[0] == 1240 && bpm[0] == 72 && t[1] == 2410 && bpm[1] == 80);
	CHECK(hrlog_get(2000, t, bpm, HRLOG_SAMPLES) == 1 && bpm[0] == 80);

	/* The ring: the oldest go. */
	uint32_t now = 10000 * 600;
	CHECK(!hrlog_poll(now - 1, 10, 0));
	for (unsigned i = 0; i < HRLOG_SAMPLES + 5; i++, now += 600) {
		CHECK(hrlog_poll(now, 10, 0));
		hrlog_poll(now + 20, 10, 60 + i % 100);
	}
	unsigned n = hrlog_get(0, t, bpm, HRLOG_SAMPLES);
	CHECK(n == HRLOG_SAMPLES);
	CHECK(t[0] == 10005 * 600 + 20 && t[n - 1] == now - 600 + 20);

	/* Reloaded from the store. */
	hrlog_init();
	CHECK(hrlog_get(0, t, bpm, HRLOG_SAMPLES) == HRLOG_SAMPLES && t[0] == 10005 * 600 + 20);

	printf(failures ? "hrlog_test: %d failures\n" : "hrlog_test: ok\n", failures);
	return failures != 0;
}

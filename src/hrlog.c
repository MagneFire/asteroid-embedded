/* The heart rate now and then: at each whole interval of the clock the
 * sensor goes on until a reading can be kept, or for HRLOG_TIMEOUT_S
 * without one, and off again. The last HRLOG_SAMPLES readings are kept,
 * a day of them at 10 minutes, and saved with each new one. */
#include <string.h>
#include "asteroid.h"
#include "storage/storage.h"
#include "hrlog.h"

#define KV_HRLOG	"hr.log"

static struct saved {
	uint16_t next, count;
	uint32_t t[HRLOG_SAMPLES];
	uint8_t bpm[HRLOG_SAMPLES];
} saved;
static uint32_t next_at, started, interval;
static bool measuring;

void hrlog_init(void)
{
	if (kv_get(KV_HRLOG, &saved, sizeof saved) != (int)sizeof saved || saved.next >= HRLOG_SAMPLES ||
	    saved.count > HRLOG_SAMPLES)
		memset(&saved, 0, sizeof saved);
	next_at = 0;
	measuring = false;
}

static void record(uint32_t rtc, uint32_t bpm)
{
	saved.t[saved.next] = rtc;
	saved.bpm[saved.next] = bpm > 255 ? 255 : (uint8_t)bpm;
	saved.next = (saved.next + 1) % HRLOG_SAMPLES;
	if (saved.count < HRLOG_SAMPLES)
		saved.count++;
	kv_set(KV_HRLOG, &saved, sizeof saved);
}

bool hrlog_poll(uint32_t rtc, unsigned interval_min, uint32_t steady_bpm)
{
	if (!interval_min) {
		measuring = false;
		next_at = 0;
		return false;
	}
	uint32_t period = interval_min * 60, slot = (rtc / period + 1) * period;
	/* First use, a new interval or the clock set back. */
	if (!next_at || interval != interval_min || next_at > slot)
		next_at = slot;
	interval = interval_min;
	if (!measuring && rtc >= next_at) {
		measuring = true;
		started = rtc;
	}
	if (measuring && (steady_bpm || rtc - started >= HRLOG_TIMEOUT_S || rtc < started)) {
		if (steady_bpm)
			record(rtc, steady_bpm);
		measuring = false;
		next_at = slot;
	}
	return measuring;
}

uint32_t hrlog_next_ms(uint32_t rtc, unsigned interval_min)
{
	if (!interval_min)
		return UINT32_MAX;
	uint32_t at = measuring ? started + HRLOG_TIMEOUT_S : next_at ? next_at : (rtc / (interval_min * 60) + 1) * interval_min * 60;
	return at > rtc ? (at - rtc) * 1000 : 0;
}

unsigned hrlog_get(uint32_t since, uint32_t *t, uint8_t *bpm, unsigned max)
{
	unsigned n = 0;
	for (unsigned i = 0; i < saved.count && n < max; i++) {
		unsigned k = (saved.next + HRLOG_SAMPLES - saved.count + i) % HRLOG_SAMPLES;
		if (saved.t[k] < since)
			continue;
		t[n] = saved.t[k];
		bpm[n++] = saved.bpm[k];
	}
	return n;
}

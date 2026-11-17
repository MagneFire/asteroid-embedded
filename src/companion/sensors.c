/* Events wait in one queue of PROTO_SNS_EVENTS, sent when it is full,
 * when the shortest latency asked for has passed since the first, or
 * at once for an event of the on-change sensors or a flush. A report
 * carries only what fits in link_send, so the queue goes out in as
 * many reports as that takes. The times are the MCU's clock moved by
 * the AP's elapsedRealtimeNano at its last activate. The subscriptions
 * are kept across a warm reset, for an AP that keeps the panel; the
 * clock starts over then, so the times go on from the last one sent,
 * early by the time the reset took. */
#include <string.h>
#include "asteroid.h"
#include "sensors/hub.h"
#include "companion/link.h"
#include "companion/proto.h"
#include "companion/sensors.h"
#include "companion/wire.h"

/* The hub's 26 Hz. */
#define ACCEL_MIN_MS	39
#define DEFAULT_MS	200
#define STEPS_MS	1000
#define DETECTOR_MAX	4
#define FLUSH_MAX	8
/* Keeps the deadlines' differences in an int32_t. */
#define LONGEST_MS	3600000
#define ACCURACY_HIGH	3
#define MG_TO_MS2	0.00980665f
#define KEPT_MAGIC	0x534e5331u	/* "SNS1" */

#ifdef HOST_TEST
#define KEPT
#else
#define KEPT		__attribute__((section(".kept")))
#endif

struct sub {
	bool on;
	uint32_t period_ms, latency_ms;
};

enum { ACCEL, COUNTER, DETECTOR, TILT, SUBS };
static const uint32_t handles[SUBS] = { PROTO_SNS_ACCEL, PROTO_SNS_STEP_COUNTER, PROTO_SNS_STEP_DETECTOR,
					PROTO_SNS_WRIST_TILT };
static struct kept {
	uint32_t magic;
	struct sub subs[SUBS];
	int64_t offset_ns;
	bool synced;
	uint32_t sum;
	/* Changes with every event, so outside the sum. */
	uint64_t last_ns;
} kept KEPT;
static struct proto_sns_event queue[PROTO_SNS_EVENTS];
static unsigned queued, per_report, taken;
static uint32_t first_ms, accel_ms, steps_ms, last_steps = HUB_NONE, seen_tilts;
static bool urgent;
static uint32_t flushes[FLUSH_MAX];
static unsigned flush_n;
static struct {
	uint32_t commands, bad, flushes, events, reports, dropped;
} count;

static uint32_t kept_sum(void)
{
	const uint8_t *p = (const uint8_t *)&kept;
	uint32_t h = 2166136261u;
	for (unsigned i = 0; i < offsetof(struct kept, sum); i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

static void keep(void)
{
	kept.magic = KEPT_MAGIC;
	kept.sum = kept_sum();
}

static int sub_of(uint32_t handle)
{
	for (int i = 0; i < SUBS; i++)
		if (handles[i] == handle)
			return i;
	return -1;
}

static uint64_t mcu_ns(void)
{
	uint64_t c = timer_count();
	uint32_t hz = timer_hz();
	return c / hz * 1000000000ull + c % hz * 1000000000ull / hz;
}

static uint64_t ap_ns(uint64_t ns) { return (uint64_t)((int64_t)ns + kept.offset_ns); }

static bool push(uint32_t handle, uint64_t ns, const uint32_t w[4], bool now)
{
	if (queued == PROTO_SNS_EVENTS) {
		count.dropped++;
		return false;
	}
	if (!queued)
		first_ms = time_ms();
	kept.last_ns = ap_ns(ns);
	queue[queued++] = (struct proto_sns_event){ handle, kept.last_ns, { w[0], w[1], w[2], w[3] } };
	count.events++;
	urgent |= now;
	return true;
}

static void push_value(uint32_t handle, float v)
{
	uint32_t w[4] = { proto_float_bits(v), 0, 0, 0 };
	push(handle, mcu_ns(), w, true);
}

static void accel_share(void) { hub_accel_share(kept.subs[ACCEL].on); }

static void activate(int i, bool on)
{
	if (kept.subs[i].on == on)
		return;
	kept.subs[i].on = on;
	uint32_t now = time_ms();
	if (i == ACCEL) {
		accel_share();
		accel_ms = now;
	} else if (i == COUNTER || i == DETECTOR) {
		steps_ms = now;
		if (on)
			last_steps = HUB_NONE;
	} else if (i == TILT)
		seen_tilts = hub_tilts();
	keep();
}

static uint32_t ms_of(uint64_t ns) { return ns / 1000000 < LONGEST_MS ? (uint32_t)(ns / 1000000) : LONGEST_MS; }

bool sns_command(const uint8_t *p, unsigned n)
{
	struct proto_sns_cmd c;
	count.commands++;
	if (!proto_sns_cmd(p, n, &c)) {
		count.bad++;
		return false;
	}
	int i = sub_of(c.handle);
	if (c.op == PROTO_SNS_ACTIVATE) {
		if (c.time_ns) {
			kept.offset_ns = (int64_t)(c.time_ns - mcu_ns());
			kept.synced = true;
			kept.last_ns = c.time_ns;
			keep();
		}
		if (i >= 0)
			activate(i, c.enable);
	} else if (c.op == PROTO_SNS_BATCH && i >= 0) {
		kept.subs[i].period_ms = ms_of(c.period_ns);
		kept.subs[i].latency_ms = ms_of(c.latency_ns);
		keep();
	} else if (c.op == PROTO_SNS_FLUSH) {
		count.flushes++;
		if (flush_n < FLUSH_MAX)
			flushes[flush_n++] = c.handle;
		else
			count.dropped++;
	}
	return true;
}

void sns_reset(void)
{
	for (int i = 0; i < SUBS; i++)
		kept.subs[i] = (struct sub){ 0 };
	accel_share();
	queued = flush_n = taken = 0;
	urgent = kept.synced = false;
	kept.offset_ns = 0;
	kept.last_ns = 0;
	keep();
}

/* An event's length does not depend on its values, so the encoder
 * tells how many fit with empty ones. */
static unsigned fit(void)
{
	static const struct proto_sns_event none[PROTO_SNS_EVENTS];
	uint8_t body[PROTO_SNS_REPORT_MAX], env[LINK_SEND_MAX];
	unsigned n = PROTO_SNS_EVENTS;
	for (; n > 1; n--)
		if (wire_wrap_raw(env, sizeof env, WIRE_CHAN_SENSORS, body, proto_sns_report(body, sizeof body, none, n)))
			break;
	return n;
}

void sns_start(bool keep_subs)
{
	per_report = fit();
	if (!keep_subs || kept.magic != KEPT_MAGIC || kept.sum != kept_sum()) {
		sns_reset();
		return;
	}
	if (kept.synced)
		kept.offset_ns = (int64_t)(kept.last_ns - mcu_ns());
	keep();
	uint32_t now = time_ms();
	accel_ms = steps_ms = now;
	seen_tilts = hub_tilts();
	accel_share();
	log_printf("sensors: the AP's subscriptions kept from before the reset\n");
}

static uint32_t period(int i)
{
	uint32_t ms = kept.subs[i].period_ms ? kept.subs[i].period_ms : DEFAULT_MS;
	return i == ACCEL && ms < ACCEL_MIN_MS ? ACCEL_MIN_MS : ms;
}

static void sample_accel(uint32_t now)
{
	if (!kept.subs[ACCEL].on || (int32_t)(now - accel_ms) < 0)
		return;
	uint32_t p = period(ACCEL);
	accel_ms += p;
	if ((int32_t)(now - accel_ms) >= 0)
		accel_ms = now + p;
	int16_t g[3];
	if (!hub_accel(g))
		return;
	uint32_t w[4] = { proto_float_bits(g[0] * MG_TO_MS2), proto_float_bits(g[1] * MG_TO_MS2),
			  proto_float_bits(g[2] * MG_TO_MS2), ACCURACY_HIGH };
	push(PROTO_SNS_ACCEL, mcu_ns(), w, false);
}

static void sample_steps(uint32_t now)
{
	if ((!kept.subs[COUNTER].on && !kept.subs[DETECTOR].on) || (int32_t)(now - steps_ms) < 0)
		return;
	steps_ms = now + STEPS_MS;
	uint32_t s = hub_steps();
	if (s == HUB_NONE)
		return;
	if (last_steps == HUB_NONE || s < last_steps) {
		if (kept.subs[COUNTER].on)
			push_value(PROTO_SNS_STEP_COUNTER, (float)s);
	} else if (s > last_steps) {
		if (kept.subs[COUNTER].on)
			push_value(PROTO_SNS_STEP_COUNTER, (float)s);
		for (uint32_t k = 0; kept.subs[DETECTOR].on && k < s - last_steps && k < DETECTOR_MAX; k++)
			push_value(PROTO_SNS_STEP_DETECTOR, 1.0f);
	}
	last_steps = s;
}

void sns_pass(void)
{
	uint32_t now = time_ms();
	sample_accel(now);
	sample_steps(now);
	uint32_t t = hub_tilts();
	if (kept.subs[TILT].on && t != seen_tilts)
		push_value(PROTO_SNS_WRIST_TILT, 1.0f);
	seen_tilts = t;
	while (flush_n && queued < PROTO_SNS_EVENTS) {
		uint32_t w[4] = { flushes[0], 0, 0, 0 };
		push(PROTO_SNS_META, mcu_ns(), w, true);
		for (unsigned k = 1; k < flush_n; k++)
			flushes[k - 1] = flushes[k];
		flush_n--;
	}
}

static uint32_t latency_ms(void)
{
	uint32_t ms = UINT32_MAX;
	for (int i = 0; i < SUBS; i++)
		if (kept.subs[i].on && kept.subs[i].latency_ms < ms)
			ms = kept.subs[i].latency_ms;
	return ms == UINT32_MAX ? 0 : ms;
}

static uint32_t report_in(uint32_t now)
{
	if (!queued)
		return UINT32_MAX;
	uint32_t gone = now - first_ms, ms = latency_ms();
	if (urgent || queued == PROTO_SNS_EVENTS || flush_n || gone >= ms)
		return 0;
	return ms - gone;
}

bool sns_due(void) { return report_in(time_ms()) == 0; }

static void drop_front(unsigned n)
{
	queued -= n;
	memmove(queue, queue + n, queued * sizeof queue[0]);
	/* The rest of a queue that was due follows at once. */
	urgent = queued != 0;
}

unsigned sns_report(uint8_t *out, unsigned cap)
{
	uint8_t body[PROTO_SNS_REPORT_MAX];
	taken = queued < per_report ? queued : per_report;
	unsigned n = proto_sns_report(body, sizeof body, queue, taken);
	n = n ? wire_wrap_raw(out, cap, WIRE_CHAN_SENSORS, body, n) : 0;
	if (!n) {
		count.dropped += taken;
		drop_front(taken);
		taken = 0;
	}
	return n;
}

void sns_sent(void)
{
	count.reports++;
	drop_front(taken);
	taken = 0;
}

/* A flush's answer stays for the next report; samples are dropped. */
void sns_unsent(void)
{
	unsigned metas = 0;
	for (unsigned i = 0; i < taken; i++)
		if (queue[i].handle == PROTO_SNS_META)
			queue[metas++] = queue[i];
	count.dropped += taken - metas;
	memmove(queue + metas, queue + taken, (queued - taken) * sizeof queue[0]);
	queued -= taken - metas;
	taken = 0;
	urgent |= metas != 0;
}

/* At least 1: a sample due now still needs a pass. */
static uint32_t until(uint32_t t, uint32_t now)
{
	int32_t d = (int32_t)(t - now);
	return d > 0 ? (uint32_t)d : 1;
}

uint32_t sns_next_ms(bool link_free)
{
	uint32_t now = time_ms(), ms = kept.subs[ACCEL].on ? until(accel_ms, now) : UINT32_MAX;
	uint32_t s = kept.subs[COUNTER].on || kept.subs[DETECTOR].on ? until(steps_ms, now) : UINT32_MAX;
	uint32_t r = report_in(now);
	if (s < ms)
		ms = s;
	if ((r || link_free) && r < ms)
		ms = r;
	return ms;
}

void sns_status(void (*say)(const char *fmt, ...))
{
	static const char *const names[SUBS] = { "accelerometer", "step counter", "step detector", "wrist tilt" };
	for (int i = 0; i < SUBS; i++)
		if (kept.subs[i].on)
			say("sensors: %s every %lu ms, latency %lu ms\n", names[i], period(i), kept.subs[i].latency_ms);
	say("sensors: %lu commands (%lu bad), %lu flushes, %lu events in %lu reports of up to %u, %lu dropped, "
	    "%u waiting; AP clock %s\n",
	    count.commands, count.bad, count.flushes, count.events, count.reports, per_report, count.dropped, queued,
	    kept.synced ? "known" : "not known");
}

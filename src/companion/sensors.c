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
#include "sensors/afe4900.h"
#include "sensors/ak09918.h"
#include "sensors/compass.h"
#include "sensors/fusion.h"
#include "sensors/hub.h"
#include "sensors/lsm6dso.h"
#include "sensors/tsl258x.h"
#include "companion/companion.h"
#include "companion/link.h"
#include "companion/proto.h"
#include "companion/sensors.h"
#include "companion/wire.h"

/* The hub's 26 Hz; the fastest rates used of the gyroscope, 52 Hz,
 * and of the magnetometer, 50 Hz. */
#define ACCEL_MIN_MS	39
#define GYRO_MIN_MS	20
#define MAG_MIN_MS	20
/* The compass takes a sample of the magnetometer and the gyroscope
 * this often while either compass sensor is on, as the vendor's
 * library does, whatever the AP asked for. */
#define COMPASS_MS	20
#define ORIENT_MIN_MS	ACCEL_MIN_MS
/* The light sensor's integration. */
#define LIGHT_MIN_MS	100
#define DEFAULT_MS	200
#define STEPS_MS	1000
#define DETECTOR_MAX	4
#define FLUSH_MAX	8
/* Longer than a start takes: the AFE4900's calibration (up to 4 s) and
 * the five intervals of a first reading. */
#define NO_CONTACT_MS	15000
/* Keeps the deadlines' differences in an int32_t. */
#define LONGEST_MS	3600000
/* A lux change smaller than this, and than 1/LUX_PART of the last lux
 * sent, is the sensor's jitter. */
#define LUX_STEP	2
#define LUX_PART	20
/* A chip that did not take what it was asked is asked again, at first
 * a second later and then half as often each time. */
#define RETRY_MS	1000
#define RETRY_MAX_MS	60000
/* The magnetometer and the light sensor share the touch controller's
 * bus, which the AP may use while it has the panel. Stock reads the
 * light sensor then, so they serve the AP's subscriptions while it
 * has the panel; at 0 they wait for the panel to come back. */
#define BUS_WHILE_LENT	1
#define MG_TO_MS2	0.00980665f
#define GYRO_TO_RADS	(LSM6DSO_MDPS_PER_COUNT * 0.001f * 3.14159265f / 180)
#define KEPT_MAGIC	0x534e5332u	/* "SNS2" */

#ifdef HOST_TEST
#define KEPT
#else
#define KEPT		__attribute__((section(".kept")))
#endif

struct sub {
	bool on;
	uint32_t period_ms, latency_ms;
};

enum { ACCEL, COUNTER, DETECTOR, TILT, HEART, GYRO, MAG, LIGHT, ORIENT, SUBS };
static const uint32_t handles[SUBS] = { PROTO_SNS_ACCEL, PROTO_SNS_STEP_COUNTER, PROTO_SNS_STEP_DETECTOR,
					PROTO_SNS_WRIST_TILT, PROTO_SNS_HEART_RATE, PROTO_SNS_GYRO, PROTO_SNS_MAG,
					PROTO_SNS_LIGHT, PROTO_SNS_ORIENTATION };
static const uint32_t min_ms[SUBS] = { [ACCEL] = ACCEL_MIN_MS, [GYRO] = GYRO_MIN_MS, [MAG] = MAG_MIN_MS,
				       [LIGHT] = LIGHT_MIN_MS, [ORIENT] = ORIENT_MIN_MS };
/* Android's axis for each of the AK09918's, from the remap the
 * vendor's firmware applies before its compass library: Android x is
 * the chip's -y and y its x. */
static const struct {
	uint8_t from;
	int8_t sign;
} mag_axes[3] = { { 1, -1 }, { 0, 1 }, { 2, 1 } };
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
static uint32_t due[SUBS];
static uint32_t first_ms, steps_ms, last_steps = HUB_NONE, seen_tilts, heart_ms;
static unsigned seen_readings;
static int32_t last_lux = -1;
static bool urgent, no_contact, lent, settled = true, tilt_owed;
static uint32_t retry_ms, retry_wait = RETRY_MS;
static uint32_t flushes[FLUSH_MAX];
static unsigned flush_n;
static struct {
	uint32_t commands, bad, flushes, events, reports, dropped;
} count;
/* The calibration lasts for the boot, as the vendor's. */
static struct compass compass;
static struct fusion fusion;
static bool compass_ready, field_new;
static float field[3];
static int field_accuracy = -1;
static uint32_t compass_t, compass_due, field_t;

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

static bool push_value(uint32_t handle, float v)
{
	uint32_t w[4] = { proto_float_bits(v), 0, 0, 0 };
	return push(handle, mcu_ns(), w, true);
}

static uint32_t period(int i)
{
	uint32_t ms = kept.subs[i].period_ms ? kept.subs[i].period_ms : DEFAULT_MS;
	return ms < min_ms[i] ? min_ms[i] : ms;
}

static bool compass_on(void) { return kept.subs[MAG].on || kept.subs[ORIENT].on; }

static uint32_t gyro_period(void)
{
	uint32_t ms = kept.subs[GYRO].on ? period(GYRO) : 0;
	if (compass_on() && (!ms || COMPASS_MS < ms))
		ms = COMPASS_MS;
	return ms;
}

static void share(void)
{
	bool bus = BUS_WHILE_LENT || !lent;
	hub_accel_share(kept.subs[ACCEL].on || kept.subs[ORIENT].on);
	hub_tilt_share(kept.subs[TILT].on);
	afe4900_share(kept.subs[HEART].on);
	settled = lsm6dso_gyro_on(gyro_period());
	settled &= ak09918_on(bus && compass_on() ? COMPASS_MS : 0);
	tsl258x_share(bus && kept.subs[LIGHT].on);
	retry_ms = time_ms();
}

/* The light sensor is the watch's too, so its state is asked for each
 * time. */
static bool unsettled(void) { return !settled || !tsl258x_settled(); }

static void settle(uint32_t now)
{
	if (!unsettled()) {
		retry_wait = RETRY_MS;
		return;
	}
	if (now - retry_ms < retry_wait)
		return;
	share();
	retry_wait = retry_wait < RETRY_MAX_MS / 2 ? 2 * retry_wait : RETRY_MAX_MS;
}

void sns_lend(bool to_ap)
{
	lent = to_ap;
	share();
}

void sns_chips_off(void)
{
	tsl258x_lost();
	ak09918_lost();
}

bool sns_active(void)
{
	for (int i = 0; i < SUBS; i++)
		if (kept.subs[i].on)
			return true;
	return false;
}

static void heart_start(uint32_t now)
{
	afe4900_rate(NULL, &seen_readings);
	heart_ms = now;
	no_contact = false;
}

static void activate(int i, bool on)
{
	if (kept.subs[i].on == on)
		return;
	kept.subs[i].on = on;
	uint32_t now = time_ms();
	share();
	due[i] = now;
	if (i == LIGHT)
		last_lux = -1;
	else if (i == HEART)
		heart_start(now);
	else if (i == COUNTER || i == DETECTOR) {
		steps_ms = now;
		if (on)
			last_steps = HUB_NONE;
	} else if (i == TILT) {
		seen_tilts = hub_tilts();
		tilt_owed = false;
	} else if (i == ORIENT)
		fusion_init(&fusion);
	if ((i == MAG || i == ORIENT) && on && kept.subs[MAG].on + kept.subs[ORIENT].on == 1) {
		compass_due = now;
		compass_t = field_t = now;
		field_accuracy = -1;
		compass_restart(&compass);
	}
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
		share();
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
	share();
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

void sns_start(bool lent_boot)
{
	per_report = fit();
	lent = lent_boot;
	if (!lent_boot || kept.magic != KEPT_MAGIC || kept.sum != kept_sum()) {
		sns_reset();
		return;
	}
	if (kept.synced)
		kept.offset_ns = (int64_t)(kept.last_ns - mcu_ns());
	keep();
	uint32_t now = time_ms();
	for (int i = 0; i < SUBS; i++)
		due[i] = now;
	steps_ms = now;
	seen_tilts = hub_tilts();
	share();
	heart_start(now);
	log_printf("sensors: the AP's subscriptions kept from before the reset\n");
}

/* Whether a sample of a periodic sensor is due, moving its deadline
 * on. A sample is read from the output registers on this schedule, not
 * in step with the sensor's own rate. */
static bool sample_due(int i, uint32_t now)
{
	if (!kept.subs[i].on || (int32_t)(now - due[i]) < 0)
		return false;
	uint32_t p = period(i);
	due[i] += p;
	if ((int32_t)(now - due[i]) >= 0)
		due[i] = now + p;
	return true;
}

static void push_vector(uint32_t handle, const int16_t v[3], float scale, uint32_t accuracy)
{
	uint32_t w[4] = { proto_float_bits(v[0] * scale), proto_float_bits(v[1] * scale),
			  proto_float_bits(v[2] * scale), accuracy };
	push(handle, mcu_ns(), w, false);
}

static void push_floats(uint32_t handle, const float v[3], int accuracy)
{
	uint32_t w[4] = { proto_float_bits(v[0]), proto_float_bits(v[1]), proto_float_bits(v[2]), (uint32_t)accuracy };
	push(handle, mcu_ns(), w, false);
}

/* The magnetometer and the gyroscope into the calibration every
 * COMPASS_MS, and with the accelerometer into the orientation; a
 * magnetometer sample out of range is left out. */
static void sample_compass(uint32_t now)
{
	if (!compass_on() || (int32_t)(now - compass_due) < 0)
		return;
	compass_due += COMPASS_MS;
	if ((int32_t)(now - compass_due) >= 0)
		compass_due = now + COMPASS_MS;
	int16_t c[3], g[3], v[3];
	int got = ak09918_read(c);
	bool turned = lsm6dso_gyro(g);
	float raw[3], rate[3], dt = (float)(now - compass_t) / 1000;
	compass_t = now;
	for (int i = 0; i < 3; i++) {
		raw[i] = (float)(mag_axes[i].sign * c[mag_axes[i].from]) * AK09918_UT_PER_COUNT;
		rate[i] = g[i] * GYRO_TO_RADS;
	}
	if (!compass_ready) {
		compass_init(&compass);
		compass_ready = true;
	}
	if (got == AK09918_OK) {
		/* The field turned for as long as since the last sample, not
		 * since the last pass that found none. */
		field_accuracy = compass_feed(&compass, raw, turned ? rate : NULL, (float)(now - field_t) / 1000, field);
		field_t = now;
		field_new = true;
	} else if (got == AK09918_OVERFLOW)
		field_new = false;
	if (kept.subs[ORIENT].on && hub_accel(v)) {
		float a[3] = { v[0], v[1], v[2] };
		bool fresh = got == AK09918_OK;
		fusion_update(&fusion, a, turned ? rate : NULL, fresh ? field : NULL, fresh ? field_accuracy : 0, dt);
	}
}

/* The LSM6DSO's axes are taken as Android's. The gyroscope's zero-rate
 * offset is not removed, hence its accuracy. */
static void sample_vectors(uint32_t now)
{
	int16_t v[3];
	if (sample_due(ACCEL, now) && hub_accel(v))
		push_vector(PROTO_SNS_ACCEL, v, MG_TO_MS2, PROTO_SNS_HIGH);
	if (sample_due(GYRO, now) && lsm6dso_gyro(v))
		push_vector(PROTO_SNS_GYRO, v, GYRO_TO_RADS, PROTO_SNS_LOW);
	if (sample_due(MAG, now) && field_new) {
		push_floats(PROTO_SNS_MAG, field, field_accuracy);
		field_new = false;
	}
}

/* Azimuth, pitch and roll with the field's accuracy. */
static void sample_orientation(uint32_t now)
{
	float out[3];
	if (sample_due(ORIENT, now) && field_accuracy >= 0 && fusion_angles(&fusion, out))
		push_floats(PROTO_SNS_ORIENTATION, out, field_accuracy);
}

/* Lux as the scalar, sent when it changes, as the light sensor is an
 * on-change one. */
static void sample_light(uint32_t now)
{
	if (!sample_due(LIGHT, now))
		return;
	int32_t lux = light_finish(), d = lux - last_lux;
	if (d < 0)
		d = -d;
	if (lux >= 0 && (last_lux < 0 || (d >= LUX_STEP && d * LUX_PART >= last_lux)) &&
	    push_value(PROTO_SNS_LIGHT, (float)lux))
		last_lux = lux;
}

static void sample_steps(uint32_t now)
{
	if ((!kept.subs[COUNTER].on && !kept.subs[DETECTOR].on) || (int32_t)(now - steps_ms) < 0)
		return;
	steps_ms = now + STEPS_MS;
	uint32_t s = hub_steps();
	if (s == HUB_NONE)
		return;
	bool lost = false;
	if (last_steps == HUB_NONE || s < last_steps) {
		if (kept.subs[COUNTER].on)
			lost = !push_value(PROTO_SNS_STEP_COUNTER, (float)s);
	} else if (s > last_steps) {
		if (kept.subs[COUNTER].on)
			lost = !push_value(PROTO_SNS_STEP_COUNTER, (float)s);
		for (uint32_t k = 0; kept.subs[DETECTOR].on && k < s - last_steps && k < DETECTOR_MAX; k++)
			push_value(PROTO_SNS_STEP_DETECTOR, 1.0f);
	}
	/* A count that did not go out goes again at the next sample. */
	last_steps = lost ? HUB_NONE : s;
}

/* Each new reading goes out as it comes, with the quality of its
 * intervals as the accuracy; with none for NO_CONTACT_MS, one event
 * says there is no contact. */
static void sample_heart(uint32_t now)
{
	if (!kept.subs[HEART].on)
		return;
	uint8_t quality;
	unsigned readings;
	uint32_t bpm = afe4900_rate(&quality, &readings);
	uint32_t w[4] = { 0 };
	bool fresh = readings != seen_readings && bpm && bpm != HUB_NONE;
	seen_readings = readings;
	if (fresh) {
		w[0] = proto_float_bits((float)bpm);
		w[1] = quality >= 2 ? PROTO_SNS_HIGH : PROTO_SNS_MEDIUM;
		heart_ms = now;
		no_contact = false;
	} else if (!no_contact && now - heart_ms >= NO_CONTACT_MS) {
		w[1] = (uint32_t)PROTO_SNS_NO_CONTACT;
		no_contact = push(PROTO_SNS_HEART_RATE, mcu_ns(), w, true);
		return;
	} else
		return;
	push(PROTO_SNS_HEART_RATE, mcu_ns(), w, true);
}

void sns_pass(void)
{
	uint32_t now = time_ms();
	settle(now);
	sample_compass(now);
	sample_vectors(now);
	sample_orientation(now);
	sample_light(now);
	sample_steps(now);
	sample_heart(now);
	/* Raises close together go as one. */
	uint32_t t = hub_tilts();
	if (t != seen_tilts) {
		seen_tilts = t;
		tilt_owed = kept.subs[TILT].on;
	}
	if (tilt_owed && push_value(PROTO_SNS_WRIST_TILT, 1.0f))
		tilt_owed = false;
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

/* The state an on-change event that did not go out leaves is sent
 * again. */
static void lost(const struct proto_sns_event *e)
{
	if (e->handle == PROTO_SNS_LIGHT)
		last_lux = -1;
	else if (e->handle == PROTO_SNS_STEP_COUNTER)
		last_steps = HUB_NONE;
	else if (e->handle == PROTO_SNS_HEART_RATE && e->word[1] == (uint32_t)PROTO_SNS_NO_CONTACT)
		no_contact = false;
	else if (e->handle == PROTO_SNS_WRIST_TILT)
		tilt_owed = kept.subs[TILT].on;
}

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
		for (unsigned i = 0; i < taken; i++)
			lost(&queue[i]);
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
		else
			lost(&queue[i]);
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
	uint32_t now = time_ms(), ms = UINT32_MAX;
	for (int i = 0; i < SUBS; i++)
		if (min_ms[i] && kept.subs[i].on && until(due[i], now) < ms)
			ms = until(due[i], now);
	if (compass_on() && until(compass_due, now) < ms)
		ms = until(compass_due, now);
	uint32_t s = kept.subs[COUNTER].on || kept.subs[DETECTOR].on ? until(steps_ms, now) : UINT32_MAX;
	uint32_t r = report_in(now);
	if (s < ms)
		ms = s;
	if (kept.subs[HEART].on) {
		unsigned readings;
		afe4900_rate(NULL, &readings);
		uint32_t h = readings != seen_readings ? 1 : no_contact ? UINT32_MAX : until(heart_ms + NO_CONTACT_MS, now);
		if (h < ms)
			ms = h;
	}
	if (tilt_owed && queued < PROTO_SNS_EVENTS)
		ms = 1;
	if ((r || link_free) && r < ms)
		ms = r;
	if (unsettled() && until(retry_ms + retry_wait, now) < ms)
		ms = until(retry_ms + retry_wait, now);
	return ms;
}

void sns_status(void (*say)(const char *fmt, ...))
{
	static const char *const names[SUBS] = { "accelerometer", "step counter", "step detector", "wrist tilt",
						 "heart rate", "gyroscope", "magnetometer", "light", "orientation" };
	for (int i = 0; i < SUBS; i++)
		if (kept.subs[i].on)
			say("sensors: %s every %lu ms, latency %lu ms\n", names[i], period(i), kept.subs[i].latency_ms);
	say("sensors: %lu commands (%lu bad), %lu flushes, %lu events in %lu reports of up to %u, %lu dropped, "
	    "%u waiting; AP clock %s\n",
	    count.commands, count.bad, count.flushes, count.events, count.reports, per_report, count.dropped, queued,
	    kept.synced ? "known" : "not known");
	ak09918_status(say);
	if (compass_ready)
		say("compass: accuracy %d, %u of %u directions, offset %ld %ld %ld uT at level %ld, radius %ld uT, "
		    "%lu jumps, %lu samples, turns %ld mrad since %lu resets\n",
		    compass.status, compass_bins(&compass), COMPASS_BINS, (long)compass.cal.offset[0],
		    (long)compass.cal.offset[1], (long)compass.cal.offset[2], (long)compass.cal.level,
		    (long)compass.cal.radius, compass.jumps, compass.fed, (long)(compass.turns.angle * 1000),
		    compass.turn_resets);
}

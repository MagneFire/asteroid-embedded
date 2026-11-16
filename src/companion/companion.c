/* The mode, the last one before it and the zone are kept in the store.
 * A mode takes effect once the AP has its 377; the panel comes back
 * TAKE_DELAY_MS after that, and the store is only written while the
 * link has been quiet for a while, as a flash write masks interrupts
 * for longer than the AP's handshakes allow. The RTC keeps local time,
 * so the AP's Unix time is moved by its zone. */
#include <string.h>
#include "asteroid.h"
#include "display/display.h"
#include "input/touch.h"
#include "sensors/hub.h"
#include "storage/storage.h"
#include "power.h"
#include "companion/companion.h"
#include "companion/link.h"
#include "companion/proto.h"
#include "companion/wire.h"

#define KV_MODE		"ap.mode"
#define KV_ZONE		"ap.zone"
#define REPLY_MAX	64
/* The AP lets go of the panel only after it sent the 375. */
#define TAKE_DELAY_MS	1000
#define STORE_QUIET_MS	3000
/* From a 373 until the AP's 384 at the latest. */
#define BURST_MS	10000
/* The power key held this long while the AP has the panel takes it
 * back, for an AP that is not there to give it back. */
#define ESCAPE_MS	5000

static bool ready, boot_lent, ap_has_panel, want_ap, wake_on_take, switching, save_mode, save_zone;
static bool reply_pending, burst, key_held;
static unsigned mode = PROTO_MODE_POWER_OFF, last_mode = PROTO_MODE_POWER_OFF, reboot_reason, reply_mode;
static int32_t zone_min;
static uint32_t take_t, burst_t, key_t;
static uint8_t reply_buf[REPLY_MAX];
static unsigned reply_len;
static int battery = -1;
static struct {
	uint32_t messages, work_modes, battery, boot, other, sensors, unknown, bad, replies_lost, replies_failed, escapes;
} count;

static const char *const mode_names[] = { "normal", "wristband", "power off" };

void companion_boot(void)
{
	if (!board->ap_link.up_int)
		return;
	uint8_t m;
	if (kv_get(KV_MODE, &m, 1) == 1 && m <= PROTO_MODE_POWER_OFF)
		mode = last_mode = m;
	int32_t z;
	if (kv_get(KV_ZONE, &z, sizeof z) == (int)sizeof z)
		zone_min = z;
	uint32_t st = reset_status();
	reboot_reason = proto_reboot_reason(st, log_previous_record() != NULL);
	if (mode == PROTO_MODE_NORMAL && !proto_powered_up(st)) {
		boot_lent = true;
		display_start_lent();
		touch_start_lent();
		haptics_hold(true);
	}
}

void companion_init(void)
{
	if (!board->ap_link.up_int)
		return;
	link_init();
	ready = true;
	if (boot_lent) {
		ap_has_panel = want_ap = true;
		power_lend_display(true);
		log_printf("companion: normal mode before the reset, the AP keeps the panel\n");
	}
}

static void set_time(const struct proto_time *t)
{
	if (!t->time)
		return;
	if (t->zone_min != zone_min) {
		zone_min = t->zone_min;
		save_zone = true;
	}
	rtc_write((uint32_t)(t->time + zone_min * 60));
	hub_set_time();
}

static bool reply(uint16_t chan, unsigned now_mode, unsigned before)
{
	struct proto_reply r = { now_mode, before, reboot_reason, { 0, zone_min } };
	if (rtc_valid())
		r.time.time = (int32_t)(rtc_read() - (uint32_t)(zone_min * 60));
	uint8_t body[REPLY_MAX - 8];
	int n = proto_reply_put(body, sizeof body, &r);
	if (reply_len)
		count.replies_lost++;
	reply_len = n >= 0 ? wire_wrap(reply_buf, sizeof reply_buf, wire_reply_chan(chan), PROTO_REPLY, body, (unsigned)n) : 0;
	return reply_len != 0;
}

/* A malformed message gets no 377, so the AP's retry and reset take
 * over. */
static void work_mode(const struct wire_msg *m, unsigned to)
{
	unsigned field;
	struct proto_time t;
	count.work_modes++;
	if (!proto_work_mode(m->body, m->len, &field, &t)) {
		count.bad++;
		log_printf("companion: malformed %s mode message from the AP\n", mode_names[to]);
		return;
	}
	if (to == PROTO_MODE_NORMAL) {
		burst = true;
		burst_t = time_ms();
	}
	set_time(&t);
	if (!reply(m->chan, to, mode))
		return;
	reply_pending = true;
	reply_mode = to;
	log_printf("companion: %s mode from the AP (field %u), was %s\n", mode_names[to], field, mode_names[mode]);
}

/* Takes effect once the AP has the 377. */
static void enter(unsigned to)
{
	if (to != mode) {
		last_mode = mode;
		mode = to;
		save_mode = true;
	}
	want_ap = to == PROTO_MODE_NORMAL;
	wake_on_take = to == PROTO_MODE_WRISTBAND;
	switching = want_ap != ap_has_panel;
	take_t = time_ms();
}

/* Where the sensor channels will be served; their layouts are not
 * known yet. */
static void sensor_data(const struct wire_msg *m)
{
	count.sensors++;
}

static void message(const uint8_t *p, unsigned n)
{
	struct wire_msg m;
	count.messages++;
	if (!wire_unwrap(p, n, &m)) {
		count.bad++;
		log_printf("companion: %u bytes from the AP without an envelope\n", n);
		return;
	}
	if (!m.framework) {
		sensor_data(&m);
		return;
	}
	int to = proto_mode_of(m.id);
	struct proto_time t;
	if (to >= 0)
		work_mode(&m, (unsigned)to);
	else if (m.id == PROTO_BATTERY) {
		count.battery++;
		battery = proto_battery(m.body, m.len);
		if (battery >= 0)
			battery_from_ap((unsigned)battery);
		else
			count.bad++;
	} else if (m.id == PROTO_BOOT) {
		count.boot++;
		if (proto_boot(m.body, m.len, &t))
			set_time(&t);
		else
			count.bad++;
	} else if (m.id == PROTO_OOBE || m.id == PROTO_BOOT_ACK) {
		count.other++;
		if (m.id == PROTO_OOBE)
			burst = false;
	} else {
		count.unknown++;
		log_printf("companion: message %u on channel %u from the AP, %u bytes, not known\n", m.id, m.chan, m.len);
	}
}

/* Taken back for a 374, the panel stays dark: the AP is still on its
 * way down and turns it off. */
static void switch_panel(void)
{
	switching = false;
	ap_has_panel = want_ap;
	if (want_ap) {
		haptics_hold(true);
		display_lend(true);
		touch_lend(true);
		power_lend_display(true);
	} else {
		power_lend_display(false);
		display_lend(false);
		touch_lend(false);
		haptics_hold(false);
		if (wake_on_take)
			power_wake("companion");
		else
			power_dark_until_wake();
	}
	log_printf("companion: the %s has the panel\n", want_ap ? "AP" : "watch");
}

/* Power off is what a watch that never heard from the AP starts in, so
 * the next reset starts on its own too. Written at once: the release
 * of a key held this long reboots. */
static void escape(uint32_t now)
{
	count.escapes++;
	key_held = false;
	log_printf("companion: power key held, the watch takes the panel back\n");
	if (mode != PROTO_MODE_POWER_OFF) {
		last_mode = mode;
		mode = PROTO_MODE_POWER_OFF;
		uint8_t m = (uint8_t)mode;
		kv_set(KV_MODE, &m, 1);
		save_mode = false;
	}
	want_ap = false;
	wake_on_take = switching = true;
	take_t = now - TAKE_DELAY_MS;
}

static void watch_key(uint32_t now)
{
	if (!ap_has_panel || switching || !power_key())
		key_held = false;
	else if (!key_held) {
		key_held = true;
		key_t = now;
	} else if (now - key_t >= ESCAPE_MS)
		escape(now);
}

static bool link_idle(void) { return !reply_len && link_sent() != LINK_PENDING; }

static uint32_t store_wait_ms(uint32_t now)
{
	uint32_t wait = 0, quiet = link_quiet_ms();
	if (burst && now - burst_t < BURST_MS)
		wait = ms_left(burst_t, BURST_MS, now);
	if (quiet < STORE_QUIET_MS && STORE_QUIET_MS - quiet > wait)
		wait = STORE_QUIET_MS - quiet;
	return wait;
}

bool companion_store_ok(void) { return !ready || store_wait_ms(time_ms()) == 0; }

bool companion_resting(void) { return !link_busy(); }

void companion_poll(void)
{
	if (ready)
		link_poll();
}

void companion_pass(void)
{
	if (!ready)
		return;
	uint32_t now = time_ms();
	link_poll();
	uint8_t buf[WIRE_PAYLOAD_MAX];
	unsigned n;
	while ((n = link_recv(buf, sizeof buf)))
		message(buf, n);
	if (reply_len && link_send(reply_buf, reply_len)) {
		reply_len = 0;
		link_poll();
	}
	watch_key(now);
	if (!link_idle())
		return;
	if (reply_pending) {
		reply_pending = false;
		if (link_sent() == LINK_SENT)
			enter(reply_mode);
		else {
			count.replies_failed++;
			log_printf("companion: the AP did not take the 377, staying in %s mode\n", mode_names[mode]);
		}
	}
	if (switching && (want_ap || now - take_t >= TAKE_DELAY_MS))
		switch_panel();
	if ((save_mode || save_zone) && store_wait_ms(now) == 0) {
		if (save_mode) {
			uint8_t m = (uint8_t)mode;
			save_mode = false;
			kv_set(KV_MODE, &m, 1);
		}
		if (save_zone) {
			save_zone = false;
			kv_set(KV_ZONE, &zone_min, sizeof zone_min);
		}
	}
}

static uint32_t sooner(uint32_t a, uint32_t b) { return a < b ? a : b; }

uint32_t companion_next_ms(void)
{
	if (!ready)
		return UINT32_MAX;
	uint32_t now = time_ms(), next = link_next_ms();
	if (reply_len && link_sent() != LINK_PENDING)
		return 0;
	if (link_idle()) {
		if (reply_pending || (switching && want_ap))
			return 0;
		if (switching)
			next = sooner(next, ms_left(take_t, TAKE_DELAY_MS, now));
		if (save_mode || save_zone)
			next = sooner(next, store_wait_ms(now));
	}
	if (key_held)
		next = sooner(next, ms_left(key_t, ESCAPE_MS, now));
	return next;
}

void companion_status(void (*say)(const char *fmt, ...))
{
	if (!ready) {
		say("companion: no link on this watch\n");
		return;
	}
	say("companion: %s mode, %s before, the %s has the panel%s, zone %ld min, reboot reason %u\n", mode_names[mode],
	    mode_names[last_mode], ap_has_panel ? "AP" : "watch", boot_lent ? " (kept at the start)" : "", (long)zone_min,
	    reboot_reason);
	say("companion: %lu messages: %lu work mode, %lu battery (%d %%), %lu boot, %lu other, %lu sensor, %lu unknown, "
	    "%lu bad; %lu replies replaced, %lu not taken; %lu taken back with the key\n",
	    count.messages, count.work_modes, count.battery, battery, count.boot, count.other, count.sensors,
	    count.unknown, count.bad, count.replies_lost, count.replies_failed, count.escapes);
	link_status(say);
}

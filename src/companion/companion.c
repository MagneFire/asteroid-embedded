/* The mode, the last one before it and the zone are kept in the store.
 * The normal mode takes effect once the AP has its 377. The AP lets go
 * of the panel right after it sent a 375 or 374, without waiting for
 * an answer: those take effect at once, and only the 374 gets a 377,
 * as with the vendor's firmware. The panel comes back TAKE_DELAY_MS
 * after a mode change, and its picture fades in over FADE_IN_MS, or
 * over the store's ap.fade: milliseconds as decimal text, for trying
 * times on the watch. The store is only written while the link has
 * been quiet for a while, as a flash write masks interrupts for longer
 * than the AP's handshakes allow. The RTC keeps local time, so the
 * AP's Unix time is moved by its zone. The AP's clock can be far off,
 * so its time is only taken while the AP set the clock last or nothing
 * has; its zone is always taken. */
#include <string.h>
#include "asteroid.h"
#include "display/display.h"
#include "input/touch.h"
#include "sensors/hub.h"
#include "storage/storage.h"
#include "power.h"
#include "actions.h"
#include "ui/ui.h"
#include "companion/companion.h"
#include "companion/link.h"
#include "companion/proto.h"
#include "companion/sensors.h"
#include "companion/wire.h"

#define KV_MODE		"ap.mode"
#define KEPT_MAGIC	0x4d4f4431u	/* "MOD1" */
#define KV_ZONE		"ap.zone"
#define KV_FADE		"ap.fade"
/* The power off mode as a 374 leaves it, stored apart from the one a
 * watch on its own runs in. */
#define STORED_OFF	3
#define REPLY_MAX	64
/* The AP lets go of the panel only after it sent the 375. */
#define TAKE_DELAY_MS	1000
#define FADE_IN_MS	400
#define FADE_MAX_MS	2000
#define STORE_QUIET_MS	3000
/* From a 373 until the AP's 384 at the latest. */
#define BURST_MS	10000
/* The power key held this long while the AP has the panel takes it
 * back, for an AP that is not there to give it back. */
#define ESCAPE_MS	5000
/* The boot loader resets the MCU about 0.2 s after its ask. */
#define LK_RESET_MS	1000

static bool ready, boot_lent, pin_reset, chips_on, ap_has_panel, want_ap, switching, save_mode, save_zone;
/* Powered off by the AP: nobody has the panel and the watch stays dark
 * until the AP comes back. */
static bool off;
static bool replying, reporting, burst, key_held, lk_wait, ap_off_shown;
static unsigned mode = PROTO_MODE_POWER_OFF, last_mode = PROTO_MODE_POWER_OFF, reboot_reason;
/* The mode a 377 queued or under way makes take effect once the AP has
 * it, or -1. */
static int reply_to = -1, sending_to = -1;
static int32_t zone_min;
static uint32_t take_t, burst_t, key_t, lk_t;
static uint8_t reply_buf[REPLY_MAX], report_buf[LINK_SEND_MAX];
static unsigned reply_len;
static int battery = -1;

#ifdef HOST_TEST
#define KEPT
#else
#define KEPT		__attribute__((section(".kept")))
#endif

/* The mode before it is in the store, for a reset in between. */
static struct {
	uint32_t magic;
	uint8_t mode, last;
	uint16_t check;
} kept KEPT;

static uint16_t kept_check(void) { return (uint16_t)~(kept.mode | kept.last << 8); }

static uint8_t stored_mode(void) { return off ? STORED_OFF : (uint8_t)mode; }

static void take_stored(uint8_t m)
{
	off = m == STORED_OFF;
	mode = off ? PROTO_MODE_POWER_OFF : m;
}

static void keep_mode(void)
{
	kept.mode = stored_mode();
	kept.last = (uint8_t)last_mode;
	kept.check = kept_check();
	kept.magic = KEPT_MAGIC;
}
static struct {
	uint32_t messages, work_modes, battery, boot, other, sensors, batch, unknown, bad, replies_lost, replies_failed,
		escapes, lk_lends;
} count;

static const char *const mode_names[] = { "normal", "wristband", "power off" };

/* Stock's lines around a handover besides the panel's and the touch
 * controller's: own_gpio low takes touch and haptics off our side, and
 * release_gpio is let go of, or held high after the AP's reset as
 * stock does. Taken back, both go high first, so the chips are there
 * when their drivers start again. */
static void lend_lines(bool to_ap, bool hold_release)
{
	const uint8_t own = board->ap_link.own_gpio, rel = board->ap_link.release_gpio;
	if (to_ap) {
		if (own) {
			gpio_set(own, 0);
			gpio_config(own, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
		}
		if (rel && hold_release) {
			gpio_set(rel, 1);
			gpio_config(rel, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
		} else if (rel)
			gpio_config(rel, 0, GPIO_INPUT, GPIO_PULL_NONE, 2);
		return;
	}
	for (unsigned i = 0; i < 2; i++) {
		unsigned pad = i ? rel : own;
		if (pad) {
			gpio_set(pad, 1);
			gpio_config(pad, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
		}
	}
	delay_ms(5);
}

bool companion_boot(void)
{
	if (!board->ap_link.up_int)
		return false;
	uint8_t m;
	if (kv_get(KV_MODE, &m, 1) == 1 && m <= STORED_OFF) {
		take_stored(m);
		last_mode = mode;
	}
	int32_t z;
	if (kv_get(KV_ZONE, &z, sizeof z) == (int)sizeof z)
		zone_min = z;
	uint32_t st = reset_status();
	reboot_reason = proto_reboot_reason(st, log_previous_record() != NULL);
	pin_reset = proto_pin_reset(st);
	if (pin_reset) {
		save_mode = mode != PROTO_MODE_NORMAL || off;
		last_mode = mode;
		mode = PROTO_MODE_NORMAL;
		off = false;
	} else if (!proto_powered_up(st) && kept.magic == KEPT_MAGIC && kept.check == kept_check() &&
		   kept.mode <= STORED_OFF && kept.last <= PROTO_MODE_POWER_OFF) {
		save_mode = kept.mode != stored_mode();
		take_stored(kept.mode);
		last_mode = kept.last;
	}
	keep_mode();
	boot_lent = off || (mode == PROTO_MODE_NORMAL && (pin_reset || !proto_powered_up(st)));
	if (boot_lent) {
		chips_on = true;
		display_start_lent();
		touch_start_lent();
		haptics_hold(true);
		lend_lines(true, pin_reset);
	}
	return boot_lent;
}

void companion_init(void)
{
	if (!board->ap_link.up_int)
		return;
	link_init();
	sns_start(boot_lent && !off);
	ready = true;
	if (boot_lent) {
		ap_has_panel = want_ap = true;
		power_lend_display(true);
		action_bluetooth_lend(true, time_ms());
		if (off) {
			hub_power_down(true);
			log_printf("companion: powered off by the AP, the watch stays dark\n");
		} else
			log_printf(pin_reset ? "companion: reset by the AP, it keeps the panel\n"
					     : "companion: normal mode before the reset, the AP keeps the panel\n");
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
	if (rtc_valid() && !rtc_from_ap()) {
		log_printf("companion: time %ld from the AP left alone, zone %ld min\n", (long)t->time, (long)zone_min);
		return;
	}
	rtc_write_from_ap((uint32_t)(t->time + zone_min * 60));
	hub_set_time();
	log_printf("companion: time %ld from the AP, zone %ld min\n", (long)t->time, (long)zone_min);
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
	reply_to = -1;
	reply_len = n >= 0 ? wire_wrap(reply_buf, sizeof reply_buf, wire_reply_chan(chan), PROTO_REPLY, body, (unsigned)n) : 0;
	return reply_len != 0;
}

static uint32_t stored_ms(const char *key, uint32_t unset, uint32_t min, uint32_t max)
{
	char text[8];
	int n = kv_get(key, text, sizeof text);
	if (n <= 0 || n > (int)sizeof text)
		return unset;
	uint32_t ms = 0;
	for (int i = 0; i < n; i++) {
		if (text[i] < '0' || text[i] > '9')
			return unset;
		ms = ms * 10 + (uint32_t)(text[i] - '0');
	}
	return ms < min ? min : ms > max ? max : ms;
}

static void enter(unsigned to, uint32_t now);

/* A malformed message gets no 377, so the AP's retry and reset take
 * over. */
static void work_mode(const struct wire_msg *m, unsigned to, uint32_t now)
{
	unsigned field;
	struct proto_time t;
	count.work_modes++;
	if (!proto_work_mode(m->body, m->len, &field, &t)) {
		count.bad++;
		log_printf("companion: malformed %s mode message from the AP\n", mode_names[to]);
		return;
	}
	log_printf("companion: %s mode from the AP (field %u), was %s\n", mode_names[to], field, mode_names[mode]);
	char what[48];
	snprintf(what, sizeof what, "%s mode from the AP, was %s", mode_names[to], mode_names[mode]);
	events_note(what);
	set_time(&t);
	if (to == PROTO_MODE_NORMAL) {
		burst = true;
		burst_t = now;
		if (reply(m->chan, to, mode))
			reply_to = (int)to;
		return;
	}
	sending_to = -1;
	if (to == PROTO_MODE_POWER_OFF)
		reply(m->chan, to, mode);
	else {
		reply_len = 0;
		reply_to = -1;
	}
	enter(to, now);
}

/* Only a 374 enters the power off mode here. */
static void enter(unsigned to, uint32_t now)
{
	bool was_off = off;
	off = to == PROTO_MODE_POWER_OFF;
	lk_wait = false;
	if (to != mode || off != was_off) {
		if (to != mode)
			last_mode = mode;
		mode = to;
		save_mode = true;
		keep_mode();
	}
	if (off != was_off)
		hub_power_down(off);
	if (off)
		sns_reset();
	want_ap = to == PROTO_MODE_NORMAL || off;
	switching = want_ap != ap_has_panel;
	take_t = now;
}

/* Channel 5's layout is not known. */
static void raw_data(const struct wire_msg *m)
{
	if (m->chan == WIRE_CHAN_SENSORS) {
		count.sensors++;
		if (!sns_command(m->body, m->len))
			count.bad++;
	} else if (m->chan == WIRE_CHAN_BATCH)
		count.batch++;
	else
		count.other++;
}

static void message(const uint8_t *p, unsigned n, uint32_t now)
{
	struct wire_msg m;
	count.messages++;
	if (!wire_unwrap(p, n, &m)) {
		count.bad++;
		log_printf("companion: %u bytes from the AP without an envelope\n", n);
		return;
	}
	if (!m.framework) {
		raw_data(&m);
		return;
	}
	int to = proto_mode_of(m.id);
	struct proto_time t;
	if (to >= 0)
		work_mode(&m, (unsigned)to, now);
	else if (m.id == PROTO_MODE_REQUEST) {
		count.other++;
		if (reply_to < 0)
			reply(m.chan, mode, last_mode);
	} else if (m.id == PROTO_BATTERY) {
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

static void switch_panel(uint32_t now)
{
	switching = false;
	ap_has_panel = want_ap;
	action_bluetooth_lend(want_ap, now);
	if (want_ap) {
		sns_lend(true);
		haptics_hold(true);
		display_lend(true);
		touch_lend(true);
		lend_lines(true, false);
		power_lend_display(true);
	} else {
		lend_lines(false, false);
		if (chips_on) {
			chips_on = false;
			if (board_chips_off())
				iom_stop(board->chips_off.i2c.bus);
			sns_chips_off();
		}
		power_lend_display(false);
		display_lend(false);
		touch_lend(false);
		haptics_hold(false);
		sns_lend(false);
		power_fade_in(stored_ms(KV_FADE, FADE_IN_MS, 0, FADE_MAX_MS));
		power_wake("companion");
	}
	log_printf(off ? "companion: powered off, nobody has the panel\n" : "companion: the %s has the panel\n",
		   want_ap ? "AP" : "watch");
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
		keep_mode();
		uint8_t m = (uint8_t)mode;
		kv_set(KV_MODE, &m, 1);
		save_mode = false;
	}
	sns_reset();
	want_ap = false;
	switching = true;
	take_t = now - TAKE_DELAY_MS;
}

/* The AP is booting: the panel goes now, the mode once it is clear that
 * no reset follows. */
static void lk_asked(uint32_t now)
{
	log_printf("companion: the AP's boot loader asks, the AP gets the panel\n");
	want_ap = true;
	switching = !ap_has_panel;
	lk_wait = true;
	lk_t = now;
}

static void watch_key(uint32_t now)
{
	if (off || !ap_has_panel || switching || !power_key())
		key_held = false;
	else if (!key_held) {
		key_held = true;
		key_t = now;
	} else if (now - key_t >= ESCAPE_MS)
		escape(now);
}

static bool link_idle(void) { return !reply_len && !replying && !reporting && link_sent() != LINK_PENDING; }

/* up_int stays low while the AP may be off, or the boot loader stops
 * before it asks. */
static bool report_ok(void) { return link_idle() && mode == PROTO_MODE_NORMAL; }

static uint32_t store_wait_ms(uint32_t now)
{
	uint32_t wait = 0, quiet = link_quiet_ms();
	if (burst && now - burst_t < BURST_MS)
		wait = ms_left(burst_t, BURST_MS, now);
	if (quiet < STORE_QUIET_MS && STORE_QUIET_MS - quiet > wait)
		wait = STORE_QUIET_MS - quiet;
	return wait;
}

uint32_t companion_store_wait_ms(void) { return ready ? store_wait_ms(time_ms()) : 0; }

bool companion_resting(void) { return !link_busy(); }

void companion_poll(void)
{
	if (ready)
		link_poll();
}

/* What became of the 377 or report under way, before another one is
 * queued. */
static void replied(uint32_t now)
{
	if (reporting && link_sent() != LINK_PENDING) {
		reporting = false;
		if (link_sent() == LINK_SENT)
			sns_sent();
		else
			sns_unsent();
	}
	if (!replying || link_sent() == LINK_PENDING)
		return;
	replying = false;
	if (sending_to < 0)
		return;
	if (link_sent() == LINK_SENT)
		enter((unsigned)sending_to, now);
	else {
		count.replies_failed++;
		log_printf("companion: the AP did not take the 377, staying in %s mode\n", mode_names[mode]);
	}
	sending_to = -1;
}

static void settle(uint32_t now)
{
	if (switching && (want_ap || now - take_t >= TAKE_DELAY_MS))
		switch_panel(now);
	if (lk_wait && now - lk_t >= LK_RESET_MS) {
		count.lk_lends++;
		log_printf("companion: no reset after the boot loader's ask, normal mode\n");
		events_note("no reset after the boot loader's ask, was wristband");
		enter(PROTO_MODE_NORMAL, now);
	}
	if ((save_mode || save_zone) && store_wait_ms(now) == 0) {
		if (save_mode) {
			uint8_t m = stored_mode();
			save_mode = false;
			kv_set(KV_MODE, &m, 1);
		}
		if (save_zone) {
			save_zone = false;
			kv_set(KV_ZONE, &zone_min, sizeof zone_min);
		}
	}
}

void companion_pass(void)
{
	if (!ready)
		return;
	uint32_t now = time_ms();
	link_answer_lk(!ap_has_panel);
	link_poll();
	if (link_lk_asked())
		lk_asked(now);
	uint8_t buf[WIRE_PAYLOAD_MAX];
	unsigned n;
	while ((n = link_recv(buf, sizeof buf)))
		message(buf, n, now);
	replied(now);
	if (reply_len && !replying && link_send(reply_buf, reply_len)) {
		replying = true;
		sending_to = reply_to;
		reply_len = 0;
		reply_to = -1;
		link_poll();
	}
	watch_key(now);
	/* A report is queued after these, so that reports do not keep
	 * them waiting. */
	settle(now);
	bool ap_off = mode == PROTO_MODE_WRISTBAND && !ap_has_panel;
	if (ap_off != ap_off_shown)
		ui_set_ap_off(ap_off_shown = ap_off);
	sns_pass();
	if (report_ok() && sns_due() && (n = sns_report(report_buf, sizeof report_buf)) && link_send(report_buf, n)) {
		reporting = true;
		link_poll();
	}
}

static uint32_t sooner(uint32_t a, uint32_t b) { return a < b ? a : b; }

uint32_t companion_next_ms(void)
{
	if (!ready)
		return UINT32_MAX;
	uint32_t now = time_ms(), next = link_next_ms();
	if ((replying || reporting || reply_len) && link_sent() != LINK_PENDING)
		return 0;
	if (switching && want_ap)
		return 0;
	if (switching)
		next = sooner(next, ms_left(take_t, TAKE_DELAY_MS, now));
	if (save_mode || save_zone)
		next = sooner(next, store_wait_ms(now));
	if (key_held)
		next = sooner(next, ms_left(key_t, ESCAPE_MS, now));
	if (lk_wait)
		next = sooner(next, ms_left(lk_t, LK_RESET_MS, now));
	return sooner(next, sns_next_ms(report_ok()));
}

void companion_status(void (*say)(const char *fmt, ...))
{
	if (!ready) {
		say("companion: no link on this watch\n");
		return;
	}
	say("companion: %s mode, %s before, %s has the panel%s, zone %ld min, reboot reason %u\n", mode_names[mode],
	    mode_names[last_mode], off ? "nobody" : ap_has_panel ? "the AP" : "the watch",
	    boot_lent ? " (kept at the start)" : "", (long)zone_min, reboot_reason);
	say("companion: %lu messages: %lu work mode, %lu battery (%d %%), %lu boot, %lu other, %lu sensor, %lu batch, "
	    "%lu unknown, %lu bad; %lu replies replaced, %lu not taken; %lu taken back with the key; %lu lent after "
	    "the boot loader without a reset\n",
	    count.messages, count.work_modes, count.battery, battery, count.boot, count.other, count.sensors,
	    count.batch, count.unknown, count.bad, count.replies_lost, count.replies_failed, count.escapes,
	    count.lk_lends);
	sns_status(say);
	link_status(say);
}

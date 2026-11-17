/* The link to the application processor, as the vendor's firmware runs
 * it: one transfer at a time, a write from the AP before a packet
 * waiting for it. A packet from the AP is checked as its last part
 * arrives, in the interrupt, so dn_ack drops well within the AP's 30 ms
 * however long the main loop's pass is. */
#include <string.h>
#include "asteroid.h"
#include "sched.h"
#include "companion/link.h"
#include "companion/wire.h"

#define UP_INT		(board->ap_link.up_int)
#define UP_ACK		(board->ap_link.up_ack)
#define DN_INT		(board->ap_link.dn_int)
#define DN_ACK		(board->ap_link.dn_ack)
/* The AP sleeps 100 ms after it reports an error. */
#define RETRY_MS	150

enum state { IDLE, RX, RX_HOLD, TX, TX_ACK };
enum tx_end { TX_OK, TX_CHECK, TX_TIMEOUT, TX_STUCK };

static volatile enum state state;
static volatile bool rx_ready, dn_waiting, tx_queued, deaf;
static volatile uint32_t state_ms, ack_us, retry_ms, active_ms, dn_ms;
static volatile unsigned unfinished_run;
static volatile enum link_result tx_result = LINK_SENT;
static volatile uint32_t problems;
static struct wire_rx rx;
static struct wire_seq seq_in = { 0, true };
static uint8_t tx_frame[WIRE_FRAME_MAX];
static unsigned tx_len, tx_part, tx_parts, tx_tries;
static uint16_t seq_out;
static bool resync_out = true, ready, task_added;

static volatile struct {
	uint32_t rx, rx_errors[WIRE_BAD_SEQ + 1], rx_timeouts, tx, tx_check, tx_timeouts, tx_stuck, writes, reads, deaf, strobes;
} count;

static const char *const rx_why[] = { "", "header", "length", "checksum", "sequence" };

enum { PROBLEM_RX = 1, PROBLEM_RX_TIMEOUT = 1 << 5, PROBLEM_TX_CHECK = 1 << 6, PROBLEM_TX_TIMEOUT = 1 << 7,
       PROBLEM_TX_STUCK = 1 << 8, PROBLEM_TX_FAILED = 1 << 9, PROBLEM_DEAF = 1 << 10 };

/* Runs the main loop's pass again: an interrupt that leaves it work
 * would otherwise wait for a wake planned from the old state. */
static bool task_run(uint32_t now)
{
	link_poll();
	return false;
}
static struct sched_task task = { .name = "aplink", .run = task_run };

static void wake(void)
{
	active_ms = time_ms();
	if (task_added)
		sched_post(&task);
}

static void stage(unsigned part)
{
	unsigned off, direct, fifo;
	wire_read_slice(WIRE_HEADER + tx_len, part, &off, &direct, &fifo);
	ios_stage(tx_frame + off, direct, tx_frame + off + direct, fifo);
}

static void rx_begin(void)
{
	wire_rx_start(&rx);
	state = RX;
	state_ms = time_ms();
	gpio_set(DN_ACK, 1);
}

static void rx_part(void)
{
	uint8_t part[WIRE_WRITE_PART];
	ios_read(0, part, sizeof part);
	enum wire_error err = WIRE_OK;
	enum wire_rx_state s = wire_rx_part(&rx, part, &err);
	if (s == WIRE_RX_MORE)
		return;
	unfinished_run = 0;
	wake();
	if (s == WIRE_RX_DONE && !wire_seq_take(&seq_in, &rx.h)) {
		err = WIRE_BAD_SEQ;
		s = WIRE_RX_BAD;
	}
	if (s == WIRE_RX_BAD) {
		count.rx_errors[err]++;
		problems |= PROBLEM_RX << err;
		state = RX_HOLD;
		state_ms = time_ms();
		return;
	}
	count.rx++;
	gpio_set(DN_ACK, 0);
	rx_ready = true;
	state = IDLE;
}

static void up_ack_irq(unsigned pin);

static void tx_end(enum tx_end how)
{
	gpio_set(UP_INT, 0);
	gpio_irq_config(UP_ACK, GPIO_IRQ_EDGE_RISING, up_ack_irq);
	state = IDLE;
	wake();
	if (how == TX_OK) {
		count.tx++;
		seq_out++;
		resync_out = false;
		tx_queued = false;
		tx_result = LINK_SENT;
		return;
	}
	if (how == TX_CHECK) {
		count.tx_check++;
		problems |= PROBLEM_TX_CHECK;
	} else if (how == TX_TIMEOUT) {
		count.tx_timeouts++;
		problems |= PROBLEM_TX_TIMEOUT;
	} else {
		count.tx_stuck++;
		problems |= PROBLEM_TX_STUCK;
	}
	retry_ms = time_ms();
	if (++tx_tries < LINK_TX_TRIES)
		return;
	problems |= PROBLEM_TX_FAILED;
	tx_queued = false;
	tx_result = LINK_FAILED;
}

static void tx_begin(void)
{
	wire_header_put(tx_frame, seq_out, resync_out ? WIRE_FLAG_RESYNC : 0, tx_frame + WIRE_HEADER, tx_len);
	tx_parts = wire_read_parts(tx_len);
	tx_part = 0;
	stage(0);
	state = TX;
	state_ms = time_ms();
	gpio_set(UP_INT, 1);
}

/* The AP's answer is a pulse: its length says how the packet arrived. */
static void up_ack_irq(unsigned pin)
{
	if (state == TX) {
		ack_us = time_us();
		state = TX_ACK;
		state_ms = time_ms();
		gpio_irq_config(UP_ACK, GPIO_IRQ_EDGE_FALLING, up_ack_irq);
		wake();
		if (gpio_get(UP_ACK))
			return;
	} else if (state != TX_ACK)
		return;
	tx_end(time_us() - ack_us < LINK_ACK_ERROR_US ? TX_OK : TX_CHECK);
}

static void dn_rose(void)
{
	dn_waiting = true;
	dn_ms = time_ms();
}

static void dn_int_irq(unsigned pin)
{
	if (deaf)
		return;
	dn_rose();
	wake();
}

static void hear(bool on)
{
	deaf = !on;
	gpio_irq_enable(DN_INT, on);
	if (on && gpio_get(DN_INT))
		dn_rose();
}

static void ios_event(uint32_t st)
{
	if (st & IOS_WRITTEN) {
		count.writes++;
		if (deaf) {
			unfinished_run = 0;
			hear(true);
			wake();
		}
		if (state == RX)
			rx_part();
	}
	if (st & IOS_READ) {
		count.reads++;
		if (state == TX && tx_part + 1 < tx_parts)
			stage(++tx_part);
	}
}

static void line_out(unsigned pin)
{
	gpio_set(pin, 0);
	gpio_config(pin, 0, GPIO_OUTPUT, GPIO_PULL_NONE, 2);
}

void link_init(void)
{
	if (!UP_INT)
		return;
	line_out(UP_INT);
	line_out(DN_ACK);
	gpio_config(DN_INT, 0, GPIO_INPUT, GPIO_PULL_NONE, 2);
	gpio_config(UP_ACK, 0, GPIO_INPUT, GPIO_PULL_NONE, 2);
	task_added = sched_add(&task);
	ios_init(ios_event);
	gpio_irq_config(UP_ACK, GPIO_IRQ_EDGE_RISING, up_ack_irq);
	gpio_irq_enable(UP_ACK, true);
	gpio_irq_config(DN_INT, GPIO_IRQ_EDGE_RISING, dn_int_irq);
	gpio_irq_enable(DN_INT, true);
	ready = true;
	if (gpio_get(DN_INT))
		dn_rose();
	log_printf("link: waiting for the application processor\n");
}

bool link_ready(void) { return ready; }

static void report(void)
{
	uint32_t flags = irq_save();
	uint32_t p = problems;
	problems = 0;
	irq_restore(flags);
	for (unsigned e = WIRE_BAD_HEADER; e <= WIRE_BAD_SEQ; e++)
		if (p & PROBLEM_RX << e)
			log_printf("link: bad packet from the AP (%s), %lu so far\n", rx_why[e], count.rx_errors[e]);
	if (p & PROBLEM_RX_TIMEOUT)
		log_printf("link: the AP did not finish its packet, %lu so far\n", count.rx_timeouts);
	if (p & PROBLEM_TX_CHECK)
		log_printf("link: the AP reports a bad packet, %lu so far\n", count.tx_check);
	if (p & PROBLEM_TX_TIMEOUT)
		log_printf("link: the AP did not read its packet, %lu so far\n", count.tx_timeouts);
	if (p & PROBLEM_TX_STUCK)
		log_printf("link: the AP holds up_ack high, %lu so far\n", count.tx_stuck);
	if (p & PROBLEM_TX_FAILED)
		log_printf("link: gave up on a packet for the AP\n");
	if (p & PROBLEM_DEAF)
		log_printf("link: %u unfinished packets in a row, dn_int ignored until the AP writes\n", LINK_UNFINISHED_MAX);
}

void link_poll(void)
{
	if (!ready)
		return;
	uint32_t now = time_ms(), flags = irq_save();
	uint32_t gone = now - state_ms;
	if (state == RX && gone >= LINK_RX_MS) {
		count.rx_timeouts++;
		problems |= PROBLEM_RX_TIMEOUT;
		gpio_set(DN_ACK, 0);
		state = IDLE;
		active_ms = now;
		if (++unfinished_run >= LINK_UNFINISHED_MAX) {
			count.deaf++;
			problems |= PROBLEM_DEAF;
			hear(false);
			dn_waiting = false;
		}
	} else if (state == RX_HOLD && gone >= LINK_ERROR_MS) {
		gpio_set(DN_ACK, 0);
		state = IDLE;
	} else if (state == TX && gone >= LINK_TX_MS)
		tx_end(TX_TIMEOUT);
	else if (state == TX_ACK && gone >= LINK_ACK_STUCK_MS)
		tx_end(gpio_get(UP_ACK) ? TX_STUCK : TX_CHECK);
	if (state == IDLE && dn_waiting && !rx_ready) {
		if (!gpio_get(DN_INT)) {
			dn_waiting = false;
			count.strobes++;
		} else if (now - dn_ms >= LINK_DN_SETTLE_MS) {
			dn_waiting = false;
			rx_begin();
		}
	}
	if (state == IDLE && !dn_waiting && tx_queued && (!tx_tries || now - retry_ms >= RETRY_MS))
		tx_begin();
	irq_restore(flags);
	if (problems)
		report();
}

unsigned link_recv(uint8_t *buf, unsigned cap)
{
	if (!rx_ready)
		return 0;
	unsigned n = rx.h.len < cap ? rx.h.len : cap;
	memcpy(buf, rx.buf + WIRE_HEADER, n);
	rx_ready = false;
	return n;
}

bool link_send(const uint8_t *p, unsigned n)
{
	if (!ready || tx_queued || n > LINK_SEND_MAX)
		return false;
	memcpy(tx_frame + WIRE_HEADER, p, n);
	tx_len = n;
	tx_tries = 0;
	tx_result = LINK_PENDING;
	tx_queued = true;
	return true;
}

enum link_result link_sent(void) { return tx_result; }

bool link_busy(void) { return ready && state != IDLE; }

uint32_t link_quiet_ms(void)
{
	if (!ready)
		return UINT32_MAX;
	if (state != IDLE || rx_ready || tx_queued || dn_waiting || (!deaf && gpio_get(DN_INT)))
		return 0;
	return time_ms() - active_ms;
}

static uint32_t state_next_ms(uint32_t now)
{
	switch (state) {
	case RX:
		return ms_left(state_ms, LINK_RX_MS, now);
	case RX_HOLD:
		return ms_left(state_ms, LINK_ERROR_MS, now);
	case TX:
		return ms_left(state_ms, LINK_TX_MS, now);
	case TX_ACK:
		return ms_left(state_ms, LINK_ACK_STUCK_MS, now);
	default:
		if (dn_waiting)
			return gpio_get(DN_INT) ? ms_left(dn_ms, LINK_DN_SETTLE_MS, now) : 0;
		if (!tx_queued)
			return UINT32_MAX;
		return tx_tries ? ms_left(retry_ms, RETRY_MS, now) : 0;
	}
}

uint32_t link_next_ms(void)
{
	if (!ready)
		return UINT32_MAX;
	if (rx_ready)
		return 0;
	return state_next_ms(time_ms());
}

void link_status(void (*say)(const char *fmt, ...))
{
	static const char *const names[] = { "idle", "receiving", "holding dn_ack", "sending", "answered" };
	if (!ready) {
		say("link: none\n");
		return;
	}
	say("link: %s, lines up_int %u up_ack %u dn_int %u dn_ack %u, sequence in %u out %u%s\n", names[state],
	    gpio_get(UP_INT), gpio_get(UP_ACK), gpio_get(DN_INT), gpio_get(DN_ACK), seq_in.expect, seq_out,
	    seq_in.resync ? ", taking any" : "");
	say("link: received %lu, bad header %lu length %lu checksum %lu sequence %lu, unfinished %lu\n", count.rx,
	    count.rx_errors[WIRE_BAD_HEADER], count.rx_errors[WIRE_TOO_LONG], count.rx_errors[WIRE_BAD_SUM],
	    count.rx_errors[WIRE_BAD_SEQ], count.rx_timeouts);
	say("link: sent %lu, reported bad %lu, not read %lu, up_ack stuck %lu; %lu writes, %lu reads, %lu interrupts\n",
	    count.tx, count.tx_check, count.tx_timeouts, count.tx_stuck, count.writes, count.reads, ios_interrupts());
	say("link: dn_int ignored %lu times%s, %lu requests dropped before %u ms\n", count.deaf, deaf ? ", now" : "",
	    count.strobes, LINK_DN_SETTLE_MS);
}

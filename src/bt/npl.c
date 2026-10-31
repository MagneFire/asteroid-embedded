/* NimBLE porting layer for the superloop. Everything runs in the main
 * loop's context: event queues are plain lists drained by bt_poll(),
 * callouts are a list of deadlines on time_ms() checked there, mutexes
 * only count and semaphores poll the HCI transport while they wait (the
 * host blocks on one for each command it sends). No NimBLE code runs in
 * an interrupt handler, so critical sections do not mask anything. The
 * host's malloc/free (GATT tables at start) share Mbed TLS's buffer
 * allocator. */
#include <string.h>
#include "asteroid.h"
#include "nimble/nimble_npl.h"
#include "mbedtls/platform.h"
#include "bt.h"

static struct ble_npl_callout *callouts;
static void (*wait_fn)(void);
static unsigned critical;
static char line[160];
static unsigned line_len;

void bt_npl_set_wait(void (*fn)(void))
{
	wait_fn = fn;
}

bool ble_npl_os_started(void)
{
	return true;
}

void *ble_npl_get_current_task_id(void)
{
	return &callouts;
}

void ble_npl_eventq_init(struct ble_npl_eventq *evq)
{
	evq->head = evq->tail = NULL;
}

static struct ble_npl_event *pop(struct ble_npl_eventq *evq)
{
	struct ble_npl_event *ev = evq->head;
	if (!ev)
		return NULL;
	evq->head = ev->next;
	if (!evq->head)
		evq->tail = NULL;
	ev->next = NULL;
	ev->queued = false;
	return ev;
}

/* The host waits here with the controller running the transport in
 * wait_fn. The watchdog is fed for WAIT_FEED_MS; a wait longer than that
 * is a hung controller, and the watchdog then records where. */
#define WAIT_FEED_MS 10000

static void wait_feed(uint32_t t0, const char *what)
{
	uint32_t waited = time_ms() - t0;
	if (waited < WAIT_FEED_MS)
		wdt_feed();
	else if (waited < WAIT_FEED_MS + 100)
		log_printf("bt: %s stuck for %lu ms, letting the watchdog bite\n", what, waited);
}

struct ble_npl_event *ble_npl_eventq_get(struct ble_npl_eventq *evq, ble_npl_time_t tmo)
{
	uint32_t t0 = time_ms();
	struct ble_npl_event *ev;
	while (!(ev = pop(evq)) && tmo && (tmo == BLE_NPL_TIME_FOREVER || time_ms() - t0 < tmo)) {
		if (wait_fn)
			wait_fn();
		bt_npl_expire();
		wait_feed(t0, "eventq_get");
	}
	return ev;
}

void ble_npl_eventq_put(struct ble_npl_eventq *evq, struct ble_npl_event *ev)
{
	if (ev->queued)
		return;
	ev->next = NULL;
	ev->queued = true;
	if (evq->tail)
		evq->tail->next = ev;
	else
		evq->head = ev;
	evq->tail = ev;
}

void ble_npl_eventq_remove(struct ble_npl_eventq *evq, struct ble_npl_event *ev)
{
	if (!ev->queued)
		return;
	struct ble_npl_event **pp = &evq->head, *prev = NULL;
	while (*pp && *pp != ev) {
		prev = *pp;
		pp = &(*pp)->next;
	}
	if (!*pp)
		return;
	*pp = ev->next;
	if (evq->tail == ev)
		evq->tail = prev;
	ev->next = NULL;
	ev->queued = false;
}

bool ble_npl_eventq_is_empty(struct ble_npl_eventq *evq)
{
	return !evq->head;
}

void ble_npl_event_init(struct ble_npl_event *ev, ble_npl_event_fn *fn, void *arg)
{
	memset(ev, 0, sizeof *ev);
	ev->fn = fn;
	ev->arg = arg;
}

bool ble_npl_event_is_queued(struct ble_npl_event *ev)
{
	return ev->queued;
}

void *ble_npl_event_get_arg(struct ble_npl_event *ev)
{
	return ev->arg;
}

void ble_npl_event_set_arg(struct ble_npl_event *ev, void *arg)
{
	ev->arg = arg;
}

void ble_npl_event_run(struct ble_npl_event *ev)
{
	ev->fn(ev);
}

unsigned bt_npl_run(struct ble_npl_eventq *evq, unsigned max)
{
	unsigned n = 0;
	struct ble_npl_event *ev;
	bt_npl_expire();
	while (n < max && (ev = pop(evq))) {
		ble_npl_event_run(ev);
		n++;
	}
	return n;
}

ble_npl_error_t ble_npl_mutex_init(struct ble_npl_mutex *mu)
{
	mu->depth = 0;
	return BLE_NPL_OK;
}

ble_npl_error_t ble_npl_mutex_pend(struct ble_npl_mutex *mu, ble_npl_time_t timeout)
{
	mu->depth++;
	return BLE_NPL_OK;
}

ble_npl_error_t ble_npl_mutex_release(struct ble_npl_mutex *mu)
{
	if (!mu->depth)
		return BLE_NPL_BAD_MUTEX;
	mu->depth--;
	return BLE_NPL_OK;
}

ble_npl_error_t ble_npl_sem_init(struct ble_npl_sem *sem, uint16_t tokens)
{
	sem->tokens = tokens;
	return BLE_NPL_OK;
}

ble_npl_error_t ble_npl_sem_pend(struct ble_npl_sem *sem, ble_npl_time_t timeout)
{
	uint32_t t0 = time_ms();
	while (!sem->tokens) {
		if (timeout != BLE_NPL_TIME_FOREVER && time_ms() - t0 >= timeout)
			return BLE_NPL_TIMEOUT;
		if (wait_fn)
			wait_fn();
		wait_feed(t0, "sem_pend");
	}
	sem->tokens--;
	return BLE_NPL_OK;
}

ble_npl_error_t ble_npl_sem_release(struct ble_npl_sem *sem)
{
	sem->tokens++;
	return BLE_NPL_OK;
}

uint16_t ble_npl_sem_get_count(struct ble_npl_sem *sem)
{
	return sem->tokens;
}

static void unlink_callout(struct ble_npl_callout *co)
{
	for (struct ble_npl_callout **pp = &callouts; *pp; pp = &(*pp)->next)
		if (*pp == co) {
			*pp = co->next;
			break;
		}
	co->next = NULL;
	co->linked = false;
}

void ble_npl_callout_init(struct ble_npl_callout *co, struct ble_npl_eventq *evq, ble_npl_event_fn *ev_cb, void *ev_arg)
{
	if (co->linked)
		unlink_callout(co);
	memset(co, 0, sizeof *co);
	ble_npl_event_init(&co->ev, ev_cb, ev_arg);
	co->evq = evq;
}

ble_npl_error_t ble_npl_callout_reset(struct ble_npl_callout *co, ble_npl_time_t ticks)
{
	if ((int32_t)ticks < 0)
		return BLE_NPL_EINVAL;
	if (co->ev.queued && co->evq)
		ble_npl_eventq_remove(co->evq, &co->ev);
	co->expiry = time_ms() + ticks;
	co->active = true;
	if (!co->linked) {
		co->next = callouts;
		callouts = co;
		co->linked = true;
	}
	return BLE_NPL_OK;
}

void ble_npl_callout_stop(struct ble_npl_callout *co)
{
	co->active = false;
	if (co->linked)
		unlink_callout(co);
	if (co->ev.queued && co->evq)
		ble_npl_eventq_remove(co->evq, &co->ev);
}

bool ble_npl_callout_is_active(struct ble_npl_callout *co)
{
	return co->active;
}

ble_npl_time_t ble_npl_callout_get_ticks(struct ble_npl_callout *co)
{
	return co->expiry;
}

ble_npl_time_t ble_npl_callout_remaining_ticks(struct ble_npl_callout *co, ble_npl_time_t now)
{
	int32_t left = (int32_t)(co->expiry - now);
	return co->active && left > 0 ? (ble_npl_time_t)left : 0;
}

void ble_npl_callout_set_arg(struct ble_npl_callout *co, void *arg)
{
	co->ev.arg = arg;
}

void bt_npl_expire(void)
{
	uint32_t now = time_ms();
	struct ble_npl_callout *co = callouts;
	while (co) {
		struct ble_npl_callout *next = co->next;
		if (co->active && (int32_t)(now - co->expiry) >= 0) {
			co->active = false;
			unlink_callout(co);
			if (co->evq)
				ble_npl_eventq_put(co->evq, &co->ev);
			else
				ble_npl_event_run(&co->ev);
		}
		co = next;
	}
}

uint32_t bt_npl_next_ms(void)
{
	uint32_t now = time_ms(), best = UINT32_MAX;
	for (struct ble_npl_callout *co = callouts; co; co = co->next) {
		if (!co->active)
			continue;
		int32_t left = (int32_t)(co->expiry - now);
		uint32_t ms = left > 0 ? (uint32_t)left : 0;
		if (ms < best)
			best = ms;
	}
	return best;
}

ble_npl_time_t ble_npl_time_get(void)
{
	return time_ms();
}

ble_npl_error_t ble_npl_time_ms_to_ticks(uint32_t ms, ble_npl_time_t *out_ticks)
{
	*out_ticks = ms;
	return BLE_NPL_OK;
}

ble_npl_error_t ble_npl_time_ticks_to_ms(ble_npl_time_t ticks, uint32_t *out_ms)
{
	*out_ms = ticks;
	return BLE_NPL_OK;
}

ble_npl_time_t ble_npl_time_ms_to_ticks32(uint32_t ms)
{
	return ms;
}

uint32_t ble_npl_time_ticks_to_ms32(ble_npl_time_t ticks)
{
	return ticks;
}

void ble_npl_time_delay(ble_npl_time_t ticks)
{
	for (uint32_t t0 = time_ms(); time_ms() - t0 < ticks;)
		wdt_feed();
}

void ble_npl_hw_set_isr(int irqn, void (*addr)(void))
{
}

uint32_t ble_npl_hw_enter_critical(void)
{
	return critical++;
}

void ble_npl_hw_exit_critical(uint32_t ctx)
{
	critical = ctx;
}

bool ble_npl_hw_is_in_critical(void)
{
	return critical > 0;
}

void bt_npl_vlog(const char *fmt, va_list ap)
{
	int n = vsnprintf(line + line_len, sizeof line - line_len, fmt, ap);
	if (n > 0)
		line_len += (unsigned)n;
	if (line_len >= sizeof line)
		line_len = sizeof line - 1;
	if (line_len && line[line_len - 1] != '\n' && line_len < sizeof line - 1)
		return;
	if (line_len && line[line_len - 1] == '\n')
		line[--line_len] = 0;
	log_printf("nimble: %s\n", line);
	line_len = 0;
}

void __assert_func(const char *file, int lineno, const char *func, const char *expr)
{
	char buf[160];
	snprintf(buf, sizeof buf, "assert %s at %s:%d", expr, file, lineno);
	log_printf("%s\n", buf);
	log_record(buf);
	reboot();
}

void bt_crypto_exit(int status)
{
	__assert_func("mbedtls heap", status, NULL, "intact");
}

void *malloc(size_t n)
{
	size_t *p = mbedtls_calloc(1, n + sizeof(size_t) * 2);
	if (!p)
		return NULL;
	p[0] = n;
	return p + 2;
}

void free(void *ptr)
{
	if (ptr)
		mbedtls_free((size_t *)ptr - 2);
}

void *realloc(void *ptr, size_t n)
{
	void *q = malloc(n);
	if (q && ptr) {
		size_t old = ((size_t *)ptr)[-2];
		memcpy(q, ptr, old < n ? old : n);
	}
	if (q || !n)
		free(ptr);
	return q;
}

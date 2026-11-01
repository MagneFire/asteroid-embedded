/* Sensor hub: a PNI EM7184 (SENtral) at 0x28 on BLSP1 QUP2. It has no
 * flash: at every boot it is reset through its reset register and its
 * RAM image, em7184.fw from the system partition, is uploaded in 32-bit
 * words sent big endian (the file holds them little endian), then the
 * CRC-32 the hub computed is compared with the one in the image header
 * and its CPU started. The firmware announces itself with an
 * "initialized" meta event; only after that do sensor settings stick.
 * Settings go through the parameter interface: a page (with the size in
 * its upper nibble when short of the maximum) and a parameter number,
 * bit 7 set for a write, then the hub echoes the number in the
 * acknowledge register once done. A sensor runs while its sample rate
 * is non-zero: the step counter (id 19, its rate field taken as a report
 * interval in ms) sends a 16-bit count on change, which wraps; the wrist
 * tilt gesture (id 26) is counted modulo 4 in the wake source register.
 *
 * Events come out of a FIFO read through the window at 0x00-0x31: an id
 * byte, then a payload whose size the id fixes (timestamps and meta
 * events have ids of their own). The interrupt, gpio 111 on a rising
 * edge (MPM pin 54 while the cluster is off), starts a chain of I2C
 * transfers in interrupt context: the wake source, the bytes left, then
 * the FIFO in pieces that never cross the window, parsed as they come.
 * Until the hub is configured nothing is chained: the main loop polls.
 * Once the watch sleeps the hub is told the AP is suspended, so only
 * wake-up sensors interrupt; the FIFO is drained once a minute while it
 * is (for the always-on face) and the flag cleared when the screen comes
 * on. A health check 2 s after the last read restarts the hub after a
 * fatal error or a changed image CRC. There is no heart rate sensor. */
#include <string.h>
#include "asteroid.h"
#include "hub.h"
#include "power.h"

#define QUP		BOARD_HUB_QUP
#define ADDR		BOARD_HUB_ADDR
#define GPIO_IRQ	BOARD_HUB_IRQ
#define MPM_PIN_IRQ	BOARD_HUB_MPM
#define FIRMWARE	"em7184.fw"
#define BUS_HZ		384000

#define REG_FIFO_FLUSH		0x32
#define REG_CHIP_CONTROL	0x34
#define REG_HOST_STATUS		0x35
#define REG_CHIP_STATUS		0x37
#define REG_FIFO_BYTES		0x38
#define REG_PARAM_ACK		0x3a
#define REG_PARAM_SAVE		0x3b
#define REG_WAKE_SRC		0x4d
#define REG_ERROR		0x50
#define REG_PARAM_PAGE		0x54
#define REG_HOST_CONTROL	0x55
#define REG_PARAM_LOAD		0x5c
#define REG_PARAM_REQ		0x64
#define REG_VERSION		0x70
#define REG_PRODUCT_ID		0x90
#define REG_UPLOAD_DATA		0x96
#define REG_CRC			0x97
#define REG_RESET_REQ		0x9b

#define FIFO_WINDOW		50
#define CHIP_CPU_RUN		BIT(0)
#define CHIP_UPLOAD		BIT(1)
#define STATUS_UPLOAD_DONE	BIT(1)
#define STATUS_UPLOAD_ERROR	BIT(2)
#define STATUS_IDLE		BIT(3)
#define STATUS_NO_EEPROM	BIT(4)
#define HOST_AP_SUSPENDED	BIT(5)
#define ERROR_STACK		0x27
#define ERROR_MATH		0x30
#define ERROR_MEMORY		0x40

#define PAGE_SYSTEM		1
#define PAGE_SENSORS		3
#define PAGE_PRODUCT		14
#define PARAM_META_CONTROL	1
#define PARAM_FIFO_CONTROL	2
#define PARAM_STEP_INIT		2
#define PARAM_CONFIG		31	/* a sensor's config is its id + 31, its info its id */
#define PARAM_READ_MAX		16
#define PARAM_WRITE_MAX		8
#define PARAM_WRITE		0x80
#define PARAM_POLL_MS		2
#define PARAM_TRIES		100

#define ID_STEP_COUNTER		19
#define ID_WRIST_TILT		26
#define ID_DEBUG		0xf5
#define ID_TIME_LOW		0xfc
#define ID_TIME_HIGH		0xfd
#define ID_META			0xfe
#define META_OVERFLOW		12
#define META_WATERMARK		14
#define META_INITIALIZED	16

#define STEP_INTERVAL_MS	1000
#define TILT_RATE_HZ		30
#define CONFIG_GAP_MS		20
#define FIFO_SPARE		500

#define FW_SIGNATURE		0x652a
#define FW_HEADER		16
#define UPLOAD_CHUNK		32
#define RESET_MS		1000
#define RUN_MS			100
#define START_POLL_MS		10
#define INIT_MS			2000
#define CHECK_MS		2000
#define CHECK_MAX_MS		10000
#define DRAIN_MS		60000
#define PIN_STUCK_MS		100
#define RESTARTS		3

enum state { S_OFF, S_START, S_READY, S_FAILED };

static enum state state;
static uint32_t state_t, poll_t;
static bool bus_ok, ap_suspended;
static uint8_t product[2], version[4];
static uint32_t fw_crc;
static unsigned restarts;

/* Filled by the parser, in interrupt context once the chain runs. */
static volatile uint32_t steps = HUB_NONE;
static uint32_t step_base;
static uint16_t step_prev;
static volatile bool tilted, initialized;
static volatile uint32_t tilts, events, fifo_bytes, overflows, corrupt_reads, irqs, chains, errors;
static uint8_t wake_prev;
static struct { uint8_t id, a, b; } metas[8];
static volatile unsigned meta_head, meta_tail;
static char debug_text[13];
static volatile bool debug_pending;
static uint8_t pend[FIFO_WINDOW + 16];
static unsigned pend_len;
static bool corrupt;

/* The interrupt chain. */
static volatile bool busy, claimed, kick, check_due;
static volatile uint32_t chain_end_t;
static uint32_t check_t;
static uint8_t rx[FIFO_WINDOW], wake_buf, count_buf[2];
static unsigned remaining, offset, chunk = FIFO_WINDOW;

static int rd(uint8_t reg, void *p, unsigned n)
{
	return i2c_qup_xfer(QUP, ADDR, &reg, 1, p, n);
}

static int wr(uint8_t reg, const void *p, unsigned n)
{
	uint8_t buf[1 + PARAM_WRITE_MAX];
	buf[0] = reg;
	memcpy(buf + 1, p, n);
	return i2c_qup_xfer(QUP, ADDR, buf, n + 1, NULL, 0);
}

static int wr8(uint8_t reg, uint8_t v)
{
	return wr(reg, &v, 1);
}

static int set_bits(uint8_t reg, uint8_t bits, bool on)
{
	uint8_t v;
	int r = rd(reg, &v, 1);
	if (r)
		return r;
	uint8_t w = on ? v | bits : v & (uint8_t)~bits;
	return w == v ? 0 : wr8(reg, w);
}

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void enter(enum state s)
{
	state = s;
	state_t = time_ms();
}

/* Parameters: the hub takes a few ms per request. */
static int param_ack(uint8_t want)
{
	for (unsigned i = 0; i < PARAM_TRIES; i++) {
		uint8_t ack;
		delay_ms(PARAM_POLL_MS);
		wdt_feed();
		int r = rd(REG_PARAM_ACK, &ack, 1);
		if (r)
			return r;
		if (ack == want)
			return 0;
	}
	return -6;
}

static int param_read(uint8_t page, uint8_t param, void *p, unsigned n)
{
	int r = wr8(REG_PARAM_PAGE, (uint8_t)(n < PARAM_READ_MAX ? n << 4 | page : page));
	if (!r)
		r = wr8(REG_PARAM_REQ, param);
	if (!r)
		r = param_ack(param);
	if (!r)
		r = rd(REG_PARAM_SAVE, p, n);
	wr8(REG_PARAM_PAGE, 0);
	wr8(REG_PARAM_REQ, 0);
	return r;
}

static int param_write(uint8_t page, uint8_t param, const void *p, unsigned n)
{
	int r = wr8(REG_PARAM_PAGE, (uint8_t)(n < PARAM_WRITE_MAX ? n << 4 | page : page));
	if (!r)
		r = wr(REG_PARAM_LOAD, p, n);
	if (!r)
		r = wr8(REG_PARAM_REQ, param | PARAM_WRITE);
	if (!r)
		r = param_ack(param | PARAM_WRITE);
	wr8(REG_PARAM_PAGE, 0);
	wr8(REG_PARAM_REQ, 0);
	return r;
}

/* Sensor config: sample rate, max report latency (ms), change
 * sensitivity, dynamic range, all u16. */
static int sensor_rate(uint8_t id, uint16_t rate)
{
	uint8_t config[8] = { (uint8_t)rate, (uint8_t)(rate >> 8) };
	return param_write(PAGE_SENSORS, id + PARAM_CONFIG, config, sizeof config);
}

/* FIFO events: payload bytes after the id, -1 for an id that cannot be
 * there (the rest of the read is then garbage). */
static int event_size(uint8_t id)
{
	switch (id) {
	case 0: case 17: case 18: case 22: case 23: case 24: case 25: case 26: case 30:
		return 0;
	case 21: case 28:
		return 1;
	case 5: case 7: case 8: case 12: case 13: case 19: case 31: case ID_TIME_LOW: case ID_TIME_HIGH:
		return 2;
	case 6: case 29: case ID_META:
		return 3;
	case 1: case 2: case 3: case 4: case 9: case 10:
		return 7;
	case 11: case 15: case 20:
		return 10;
	case 14: case 16: case ID_DEBUG:
		return 13;
	default:
		return -1;
	}
}

static void event(const uint8_t *p)
{
	events++;
	if (p[0] == ID_STEP_COUNTER) {
		uint16_t count = le16(p + 1);
		if (steps != HUB_NONE && count < step_prev)
			step_base += 0x10000;
		step_prev = count;
		steps = step_base + count;
	} else if (p[0] == ID_META) {
		if (p[1] == META_INITIALIZED)
			initialized = true;
		else if (p[1] == META_OVERFLOW)
			overflows++;
		unsigned next = (meta_head + 1) % ARRAY_SIZE(metas);
		if (next != meta_tail) {
			metas[meta_head].id = p[1];
			metas[meta_head].a = p[2];
			metas[meta_head].b = p[3];
			barrier();
			meta_head = next;
		}
	} else if (p[0] == ID_DEBUG && !debug_pending && !(p[1] >> 6)) {
		unsigned len = (p[1] & 0x3f) < 12 ? p[1] & 0x3f : 12;
		memcpy(debug_text, p + 2, len);
		debug_text[len] = 0;
		barrier();
		debug_pending = true;
	}
}

/* Bytes in FIFO order; an event split across two reads waits in pend. */
static void parse(const uint8_t *p, unsigned n)
{
	fifo_bytes += n;
	while (n && !corrupt) {
		unsigned k = n < sizeof pend - pend_len ? n : sizeof pend - pend_len, i = 0;
		memcpy(pend + pend_len, p, k);
		pend_len += k;
		p += k;
		n -= k;
		while (i < pend_len) {
			int size = event_size(pend[i]);
			if (size < 0) {
				corrupt = true;
				corrupt_reads++;
				break;
			}
			if (i + 1 + (unsigned)size > pend_len)
				break;
			if (pend[i])
				event(pend + i);
			i += 1 + (unsigned)size;
		}
		memmove(pend, pend + i, pend_len - i);
		pend_len -= i;
	}
}

static void parse_end(void)
{
	pend_len = 0;
	corrupt = false;
}

/* The next piece of the FIFO: no more than the QUP takes and never past
 * the end of the window. */
static unsigned piece(unsigned left, unsigned at)
{
	unsigned n = left < chunk ? left : chunk;
	return n < FIFO_WINDOW - at % FIFO_WINDOW ? n : FIFO_WINDOW - at % FIFO_WINDOW;
}

static int fifo_read_sync(void)
{
	uint8_t c[2];
	int r = rd(REG_FIFO_BYTES, c, 2);
	unsigned left = r ? 0 : le16(c), at = 0;
	while (!r && left) {
		unsigned n = piece(left, at);
		r = rd((uint8_t)(at % FIFO_WINDOW), rx, n);
		if (r == -3 && chunk > 1) {
			chunk /= 2;
			r = 0;
		} else if (!r) {
			parse(rx, n);
			left -= n;
			at += n;
		}
	}
	parse_end();
	return r;
}

static void chain_end(int err)
{
	if (err)
		errors++;
	parse_end();
	chain_end_t = time_ms();
	check_due = true;
	busy = false;
	if (!claimed)
		tlmm_irq_enable(GPIO_IRQ, true);
}

static void piece_done(int err, void *ctx);

static void fifo_next(void)
{
	static uint8_t reg;
	while (remaining) {
		unsigned n = piece(remaining, offset);
		reg = (uint8_t)(offset % FIFO_WINDOW);
		int r = i2c_qup_start(QUP, ADDR, &reg, 1, rx, n, piece_done, (void *)(uintptr_t)n);
		if (!r)
			return;
		if (r != -3 || chunk == 1) {
			chain_end(r);
			return;
		}
		chunk /= 2;
	}
	chain_end(0);
}

static void piece_done(int err, void *ctx)
{
	unsigned n = (unsigned)(uintptr_t)ctx;
	if (err) {
		chain_end(err);
		return;
	}
	parse(rx, n);
	remaining -= n;
	offset += n;
	fifo_next();
}

static void count_done(int err, void *ctx)
{
	if (err) {
		chain_end(err);
		return;
	}
	remaining = le16(count_buf);
	offset = 0;
	fifo_next();
}

static void wake_done(int err, void *ctx)
{
	static const uint8_t reg = REG_FIFO_BYTES;
	if (!err) {
		unsigned n = (unsigned)(wake_buf - wake_prev) & 3;
		if (n) {
			tilts += n;
			tilted = true;
		}
		wake_prev = wake_buf;
	}
	if (i2c_qup_start(QUP, ADDR, &reg, 1, count_buf, 2, count_done, NULL))
		chain_end(-1);
}

/* With interrupts off or from the pin interrupt. The pin's status is
 * cleared before the reads, so an edge during the chain fires again
 * once the chain unmasks it. */
static void chain_start(void)
{
	static const uint8_t reg = REG_WAKE_SRC;
	busy = true;
	chains++;
	tlmm_irq_enable(GPIO_IRQ, false);
	tlmm_irq_ack(GPIO_IRQ);
	if (i2c_qup_start(QUP, ADDR, &reg, 1, &wake_buf, 1, wake_done, NULL))
		chain_end(-1);
}

static void pin_irq(unsigned pin)
{
	irqs++;
	if (busy)
		tlmm_irq_enable(GPIO_IRQ, false);
	else
		chain_start();
}

/* The bus for main-loop transfers: the pin interrupt masked and any
 * chain run to its end. */
static void claim(void)
{
	claimed = true;
	tlmm_irq_enable(GPIO_IRQ, false);
	while (busy) {
		i2c_qup_poll(QUP);
		wdt_feed();
	}
}

static void release(void)
{
	claimed = false;
	if (state == S_READY)
		tlmm_irq_enable(GPIO_IRQ, true);
}

/* CRC-32 (MSB first, polynomial 0x04c11db7, no final xor) of the image
 * as it goes over the bus, its words byte-swapped: the header's value. */
static uint32_t image_crc(const uint8_t *p, uint32_t n)
{
	uint32_t c = 0xffffffffu;
	for (uint32_t i = 0; i < n; i++) {
		c ^= (uint32_t)p[i ^ 3] << 24;
		for (int b = 0; b < 8; b++)
			c = c & 0x80000000u ? c << 1 ^ 0x04c11db7u : c << 1;
	}
	return c;
}

/* Reset, upload, check, run. */
static bool boot(void)
{
	uint32_t size;
	const uint8_t *fw = firmware_get(FIRMWARE, &size);
	if (!fw)
		return false;
	uint32_t text = size >= FW_HEADER ? le16(fw + 12) : 0, crc;
	if (size < FW_HEADER || le16(fw) != FW_SIGNATURE || FW_HEADER + text > size || (size - FW_HEADER) % 4 ||
	    text % 4) {
		log_printf("hub: %s: not a hub image (%lu bytes)\n", FIRMWARE, (unsigned long)size);
		return false;
	}
	fw_crc = le32(fw + 4);
	if ((crc = image_crc(fw + FW_HEADER, text)) != fw_crc) {
		log_printf("hub: %s: crc %08lx, header says %08lx\n", FIRMWARE, (unsigned long)crc, (unsigned long)fw_crc);
		return false;
	}

	uint8_t st = 0, c[4];
	uint32_t t0 = time_ms();
	int r = wr8(REG_RESET_REQ, 1);
	do {
		delay_ms(10);
		wdt_feed();
		r = rd(REG_CHIP_STATUS, &st, 1);
	} while ((r || !(st & STATUS_IDLE) || !(st & (STATUS_UPLOAD_DONE | STATUS_UPLOAD_ERROR | STATUS_NO_EEPROM))) &&
		 time_ms() - t0 < RESET_MS);
	log_printf("hub: reset, chip status %02x (%d) after %lu ms\n", st, r, (unsigned long)(time_ms() - t0));

	uint8_t buf[1 + UPLOAD_CHUNK];
	unsigned step = UPLOAD_CHUNK;
	buf[0] = REG_UPLOAD_DATA;
	t0 = time_ms();
	r = set_bits(REG_CHIP_CONTROL, CHIP_UPLOAD, true);
	for (uint32_t off = FW_HEADER; !r && off < size;) {
		unsigned n = size - off < step ? size - off : step;
		for (unsigned i = 0; i < n; i++)
			buf[1 + i] = fw[off + (i ^ 3)];
		r = i2c_qup_xfer(QUP, ADDR, buf, n + 1, NULL, 0);
		if (r == -3 && step > 4) {
			step /= 2;
			r = 0;
		} else if (!r) {
			off += n;
			wdt_feed();
		}
	}
	if (!r)
		r = set_bits(REG_CHIP_CONTROL, CHIP_UPLOAD, false);
	if (!r)
		r = rd(REG_CRC, c, 4);
	if (r || le32(c) != fw_crc) {
		log_printf("hub: upload failed (%d), crc %08lx, image %08lx\n", r, r ? 0ul : (unsigned long)le32(c),
			   (unsigned long)fw_crc);
		return false;
	}
	log_printf("hub: %lu bytes uploaded in %lu ms, %u per write, crc %08lx\n", (unsigned long)(size - FW_HEADER),
		   (unsigned long)(time_ms() - t0), step, (unsigned long)fw_crc);
	delay_ms(RUN_MS);
	if ((r = set_bits(REG_CHIP_CONTROL, CHIP_CPU_RUN, true))) {
		log_printf("hub: cpu run failed (%d)\n", r);
		return false;
	}
	if (!rd(REG_VERSION, version, sizeof version))
		log_printf("hub: rom %04x ram %04x\n", le16(version), le16(version + 2));
	initialized = false;
	parse_end();
	return true;
}

static void configure(void)
{
	uint8_t fifo[8], meta[8], info[16];
	int r = param_read(PAGE_SYSTEM, PARAM_FIFO_CONTROL, fifo, sizeof fifo);
	uint16_t fifo_size = le16(fifo + 2);
	log_printf("hub: fifo %u bytes, watermark %u (%d)\n", r ? 0 : fifo_size, r ? 0 : le16(fifo), r);
	if (!r && fifo_size > FIFO_SPARE) {
		fifo[0] = (uint8_t)(fifo_size - FIFO_SPARE);
		fifo[1] = (uint8_t)((fifo_size - FIFO_SPARE) >> 8);
		if ((r = param_write(PAGE_SYSTEM, PARAM_FIFO_CONTROL, fifo, sizeof fifo)))
			log_printf("hub: watermark not set (%d)\n", r);
	}
	/* The watermark meta event in the FIFO, without its own interrupt:
	 * per event two bits, interrupt then event. */
	unsigned bit = 2 * (META_WATERMARK - 1);
	if (!param_read(PAGE_SYSTEM, PARAM_META_CONTROL, meta, sizeof meta)) {
		meta[bit / 8] = (uint8_t)((meta[bit / 8] | BIT((bit + 1) % 8)) & ~BIT(bit % 8));
		param_write(PAGE_SYSTEM, PARAM_META_CONTROL, meta, sizeof meta);
	}
	static const uint8_t ids[] = { ID_STEP_COUNTER, ID_WRIST_TILT };
	for (unsigned i = 0; i < ARRAY_SIZE(ids); i++) {
		r = param_read(PAGE_SENSORS, ids[i], info, sizeof info);
		log_printf("hub: sensor %u: type %u driver %u.%u, max rate %u, fifo max %u, event %u bytes (%d)\n", ids[i],
			   info[0], info[1], info[2], le16(info + 8), le16(info + 12), info[14], r);
	}
	/* The count carries on across a restart of the hub. */
	uint16_t start = steps == HUB_NONE ? 0 : (uint16_t)steps;
	uint8_t start_le[2] = { (uint8_t)start, (uint8_t)(start >> 8) };
	r = param_write(PAGE_PRODUCT, PARAM_STEP_INIT, start_le, sizeof start_le);
	log_printf("hub: step count from %u (%d)\n", start, r);
	r = sensor_rate(ID_STEP_COUNTER, STEP_INTERVAL_MS);
	log_printf("hub: step counter %s (%d)\n", r ? "failed" : "on", r);
	delay_ms(CONFIG_GAP_MS);
	r = sensor_rate(ID_WRIST_TILT, TILT_RATE_HZ);
	log_printf("hub: wrist tilt %s (%d)\n", r ? "failed" : "on", r);
	if (rd(REG_WAKE_SRC, &wake_prev, 1))
		wake_prev = 0;
	chunk = FIFO_WINDOW;
	tlmm_irq_config(GPIO_IRQ, TLMM_IRQ_EDGE_RISING, pin_irq);
	spm_wake_pin(MPM_PIN_IRQ, true, true);
	ap_suspended = false;
	check_due = false;
	enter(S_READY);
	tlmm_irq_enable(GPIO_IRQ, true);
	kick = true;
}

static void restart(const char *why, unsigned v)
{
	log_printf("hub: %s %02x, restarting\n", why, v);
	tlmm_irq_enable(GPIO_IRQ, false);
	spm_wake_pin(MPM_PIN_IRQ, false, true);
	enter(S_FAILED);
	if (++restarts > RESTARTS || !boot()) {
		log_printf("hub: given up after %u restarts\n", restarts);
		return;
	}
	enter(S_START);
}

static void health(void)
{
	uint8_t err, c[4];
	claim();
	int r = rd(REG_ERROR, &err, 1);
	if (!r)
		r = rd(REG_CRC, c, 4);
	release();
	if (r)
		return;
	if (err == ERROR_STACK || err == ERROR_MATH || err == ERROR_MEMORY)
		restart("fatal error", err);
	else if (le32(c) && le32(c) != fw_crc)
		restart("image crc changed, error", err);
}

static void log_pending(void)
{
	static const char *const names[] = {
		"?", "flush complete", "rate changed", "power mode changed", "error", "mag transient", "cal status",
		"stillness", "algorithm slowdown", "cal stable", "?", "sensor error", "fifo overflow", "dynamic range",
		"fifo watermark", "self test", "initialized",
	};
	while (meta_tail != meta_head) {
		barrier();
		uint8_t id = metas[meta_tail].id, a = metas[meta_tail].a, b = metas[meta_tail].b;
		barrier();
		meta_tail = (meta_tail + 1) % ARRAY_SIZE(metas);
		log_printf("hub: meta %s %02x %02x\n", id < ARRAY_SIZE(names) ? names[id] : "?", a, b);
	}
	if (debug_pending) {
		barrier();
		log_printf("hub: debug %s\n", debug_text);
		debug_pending = false;
	}
}

bool hub_init(void)
{
	tlmm_config(GPIO_IRQ, 0, TLMM_INPUT, TLMM_PULL_DOWN, 2);
	int r = i2c_qup_init(QUP, BOARD_HUB_SDA, BOARD_HUB_SCL, BOARD_HUB_FUNC, BUS_HZ);
	if (!r)
		r = rd(REG_PRODUCT_ID, product, sizeof product);
	if (r) {
		log_printf("hub: no answer on i2c (%d)\n", r);
		enter(S_FAILED);
		return false;
	}
	bus_ok = true;
	log_printf("hub: product %02x revision %02x, irq %u\n", product[0], product[1], tlmm_get(GPIO_IRQ));
	if (!boot()) {
		enter(S_FAILED);
		return false;
	}
	enter(S_START);
	return true;
}

void hub_poll(void)
{
	if (state == S_OFF || state == S_FAILED)
		return;
	i2c_qup_poll(QUP);
	log_pending();
	uint32_t now = time_ms();
	if (state == S_START) {
		if (now - poll_t >= START_POLL_MS) {
			poll_t = now;
			if (fifo_read_sync())
				errors++;
			log_pending();
		}
		if (initialized || now - state_t >= INIT_MS) {
			if (!initialized)
				log_printf("hub: no initialized event in %u ms\n", INIT_MS);
			configure();
		}
		return;
	}
	if (ap_suspended && power_awake()) {
		claim();
		int r = set_bits(REG_HOST_CONTROL, HOST_AP_SUSPENDED, false);
		release();
		if (!r)
			ap_suspended = false;
		kick = true;
	}
	if (ap_suspended && now - chain_end_t >= DRAIN_MS)
		kick = true;
	/* Checked after a quiet spell, or anyway after a while: a hub that
	 * holds its pin high keeps the chains going and is never quiet. */
	if (check_due && !busy && (now - chain_end_t >= CHECK_MS || now - check_t >= CHECK_MAX_MS)) {
		check_due = false;
		check_t = now;
		health();
	}
	if (!busy && (kick || (tlmm_get(GPIO_IRQ) && now - chain_end_t >= PIN_STUCK_MS))) {
		uint32_t flags = irq_save();
		if (!busy) {
			kick = false;
			chain_start();
		}
		irq_restore(flags);
	}
}

/* For the bench: one byte reads that register and returns its value,
 * more write the rest to the register in the first. */
int hub_send(const uint8_t *p, unsigned n)
{
	if (!bus_ok || !n || n > 1 + PARAM_WRITE_MAX)
		return -1;
	uint8_t v = 0;
	claim();
	int r = n == 1 ? rd(p[0], &v, 1) : wr(p[0], p + 1, n - 1);
	release();
	return r ? r : v;
}

/* The hub keeps its own time (31.25 us ticks) and needs no wall clock. */
void hub_set_time(void)
{
}

uint32_t hub_steps(void)
{
	return steps;
}

void hub_heart_rate_on(bool on)
{
}

uint32_t hub_heart_rate(uint8_t *status, unsigned *readings)
{
	if (status)
		*status = 0;
	if (readings)
		*readings = 0;
	return HUB_NONE;
}

bool hub_tilted(void)
{
	bool t = tilted;
	tilted = false;
	return t;
}

bool hub_can_sleep(void)
{
	if (state == S_START)
		return false;
	if (state != S_READY)
		return true;
	return !busy && !kick && !(tlmm_get(GPIO_IRQ) && time_ms() - chain_end_t < 5 * PIN_STUCK_MS);
}

/* Told once per sleep, not at every cluster collapse: the flag is
 * cleared from hub_poll when the screen comes on. */
void hub_sleep(bool asleep)
{
	if (!asleep || state != S_READY || ap_suspended)
		return;
	claim();
	int r = set_bits(REG_HOST_CONTROL, HOST_AP_SUSPENDED, true);
	release();
	if (!r)
		ap_suspended = true;
}

void hub_status(void (*say)(const char *fmt, ...))
{
	static const char *const names[] = { "off", "starting", "ready", "failed" };
	say("hub: %s, product %02x rev %02x, rom %04x ram %04x, image crc %08lx, %u restarts\n", names[state], product[0],
	    product[1], le16(version), le16(version + 2), (unsigned long)fw_crc, restarts);
	if (bus_ok) {
		uint8_t chip = 0, host = 0, control = 0, err = 0, wake = 0, c[2] = { 0, 0 };
		claim();
		int r = rd(REG_CHIP_CONTROL, &chip, 1);
		if (!r)
			r = rd(REG_HOST_STATUS, &host, 1);
		if (!r)
			r = rd(REG_HOST_CONTROL, &control, 1);
		if (!r)
			r = rd(REG_ERROR, &err, 1);
		if (!r)
			r = rd(REG_WAKE_SRC, &wake, 1);
		if (!r)
			r = rd(REG_FIFO_BYTES, c, 2);
		uint8_t status = 0;
		if (!r)
			r = rd(REG_CHIP_STATUS, &status, 1);
		release();
		say("hub: chip control %02x status %02x, host status %02x control %02x, error %02x, wake %02x, fifo %u (%d)\n",
		    chip, status, host, control, err, wake, le16(c), r);
	}
	say("hub: steps %ld, %lu tilts, %lu irqs, %lu reads, %lu bytes, %lu events, %lu overflows, %lu corrupt, %lu errors, "
	    "piece %u, ap %s, irq pin %u\n",
	    steps == HUB_NONE ? -1L : (long)steps, (unsigned long)tilts, (unsigned long)irqs, (unsigned long)chains,
	    (unsigned long)fifo_bytes, (unsigned long)events, (unsigned long)overflows, (unsigned long)corrupt_reads,
	    (unsigned long)errors, chunk, ap_suspended ? "suspended" : "awake", tlmm_get(GPIO_IRQ));
}

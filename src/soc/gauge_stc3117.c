/* An STC3117 fuel gauge on I2C, run by ST's algorithm in stc3117.c with
 * the board's cell. Started like the vendor's probe: the algorithm's
 * start, a first task 200 ms later, and the chip left in mixed mode,
 * which it is put back to whenever the watch sleeps or wakes. A task
 * runs whenever the charger asks for a reading. */
#include "asteroid.h"
#include "soc/stc3117.h"

#define BUS		(board->gauge.i2c.bus)
#define ADDR		(board->gauge.i2c.addr)
#define BUS_HZ		100000
#define FIRST_TASK_MS	200
/* Asked again within this, the last reading stands. */
#define MIN_TASK_MS	1000
#define MAX_SOC		1000

static struct stc3117 gg;
static struct hub_gauge last;
static bool ready, have;
static uint32_t task_t, errors;
static int last_res = 2;

static int io_read(uint8_t reg, uint8_t *buf, unsigned n)
{
	int r = i2c_xfer(BUS, ADDR, &reg, 1, buf, n);
	if (r)
		errors++;
	return r;
}

static int io_write(uint8_t reg, const uint8_t *buf, unsigned n)
{
	uint8_t b[1 + STC3117_RAM_SIZE];
	if (n > sizeof b - 1)
		return -3;
	b[0] = reg;
	for (unsigned i = 0; i < n; i++)
		b[1 + i] = buf[i];
	int r = i2c_xfer(BUS, ADDR, b, n + 1, NULL, 0);
	if (r)
		errors++;
	return r;
}

static const struct stc3117_io io = { io_read, io_write };

static void mode_run(void)
{
	uint8_t b[2] = { STC3117_REG_MODE, STC3117_MODE_RUN };
	if (i2c_xfer(BUS, ADDR, b, 2, NULL, 0))
		errors++;
}

static void task(void)
{
	struct stc3117_result r = { 0 };
	int res = stc3117_task(&gg, &r);
	task_t = time_ms();
	if (res != last_res)
		log_printf("gauge: task %d, %d.%d %% %d mV %d mA %d.%d C, mode %02x ctrl %02x, %lu errors\n", res,
			   r.soc / 10, r.soc % 10, r.mv, r.ma, r.decidegrees / 10,
			   (r.decidegrees < 0 ? -r.decidegrees : r.decidegrees) % 10, gg.status & 0xff,
			   (gg.status >> 8) & 0xff, errors);
	last_res = res;
	if (res < 0)
		return;
	int soc = r.soc < 0 ? 0 : r.soc > MAX_SOC ? MAX_SOC : r.soc;
	last = (struct hub_gauge){ (uint32_t)r.mv, (uint32_t)(soc + 5) / 10, -r.ma * 1000, r.decidegrees, task_t };
	have = true;
}

static bool stc3117_init(void)
{
	if (!board->gauge.cell)
		return false;
	if (i2c_init(BUS, board->gauge.i2c.sda, board->gauge.i2c.scl, board->gauge.i2c.func, BUS_HZ))
		return false;
	int res = stc3117_start(&gg, board->gauge.cell, &io);
	if (res < 0) {
		log_printf("gauge: no STC3117 (%d, %lu errors)\n", res, errors);
		return false;
	}
	delay_ms(FIRST_TASK_MS);
	wdt_feed();
	ready = true;
	task();
	mode_run();
	log_printf("gauge: STC3117 started (%c), cc %u vm %u\n", gg.start, gg.ram[4] | gg.ram[5] << 8,
		   gg.ram[6] | gg.ram[7] << 8);
	return true;
}

static void stc3117_poll(bool now)
{
	if (ready && (!have || time_ms() - task_t >= MIN_TASK_MS))
		task();
}

static bool stc3117_read(struct hub_gauge *g)
{
	if (!have)
		return false;
	*g = last;
	return true;
}

static void stc3117_asleep(bool asleep)
{
	if (ready)
		mode_run();
}

const struct gauge_driver gauge_stc3117 = {
	.init = stc3117_init,
	.poll = stc3117_poll,
	.read = stc3117_read,
	.asleep = stc3117_asleep,
};

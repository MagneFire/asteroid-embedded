/* beluga's panel start with the size detection: a played RM69090 of
 * either size behind the SPI calls, then boots where the AP has the
 * panel, with and without the size kept from the boot before. */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "soc/apollo3/detect.c"
#include "display/display_rm69090.c"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static const struct dcs_step on_46[] = { { 0xfe, 1, 0, { 0x00 } }, { 0x31, 4, 0, { 0x00, 0x28, 0x01, 0xb9 } },
					 { 0x11, 0, 64, { 0 } }, { 0x29, 0, 0, { 0 } } };
static const struct dcs_step on_41[] = { { 0xfe, 1, 0, { 0x05 } }, { 0xa3, 1, 0, { 0x80 } },
					 { 0x11, 0, 85, { 0 } }, { 0x29, 0, 0, { 0 } } };
#define PANEL .te_gpio = 11, .reset_gpio = 10, .spi_bus = 5, .spi_cs = 3, .spi_mode = 3, .spi_hz = 48000000, \
	      .dc_gpio = 46, .spi_pads = { 45, 47, 48, 49 }
const struct board board_beluga = { .name = "beluga", .panel = { .width = 402, .height = 476, PANEL,
	.on = on_46, .on_n = ARRAY_SIZE(on_46), .enable_gpio = { 7, 12 }, .id = { 0x09, 0x80, 0x01 } } };
const struct board board_beluga_41 = { .name = "beluga-41", .panel = { .width = 320, .height = 360, PANEL,
	.on = on_41, .on_n = ARRAY_SIZE(on_41), .enable_gpio = { 7, 0 }, .id = { 0x01, 0x80, 0x01 } } };

/* The panel: its ID, whether it answers in sleep, and what it was sent
 * since the last check. */
static const uint8_t *panel_id;
static bool answers_in_sleep, in_sleep = true, rebooted;
static unsigned dc = 1, last_cmd, commands, id_reads, table_46, table_41;
static bool driven[64];
static jmp_buf reboot_jmp;

void log_printf(const char *fmt, ...) {}
uint32_t time_ms(void) { return 0; }
void delay_ms(uint32_t ms) {}
void reboot(void)
{
	rebooted = true;
	longjmp(reboot_jmp, 1);
}
void gpio_set(unsigned n, unsigned v)
{
	if (n == 46)
		dc = v;
	if (n == 10 && !v)
		in_sleep = true;
}
unsigned gpio_get(unsigned n) { return 0; }
void gpio_config(unsigned n, unsigned func, unsigned dir, unsigned pull, unsigned drive_ma) { driven[n] = dir == GPIO_OUTPUT; }
void pad_set(const struct board_mcu_pad *p) {}
int iom_spi_init(unsigned bus, uint32_t hz, unsigned mode) { return 0; }
void iom_spi_set_hz(unsigned bus, uint32_t hz, unsigned mode) {}
int iom_spi_wait(unsigned bus) { return 0; }
void iom_stop(unsigned bus) {}
int iom_spi_write(unsigned bus, unsigned cs, const void *p, unsigned n, bool cont)
{
	const uint8_t *b = p;
	if (dc) {
		if (last_cmd == 0x31 && n == 4)
			table_46 += b[1] == 0x28;
		return 0;
	}
	last_cmd = b[0];
	commands++;
	table_41 += last_cmd == 0xa3;
	if (last_cmd == 0x11)
		in_sleep = false;
	return 0;
}
int iom_spi_write_start(unsigned bus, unsigned cs, const void *p, unsigned n, bool cont) { return 0; }
int iom_spi_read(unsigned bus, unsigned cs, void *p, unsigned n, bool cont)
{
	uint8_t *b = p;
	memset(b, 0, n);
	if (last_cmd >= 0xda && last_cmd <= 0xdc) {
		id_reads++;
		if (!in_sleep || answers_in_sleep)
			b[0] = panel_id[last_cmd - 0xda];
	}
	return 0;
}
void tsl258x_crop_line(uint32_t y, uint32_t x, uint32_t w, const uint16_t *rgb565) {}

static const uint8_t id_46[3] = { 0x09, 0x80, 0x01 }, id_41[3] = { 0x01, 0x80, 0x01 }, id_other[3] = { 0x07, 0x80, 0x01 };

/* A warm reset: the driver and the board start over, the .kept record
 * stays. */
static void boot(const uint8_t *id, bool sleep_reads, bool ap_has_it)
{
	started = panel_on = panel_asleep = idle = lost = lent = unread = reads_work = false;
	level = 0xff;
	board = &board_beluga;
	panel_id = id;
	answers_in_sleep = sleep_reads;
	in_sleep = true;
	commands = id_reads = table_46 = table_41 = 0;
	rebooted = false;
	memset(driven, 0, sizeof driven);
	if (ap_has_it)
		display_start_lent();
	CHECK(display_init());
}

/* The watch takes the panel back and wakes. */
static void take_back(void)
{
	display_lend(false);
	if (!setjmp(reboot_jmp))
		display_wake();
}

int main(void)
{
	/* Owned boots: each size by its ID in sleep, or after the 46 mm
	 * table for a panel that answers only out of it. */
	boot(id_41, true, false);
	CHECK(board == &board_beluga_41 && fb.width == 320 && fb.height == 360 && table_41 == 1 && !table_46);
	boot(id_46, true, false);
	CHECK(board == &board_beluga && fb.width == 402 && table_46 == 1 && !table_41);
	boot(id_41, false, false);
	CHECK(board == &board_beluga_41 && fb.width == 320 && table_46 == 1 && table_41 == 1);

	/* Lent after a boot that read a 41 mm panel: its size, without a
	 * command or a pin driven; given back, the 41 mm table and no ID
	 * read. */
	boot(id_41, true, true);
	CHECK(board == &board_beluga_41 && fb.width == 320 && fb.height == 360 && !commands && !driven[7] && !driven[12]);
	take_back();
	CHECK(!rebooted && board == &board_beluga_41 && table_41 == 1 && !table_46 && !id_reads);
	/* A lent boot after a lent boot keeps it too. */
	boot(id_41, true, true);
	CHECK(board == &board_beluga_41);

	/* An unknown panel is kept as the 46 mm watch. */
	boot(id_other, true, false);
	CHECK(board == &board_beluga);
	boot(id_41, true, true);
	CHECK(board == &board_beluga && fb.width == 402);

	/* Nothing kept: the 46 mm watch until the panel is back, and then
	 * its ID. A 46 mm panel goes on; a 41 mm one boots again. */
	kept.magic = 0;
	boot(id_46, true, true);
	CHECK(board == &board_beluga && fb.width == 402 && !commands);
	take_back();
	CHECK(!rebooted && board == &board_beluga && id_reads == 3 && table_46 == 1 && panel_on);
	kept.check ^= 1;
	boot(id_41, false, true);
	CHECK(board == &board_beluga && fb.width == 402);
	take_back();
	CHECK(rebooted && board == &board_beluga_41 && table_41 == 1);
	boot(id_41, true, true);
	CHECK(board == &board_beluga_41 && fb.width == 320);

	if (failures)
		printf("beluga_display_test: %d failures\n", failures);
	else
		printf("beluga_display_test: ok\n");
	return failures != 0;
}

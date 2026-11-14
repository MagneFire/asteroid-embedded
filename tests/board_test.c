/* Telling the watches apart, with the real board tables and device
 * trees: argv are smelt's and sturgeon's compiled trees. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "asteroid.h"

const struct touch_driver touch_rmi4, touch_it7260, touch_mxt, touch_fts;
const struct hub_driver hub_stm32, hub_em7184, hub_m4, hub_nanohub;
const struct motor_driver motor_drv2605, motor_pm8226;
const struct backlight_driver backlight_lm3535;
const struct battery_driver battery_pmic, battery_max17050, battery_smb231;
const struct light_driver light_tsl2584, light_nanohub;

const struct qcom_chip chip_8026, chip_8009;
const struct qcom_chip *chip = &chip_8026;

static uint32_t splash_w, splash_h, platform = 8;
uint32_t smem_hw_platform(void) { return platform; }

/* smelt's fuel gauge, for the boots with no splash. */
static bool gauge_present;
static unsigned gauge_reads;
int i2c_init(unsigned bus, unsigned sda, unsigned scl, unsigned func, uint32_t bus_hz) { return 0; }
int i2c_xfer(unsigned bus, uint8_t addr, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn)
{
	gauge_reads++;
	if (!gauge_present)
		return -1;
	r[0] = 0xac;
	r[1] = 0;
	return 0;
}
static unsigned splash_reads;
void display_splash_size(uint32_t *w, uint32_t *h)
{
	splash_reads++;
	*w = splash_w;
	*h = splash_h;
}
void log_printf(const char *fmt, ...) {}

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static const struct board *detect(uint32_t w, uint32_t h, const void *fdt)
{
	splash_w = w;
	splash_h = h;
	board_assumed = false;
	board_detect(0, fdt);
	return board;
}

static void *load(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		printf("%s: not found\n", path);
		exit(1);
	}
	static char bufs[2][4096];
	static int n;
	char *b = bufs[n++];
	fread(b, 1, sizeof bufs[0], f);
	fclose(f);
	return b;
}

int main(int argc, char **argv)
{
	void *smelt = load(argv[1]), *sturgeon = load(argv[2]);
	/* The tree names the watch. */
	CHECK(detect(400, 400, sturgeon) == &board_sturgeon);
	CHECK(detect(0, 0, sturgeon) == &board_sturgeon);
	CHECK(detect(0, 0, smelt) == &board_smelt && !board_assumed);
	CHECK(detect(360, 330, smelt) == &board_smelt);
	/* Without one the panel does. */
	CHECK(detect(400, 400, NULL) == &board_sturgeon && !board_assumed);
	CHECK(detect(320, 320, NULL) == &board_sparrow);
	CHECK(detect(360, 330, NULL) == &board_smelt);
	CHECK(detect(360, 325, NULL) == &board_smelt);
	/* smelt's panel beats another watch's tree. */
	CHECK(detect(360, 330, sturgeon) == &board_smelt);
	/* So does its SMEM platform, whatever else comes. */
	platform = 0x41;
	CHECK(detect(0, 0, sturgeon) == &board_smelt && !board_assumed);
	CHECK(detect(0, 0, NULL) == &board_smelt && !board_assumed);
	platform = 0;
	CHECK(detect(400, 400, sturgeon) == &board_sturgeon);
	platform = 8;
	/* With no splash the gauge outranks another watch's tree, and the
	 * bus stays untouched when there is a splash. */
	gauge_present = true;
	CHECK(detect(0, 0, sturgeon) == &board_smelt && !board_assumed);
	gauge_reads = 0;
	CHECK(detect(400, 400, sturgeon) == &board_sturgeon && gauge_reads == 0);
	gauge_present = false;
	CHECK(detect(0, 0, sturgeon) == &board_sturgeon);
	/* On the APQ8009W only catfish, and the APQ8026's display is not
	 * looked at. */
	chip = &chip_8009;
	splash_reads = 0;
	CHECK(detect(400, 400, sturgeon) == &board_catfish && !board_assumed && splash_reads == 0);
	chip = &chip_8026;
	/* A guess stays a guess. */
	CHECK(detect(0, 0, NULL) == &board_sturgeon && board_assumed);
	CHECK(detect(123, 456, NULL) == &board_sturgeon && board_assumed);

	/* The PMIC charger stays off on smelt. */
	CHECK(board_smelt.battery_driver == &battery_max17050);
	CHECK(!board_smelt.battery);
	CHECK(!board_smelt.chg.vdd_max_mv && !board_smelt.chg.ibat_max_ma);
	CHECK(board_sturgeon.battery_driver == &battery_pmic);
	CHECK(board_sparrow.battery_driver == &battery_pmic);

	/* Only smelt's always-on face has its own backlight code. */
	CHECK(board_smelt.backlight_aod_code == 36);
	CHECK(!board_sturgeon.backlight_aod_code && !board_sparrow.backlight_aod_code);

	/* Only smelt has a light sensor, and nothing turns its rail off. */
	CHECK(board_smelt.light == &light_tsl2584 && board_smelt.light_pins.i2c.addr == 0x29);
	CHECK(!board_sturgeon.light && !board_sparrow.light);
	for (unsigned i = 0; i < sizeof board_smelt.unused_ldos; i++)
		CHECK(board_smelt.unused_ldos[i] != board_smelt.light_pins.ldo);

	puts(failures ? "board_test: FAILED" : "board_test: ok");
	return failures != 0;
}

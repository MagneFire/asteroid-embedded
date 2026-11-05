/* Telling the watches apart, with the real board tables and device
 * trees: argv are smelt's and sturgeon's compiled trees. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "asteroid.h"

const struct touch_driver touch_rmi4, touch_it7260;
const struct hub_driver hub_stm32, hub_em7184;
const struct motor_driver motor_drv2605, motor_pm8226;
const struct backlight_driver backlight_lm3535;
const struct battery_driver battery_pmic, battery_max17050;

static uint32_t splash_w, splash_h;
void display_splash_size(uint32_t *w, uint32_t *h)
{
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
	/* A guess stays a guess. */
	CHECK(detect(0, 0, NULL) == &board_sturgeon && board_assumed);
	CHECK(detect(123, 456, NULL) == &board_sturgeon && board_assumed);

	/* The PMIC charger stays off on smelt. */
	CHECK(board_smelt.battery_driver == &battery_max17050);
	CHECK(!board_smelt.battery);
	CHECK(!board_smelt.chg.vdd_max_mv && !board_smelt.chg.ibat_max_ma);
	CHECK(board_sturgeon.battery_driver == &battery_pmic);
	CHECK(board_sparrow.battery_driver == &battery_pmic);

	puts(failures ? "board_test: FAILED" : "board_test: ok");
	return failures != 0;
}

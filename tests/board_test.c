/* Telling the watches apart, with the real board tables and device
 * trees: argv are smelt's, sturgeon's, dory's, catfish's, swift's, lenok's
 * and narwhal's compiled trees. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "asteroid.h"
#include "soc/max17050.h"

const struct touch_driver touch_rmi4, touch_it7260, touch_mxt, touch_fts, touch_hx852x;
const struct hub_driver hub_stm32, hub_em7184, hub_m4, hub_nanohub, hub_lis3dsh, hub_lsm6ds3, hub_mpu6515, hub_icm40604;
const struct motor_driver motor_drv2605, motor_pm8226, motor_gp_pwm;
const struct backlight_driver backlight_lm3535, backlight_lm3530, backlight_lm36272;
const struct battery_driver battery_pmic, battery_pmic_gauge, battery_max17050, battery_pmic_max17050, battery_smb231, battery_fan54511;
const struct gauge_driver gauge_stc3117, gauge_bq27421, gauge_max17050;
const struct hands_driver hands_sop716;
const struct crown_driver crown_paj9124;
const struct light_driver light_tsl2584, light_nanohub, light_al3320;

const struct qcom_chip chip_8026, chip_8009;
const struct qcom_chip *chip = &chip_8026;

static uint32_t splash_w, splash_h, platform = 8, subtype;
uint32_t smem_hw_platform(void) { return platform; }
uint32_t smem_hw_subtype(void) { return subtype; }

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
int regulator_on(uint32_t id, uint32_t uv) { return 0; }
int regulator_off(uint32_t id) { return 0; }
uint32_t time_ms(void) { return 0; }
void delay_ms(uint32_t ms) {}
void gpio_set(unsigned n, unsigned v) {}

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
	static char bufs[7][4096];
	static int n;
	char *b = bufs[n++];
	fread(b, 1, sizeof bufs[0], f);
	fclose(f);
	return b;
}

int main(int argc, char **argv)
{
	void *smelt = load(argv[1]), *sturgeon = load(argv[2]), *dory = load(argv[3]);
	void *catfish = load(argv[4]), *swift = load(argv[5]), *lenok = load(argv[6]);
	void *narwhal = load(argv[7]);
	/* The tree names the watch. */
	CHECK(detect(400, 400, sturgeon) == &board_sturgeon);
	CHECK(detect(0, 0, sturgeon) == &board_sturgeon);
	CHECK(detect(0, 0, smelt) == &board_smelt && !board_assumed);
	CHECK(detect(360, 330, smelt) == &board_smelt);
	CHECK(detect(280, 280, dory) == &board_dory);
	CHECK(detect(0, 0, dory) == &board_dory && !board_assumed);
	CHECK(detect(320, 320, lenok) == &board_lenok);
	CHECK(detect(0, 0, lenok) == &board_lenok && !board_assumed);
	/* Without one the panel does. */
	CHECK(detect(400, 400, NULL) == &board_sturgeon && !board_assumed);
	CHECK(detect(320, 320, NULL) == &board_sparrow);
	CHECK(detect(360, 330, NULL) == &board_smelt);
	CHECK(detect(360, 325, NULL) == &board_smelt);
	CHECK(detect(280, 280, NULL) == &board_dory);
	/* dory's own SMEM platform is not smelt's. */
	platform = 131;
	CHECK(detect(280, 280, dory) == &board_dory);
	/* lenok's panel is sparrow's size, but its SMEM platform is its own. */
	platform = 132;
	CHECK(detect(320, 320, NULL) == &board_lenok && !board_assumed);
	CHECK(detect(320, 320, lenok) == &board_lenok);
	CHECK(detect(0, 0, NULL) == &board_lenok && !board_assumed);
	platform = 8;
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
	/* On the APQ8009W only the tree names catfish or swift, which both
	 * read subtype 5, and the display is not looked at. Another SoC's
	 * tree names neither, and the guess drives nothing but the display
	 * and USB. */
	chip = &chip_8009;
	splash_reads = 0;
	CHECK(detect(400, 400, catfish) == &board_catfish && !board_assumed && splash_reads == 0);
	CHECK(detect(400, 400, swift) == &board_swift && !board_assumed && splash_reads == 0);
	CHECK(detect(360, 360, narwhal) == &board_narwhal && !board_assumed && splash_reads == 0);
	subtype = 5;
	CHECK(detect(400, 400, swift) == &board_swift && !board_assumed);
	CHECK(detect(0, 0, catfish) == &board_catfish && !board_assumed);
	const struct board *bare = detect(400, 400, NULL);
	CHECK(bare != &board_catfish && bare != &board_swift && bare != &board_narwhal && board_assumed);
	CHECK(detect(400, 400, sturgeon) == bare && board_assumed);
	CHECK(bare->panel.width == 400 && bare->panel.reset_gpio == board_catfish.panel.reset_gpio &&
	      bare->usb_phy_init[0] == board_catfish.usb_phy_init[0]);
	CHECK(!bare->touch && !bare->touch_alt && !bare->hub && !bare->light && !bare->motor && !bare->battery_driver &&
	      !bare->bt.patch && !bare->keys[0].gpio && !bare->fuel_gauge);
	subtype = 0;
	CHECK(detect(0, 0, NULL) == bare && board_assumed);
	chip = &chip_8026;
	/* An APQ8026 is never taken for either. */
	CHECK(detect(400, 400, swift) == &board_sturgeon && !board_assumed);
	/* A guess stays a guess. */
	CHECK(detect(0, 0, NULL) == &board_sturgeon && board_assumed);
	CHECK(detect(123, 456, NULL) == &board_sturgeon && board_assumed);

	/* The PMIC charger stays off on smelt. */
	CHECK(board_smelt.battery_driver == &battery_max17050);
	CHECK(!board_smelt.battery);
	CHECK(!board_smelt.chg.vdd_max_mv && !board_smelt.chg.ibat_max_ma);
	CHECK(board_sturgeon.battery_driver == &battery_pmic);
	CHECK(board_sparrow.battery_driver == &battery_pmic);
	/* dory charges with the PMIC and measures with its gauge, and the
	 * charger's limits are its cell's. */
	CHECK(board_dory.battery_driver == &battery_pmic_max17050 && !board_dory.battery);
	/* lenok charges with the PMIC and measures with its bq27421, and its
	 * touch controller's I/O supply is its own LDO. */
	CHECK(board_lenok.battery_driver == &battery_pmic_gauge && board_lenok.fuel_gauge == &gauge_bq27421);
	CHECK(board_lenok.chg.vdd_max_mv == 4350 && board_lenok.chg.step_down_mv == 4215 && board_lenok.chg.no_thermistor);
	for (unsigned i = 1; i < board_lenok.therm_n; i++)
		CHECK(board_lenok.therm[i].decidegrees > board_lenok.therm[i - 1].decidegrees &&
		      board_lenok.therm[i].mv < board_lenok.therm[i - 1].mv);
	CHECK(board_lenok.touch_pins.ldo_io == 22 && board_lenok.touch_pins.ldo == 15);
	/* Its motion chip has no hub: it is on the bus the ADSP had, with
	 * its interrupt a wake pin. */
	CHECK(board_lenok.hub == &hub_mpu6515 && board_lenok.hub_pins.i2c.bus == 2 && board_lenok.hub_pins.mpm == 14);
	/* Its motor takes GP0, where dory's takes GP1, and switches its own supply. */
	CHECK(board_lenok.motor == &motor_gp_pwm && board_lenok.motor_pins.pwm_gp0 && board_lenok.motor_pins.ldo == 28);
	CHECK(board_dory.motor == &motor_gp_pwm && !board_dory.motor_pins.pwm_gp0 && !board_dory.motor_pins.ldo);
	CHECK(board_lenok.gpios_low[0] == 46 && board_lenok.gpios_low[1] == 72 && !board_dory.gpios_low[0]);
	CHECK(board_dory.chg.vdd_max_mv == 4350 && board_dory.chg.ibat_max_ma == 400 && board_dory.chg.no_thermistor);
	CHECK(board_dory.gauge.model && board_dory.gauge.model->design_cap == 0x0320 && !board_dory.gauge.dock);
	for (unsigned i = 1; i < board_dory.therm_n; i++)
		CHECK(board_dory.therm[i].decidegrees > board_dory.therm[i - 1].decidegrees &&
		      board_dory.therm[i].mv < board_dory.therm[i - 1].mv);

	/* catfish's SMB231 setup ends in a full write of CFG8, and its cell
	 * numbers come from the hub. */
	const struct board_reg *r = board_catfish.charger.setup;
	unsigned n = board_catfish.charger.setup_n;
	CHECK(n == 8 && r[n - 1].reg == 0x08 && !r[n - 1].mask && r[n - 1].value == 0xd9);
	CHECK(board_catfish.charger.cell_mah == 415 && board_catfish.charger.irqs_awake == 0xbf &&
	      board_catfish.charger.irqs_asleep == 0x21 && board_catfish.charger.cmd1 == 2 && !board_catfish.fuel_gauge);

	/* swift: its own touch controller, none of catfish's hub or
	 * Bluetooth, and the vendor's power key reset. */
	CHECK(board_swift.touch == &touch_hx852x && !board_swift.touch_alt && board_swift.touch_pins.mpm == 10);
	CHECK(board_swift.light == &light_al3320 && board_swift.light_pins.i2c.bus == 1);
	/* Its LSM6DS3 is on SPI, apart from the touch and charger buses. */
	CHECK(board_swift.hub == &hub_lsm6ds3 && board_swift.hub_pins.spi == 3 && board_swift.hub_pins.cs == 2 &&
	      board_swift.hub_pins.mpm == 36);
	/* Its charger is set up as stock leaves it, with the cell's numbers
	 * from its STC3117. */
	CHECK(board_swift.battery_driver == &battery_smb231 && board_swift.fuel_gauge == &gauge_stc3117 &&
	      board_swift.gauge.cell && board_swift.gauge.i2c.bus == board_swift.charger.i2c.bus);
	/* narwhal's motion sensor sits where swift's does. */
	CHECK(board_narwhal.hub == &hub_icm40604 && board_narwhal.hub_pins.spi == board_swift.hub_pins.spi &&
	      board_narwhal.hub_pins.irq == board_swift.hub_pins.irq && board_narwhal.hub_pins.mpm == 36);
	/* narwhal's Bluetooth is catfish's UART with its own patch, and its
	 * address comes from the bootloader. */
	CHECK(board_narwhal.bt.uart == board_catfish.bt.uart && board_narwhal.bt.gpio == board_catfish.bt.gpio &&
	      board_narwhal.bt.addr_chosen && !board_narwhal.bt.addr_part && board_narwhal.bt.patch);
	/* narwhal's FAN54511 shares its bus with the MAX17050, which takes the
	 * temperature from the host and its cell model from the board. */
	CHECK(board_narwhal.battery_driver == &battery_fan54511 && board_narwhal.fuel_gauge == &gauge_max17050 &&
	      board_narwhal.gauge.model && board_narwhal.gauge.host_temp && board_narwhal.therm_n &&
	      board_narwhal.gauge.i2c.bus == board_narwhal.charger.i2c.bus && board_narwhal.charger.mpm == 33);
	static const uint8_t live[] = { 0xc4, 0x30, 0x01, 0x17, 0x12, 0x0d, 0x23, 0x08, 0x61 };
	uint8_t chip[9];
	memcpy(chip, live, sizeof chip);
	for (unsigned i = 0; i < board_swift.charger.setup_n; i++) {
		const struct board_reg *s = &board_swift.charger.setup[i];
		uint8_t v = s->mask ? (uint8_t)((chip[s->reg] & ~s->mask) | s->value) : s->value;
		CHECK(v == live[s->reg]);
	}
	CHECK(board_swift.charger.irqs_awake == 0xbb && board_swift.charger.cmd1 == 1 &&
	      board_swift.charger.cell_mah == 340 && board_swift.charger.hold_low == 17 &&
	      board_swift.charger.pmic_charger_off && !board_catfish.charger.pmic_charger_off);
	/* Only swift has the WCNSS, so only its UI heap starts after it. */
	CHECK(board_swift.bt.wcnss && !board_catfish.bt.wcnss && !board_smelt.bt.wcnss && !board_dory.bt.wcnss);
	CHECK(!board_swift.bt.patch && board_swift.key_reset_ms == 6720 && board_swift.motor == &motor_pm8226);
	CHECK(!board_catfish.key_reset_ms);
	/* Only swift, still being brought up, stops in fastboot after a crash. */
	CHECK(board_swift.crash_to_fastboot && !board_catfish.crash_to_fastboot && !board_dory.crash_to_fastboot &&
	      !board_smelt.crash_to_fastboot && !board_sturgeon.crash_to_fastboot && !board_sparrow.crash_to_fastboot);
	/* swift's AsteroidOS ramoops is in RAM that the firmware maps and
	 * leaves alone: between the UI heap and GPU memory. */
	CHECK(board_swift.aos_ramoops.phys == 0x8fe00000 && board_swift.aos_ramoops.size == 0x200000 &&
	      board_swift.aos_ramoops.phys - PHYS_OFFSET_8009 >= UI_HEAP_BASE + UI_HEAP_SIZE &&
	      board_swift.aos_ramoops.phys - PHYS_OFFSET_8009 + board_swift.aos_ramoops.size <= GPU_MEM_BASE);
	CHECK(!board_catfish.aos_ramoops.size && !board_dory.aos_ramoops.size && !board_smelt.aos_ramoops.size);
	CHECK(board_swift.panel.lp_commands && !board_catfish.panel.lp_commands && !board_dory.panel.lp_commands &&
	      !board_smelt.panel.lp_commands && !board_sturgeon.panel.lp_commands && !board_sparrow.panel.lp_commands);
	CHECK(board_swift.panel.brightness_levels && board_swift.panel.brightness_levels[6][0] == 8 &&
	      board_swift.panel.brightness_levels[20][20] == 100 && !board_catfish.panel.brightness_levels &&
	      !board_dory.panel.brightness_levels && !board_smelt.panel.brightness_levels &&
	      !board_sturgeon.panel.brightness_levels && !board_sparrow.panel.brightness_levels);
	/* Only swift's RESIN is a key, and catfish keeps its one gpio key. */
	CHECK(board_swift.resin_key && board_swift.keys[0].gpio == 90 && board_swift.keys[1].gpio == 91);
	CHECK(!board_catfish.resin_key && board_catfish.keys[0].gpio == 98 && !board_catfish.keys[1].gpio);
	CHECK(!board_sturgeon.resin_key && !board_dory.resin_key && !board_smelt.resin_key && !board_sparrow.resin_key);

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

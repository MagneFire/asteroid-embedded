/* What differs from watch to watch, as one table per watch
 * (board/<name>.c); board_detect picks the one the bootloader's device
 * tree names. Pins are TLMM gpio numbers; MPM pins are the MPM's
 * wake-up sources. */
#ifndef BOARD_H
#define BOARD_H
#include <stdbool.h>
#include <stdint.h>
#include "lib/ramoops.h"

struct dcs_step;
struct touch;
struct max17050_model;
struct stc3117_cell;
struct m4_variant;
struct tsl258x_profile;

/* An I2C device: its bus and, where the SoC routes them by hand, the pins
 * and their function. */
struct board_i2c {
	uint8_t bus, sda, scl, func, addr;
};

/* A cell the fuel gauge knows, told apart by the pack's ID resistor:
 * its default internal resistance and the scale factor in percent at
 * each percentage and temperature. */
struct battery_cell {
	const char *name;
	int id_kohm, rbatt_mohm;
	uint16_t rbatt_sf[31][5];
};

/* The cell's data, as the vendor's battery data: full charge at each
 * temperature and the open circuit voltage at each percentage and
 * temperature; the gauge's cutoff and the low voltage below which it
 * recalculates more often. */
struct board_battery {
	int8_t lut_temp[5];
	uint8_t lut_pc[31];
	uint16_t fcc_mah[5];
	uint16_t ocv_mv[31][5];
	int cutoff_uv, low_uv;
	const struct battery_cell *cells;
	unsigned ncells;
};

struct board_therm {
	int16_t decidegrees, mv;
};

/* A driver for one kind of chip; a watch without the part has none. */
struct touch_driver {
	bool (*init)(void);
	bool (*poll)(struct touch *t);
	void (*stats)(uint32_t *attentions, uint32_t *errors);
	void (*inject)(int x0, int y0, int x1, int y1, uint32_t ms);
	void (*sleep)(bool on, bool wakes);
	uint32_t (*next_ms)(void);
	/* Lets go of the controller for the application processor, or
	 * takes it back. */
	void (*lend)(bool to_ap);
};

struct hub_profile;

/* What a hub's fuel gauge last said about the cell. */
struct hub_gauge {
	uint32_t mv, percent;
	int32_t ua, decidegrees;
	uint32_t t_ms;		/* time_ms of the reading */
};

struct hub_driver {
	bool (*init)(void);
	void (*poll)(void);
	int (*send)(const uint8_t *p, unsigned n);
	void (*set_time)(void);
	uint32_t (*steps)(void);
	/* Steps since local midnight, kept by a hub that counts while the
	 * watch is off; HUB_NONE until it has said. */
	uint32_t (*steps_today)(void);
	void (*sync_steps_today)(uint32_t steps);
	/* Sends the steps it holds back now. */
	void (*flush_steps)(void);
	bool (*activity)(uint32_t v[2]);
	void (*profile)(const struct hub_profile *p);
	void (*heart_rate_on)(bool on);
	void (*passive_on)(bool on);
	unsigned (*passive)(uint32_t *t, uint8_t *bpm, unsigned max);
	uint32_t (*heart_rate)(uint8_t *status, unsigned *readings);
	bool (*tilted)(void);
	bool (*tilt_on)(bool on);
	void (*tilt_screen)(int state);
	void (*accel_on)(bool on);
	bool (*accel)(int16_t v[3]);
	bool (*can_sleep)(void);
	/* Around each cluster power-down. */
	void (*sleep)(bool asleep);
	/* The watch going to sleep or waking up, once per change. */
	void (*asleep)(bool asleep);
	void (*status)(void (*say)(const char *fmt, ...));
	/* A register of the hub, written then read, for the console. */
	int (*raw)(uint8_t bank, uint8_t offset, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn);
	int (*power_management)(bool on);
	/* A runtime setting of the driver, for the console; < 0 if it has
	 * no such setting. */
	int (*option)(const char *name, uint32_t v);
	/* Every sensor off, also the step counter, until powered up. */
	void (*power_down)(bool down);
	/* The watch's screen on or off, for a hub with a display of its own. */
	void (*screen)(bool on);
	bool (*gauge)(struct hub_gauge *g);
	/* Asks the gauge for a reading when one is due, or with now unless
	 * one was just asked for. */
	void (*poll_gauge)(bool now);
	/* Leave the hub showing the time on its own display, before the
	 * watch powers off. */
	void (*timepiece)(void);
	/* A workout for the hub's display while the screen is off: running
	 * with its time so far, distance and calories, or ended. */
	void (*workout)(bool running, uint32_t seconds, uint32_t distance_m, uint32_t kcal);
	/* Heart rate readings the hub logged with the watch off, oldest
	 * first; how many it gave. */
	unsigned (*heart_log)(uint32_t *t, uint8_t *bpm, unsigned max);
	/* One of the hub's display modes, for the console; < 0 if refused. */
	int (*mode)(unsigned code);
	/* A magnetometer for the Compass app: see hub_compass_on. */
	void (*compass_on)(bool on);
	int (*heading)(float *degrees);
};

/* A fuel gauge read by the AP, for a charger that does not measure the
 * cell: its start, a reading taken when one is due (or now), the last
 * one, and the watch going to sleep or waking up. */
struct gauge_driver {
	bool (*init)(void);
	void (*poll)(bool now);
	bool (*read)(struct hub_gauge *g);
	void (*asleep)(bool asleep);
};

struct motor_driver {
	bool (*init)(void);
	void (*set)(bool on);
	/* Lets go of the driver's lines for the application processor,
	 * also of a chip that did not answer, or takes them back and
	 * starts it again: whether it answers then. */
	bool (*lend)(bool to_ap);
};

/* A panel lit by a separate chip; level 0 turns it off. Without one the
 * panel takes its brightness over DSI. set_code writes the chip's own
 * brightness code, for a board with backlight_aod_code. */
struct backlight_driver {
	bool (*init)(void);
	void (*set)(uint8_t level);
	void (*set_code)(uint8_t code);
};

/* A light sensor read in two steps, so the integration can run while
 * something else does: start, then finish, which gives the lux or -1
 * without a reading. keep holds it powered between readings, so that a
 * start has nothing to wait for. command takes the console's light
 * command: with no arguments what went into the last reading. */
struct light_driver {
	bool (*init)(void);
	void (*start)(void);
	int32_t (*finish)(void);
	void (*keep)(bool on);
	void (*command)(const char *args, void (*say)(const char *fmt, ...));
};

/* What feeds the battery and how full it is: the charger's setup and
 * upkeep, the state of charge in percent, whether it is charging,
 * whether a cable is in (a plugged-in interrupt comes with cable_irq),
 * whether that means USB if it does not always, the cell's voltage and current (positive while discharging, the
 * average since the previous call) and a line for the log. */
struct battery_driver {
	void (*init)(void);
	void (*poll)(void);
	uint32_t (*next_ms)(void);
	uint32_t (*percent)(void);
	bool (*charging)(void);
	bool (*cable)(void);
	bool (*usb)(void);
	void (*cable_irq_init)(void);
	bool (*cable_irq)(void);
	void (*asleep)(bool asleep);
	uint32_t (*mv)(void);
	int32_t (*ua)(void);
	int32_t (*avg_ua)(void);
	/* Without avg_ua: how old the ua reading is, UINT32_MAX for none. */
	uint32_t (*ua_age_ms)(void);
	const char *(*status)(void);
};

extern const struct battery_driver battery_pmic, battery_max17050, battery_pmic_max17050, battery_smb231, battery_adc;
extern const struct touch_driver touch_rmi4, touch_it7260, touch_mxt, touch_fts, touch_hx852x, touch_tma525;
extern const struct hub_driver hub_stm32, hub_em7184, hub_m4, hub_nanohub, hub_lis3dsh, hub_lsm6ds3, hub_lsm6dso;
extern const struct motor_driver motor_drv2605, motor_pm8226, motor_ldo, motor_gp_pwm, motor_aw8624;
extern const struct backlight_driver backlight_lm3535, backlight_lm3530;
extern const struct light_driver light_tsl2584, light_tsl258x, light_m4, light_nanohub, light_al3320;
extern const struct gauge_driver gauge_stc3117;

/* A register write: the bits of mask set to value, or with a mask of 0
 * the whole register. */
struct board_reg {
	uint8_t reg, mask, value;
};

/* A pad whose mux or pull the bootloader's setup is changed in: its
 * offset in the SoC's pad registers and their value. */
struct board_pad {
	uint16_t off, value;
};

/* A microcontroller's pad set at boot: its number, function and the
 * SoC's PAD_* flags; a function of 0xff ends the list. */
struct board_mcu_pad {
	uint8_t pad, func;
	uint16_t flags;
};

/* A register write that puts a chip the firmware leaves alone to sleep:
 * reg = (reg & ~clear) | set, unread when clear is 0xff; then wait_ms
 * and read clear_irq unless it is 0xff. */
struct board_chip_off {
	uint8_t addr, reg, clear, set, wait_ms, clear_irq;
};

struct board {
	const char *name, *compatible;
	/* Pads to set at boot, ended by an offset of 0, for a SoC that
	 * has the pins muxed apart from their use. */
	const struct board_pad *pads;
	const struct board_mcu_pad *mcu_pads;
	/* Chips that keep running across a reset of this SoC, put to
	 * sleep at boot; the list ends with addr 0. */
	struct {
		struct board_i2c i2c;
		const struct board_chip_off *regs;
	} chips_off;
	/* The pads that can wake such a SoC, which a driver names by its
	 * place here where another watch has an MPM pin. */
	const uint16_t *wake_pads;
	uint8_t wake_pad_count;
	/* SMEM's hardware platform, where it is enough to name the watch,
	 * and its subtype, where that tells watches of one SoC apart. */
	uint32_t smem_platform, smem_subtype;
	bool heart_rate;

	struct {
		uint16_t width, height;
		uint16_t height_alt;	/* a second size of the same panel, or 0 */
		uint16_t lines;		/* with the porches */
		uint8_t hz, te_gpio;
		bool flip_ud, round;
		/* An LCD needs no burn-in protection. */
		bool lcd;
		/* Sleep, wake, display on and idle commands in low power mode,
		 * as the vendor sends them. */
		bool lp_commands;
		/* Automatic brightness levels of the panel's own, 21 curves of
		 * 21 steps, or NULL for the default ones. */
		const uint8_t (*brightness_levels)[21];
		/* How far from an edge a drag starts from it, 0 for the default. */
		uint8_t edge_px;
		/* For the panel of the first height only. */
		const struct dcs_step *idle_on, *idle_off;
		uint8_t idle_on_n, idle_off_n;
		/* For a panel that keeps its power: commands before each
		 * sleep out, as it can lose them asleep. */
		const struct dcs_step *wake;
		uint8_t wake_n;
		/* A panel switched off with the screen: its commands from
		 * reset to out of sleep, its reset line, the line that
		 * enables its bias supply and the LDO for its logic. */
		const struct dcs_step *on;
		uint8_t on_n, reset_gpio, bias_gpio, io_ldo;
		uint32_t io_uv;
		/* A panel behind a bridge chip: the bridge's reset line, the
		 * lines that switch its core supply, its I/O supply (low is
		 * on, and the pad of that line) and its memory. */
		uint8_t bridge_reset, bridge_supply, vio_gpio, mem_gpio;
		uint16_t vio_pad;
		/* A panel on SPI: the bus, its chip select and rate, the
		 * data/command line, the lines that switch it on, the
		 * column its picture starts at and the bytes its ID reads. */
		uint8_t spi_bus, spi_cs, spi_mode, dc_gpio, enable_gpio[2];
		uint32_t spi_hz;
		/* The bus's pads, let go of while the application processor
		 * has the panel. */
		uint8_t spi_pads[4];
		uint16_t col_offset;
		uint8_t id[3];
	} panel;
	/* The line that switches a clock the panel's bridge and the USB
	 * PHY share, 0 for none. */
	uint8_t periph_clock_gpio;

	/* The names of the partitions of a disk whose table has none, by
	 * number from 1, "" for one the firmware has no name for. */
	const char *const *part_names;
	uint8_t part_count;
	/* Where firmware files are on the system partition, NULL-ended. */
	const char *const *firmware_dirs;
	const char *firmware_part;	/* where they are, "system" if not set */

	struct {
		uint8_t uart, gpio, func, dev_wake, host_wake, reg_on, mpm;
		bool dev_wake_active, host_wake_active;
		/* The chip's I/O supply when the firmware has to switch it, 0 if not. */
		uint8_t ldo;
		uint32_t ldo_uv;
		/* A reset line, low holds the chip in reset, 0 for none. */
		uint8_t reset;
		/* Where the chip's ROM gets its UART rates wrong (it assumes another
		 * crystal), the host rates its 115200 and 3000000 really are; 0 if
		 * they are right. */
		uint32_t rom_baud, rom_fast;
		/* Where the factory wrote the address, as xx:xx:xx:xx:xx:xx
		 * text, if anywhere. */
		const char *addr_part, *addr_path;
		const char *patch;
		uint8_t sleep_mode[12];	/* the 0xfc27 parameters */
		/* The SoC's own WCNSS core instead of a chip. */
		bool wcnss;
	} bt;

	/* The PMIC's interrupt line (power key, charger) as an MPM pin. */
	uint8_t pmic_mpm;
	/* Holding the power key this long resets the watch, as the vendor
	 * sets it; 0 leaves what the bootloader set. */
	uint16_t key_reset_ms;
	/* Any reset but a deliberate one stops in fastboot, so the log in
	 * RAM is still there for the next boot of this firmware instead of
	 * being overwritten by the watch's own system; for a watch still
	 * being brought up. */
	bool crash_to_fastboot;
	/* Where the ramoops of its AsteroidOS kernel is, when that is not
	 * where this log lives: each reset copies the log and any fault
	 * record there, so that pstore under AsteroidOS shows them. */
	struct ramoops_layout aos_ramoops;

	struct {
		uint16_t vdd_max_mv, vdd_safe_mv, vin_min_mv, ibat_max_ma, ibat_safe_ma, iterm_ma;
		/* A cell without a thermistor: the charger's hot and cold
		 * comparators are switched off, or they would stop it. */
		bool no_thermistor;
	} chg;
	/* The battery current sense resistor. */
	uint32_t rsense_uohm;
	/* USB PHY override registers 0x80-0x83, written after a PHY reset. */
	uint8_t usb_phy_init[4];	/* up to the first 0 */
	/* A USB PHY run by lines alone: high while VBUS is there, its
	 * reset (low holds it), and its two selects of opposite sense. */
	struct {
		uint8_t vbus, resetn, cs, csn;
	} usb_phy;
	/* The PHY's L10 and L20 go off with USB down. */
	bool usb_rails_off;
	/* LDOs the bootloader leaves on that nothing here uses, 0 ends. */
	uint8_t unused_ldos[4];
	const struct board_battery *battery;
	const struct battery_driver *battery_driver;
	/* The cell's thermistor at the PMIC's ADC, in tenths of a degree
	 * against mV and with mV falling, where it is not sturgeon's. */
	const struct board_therm *therm;
	uint8_t therm_n;
	/* The battery on an ADC channel: its pad and function, and the
	 * mV a full-scale reading stands for through the divider. */
	struct {
		uint8_t pad, func, channel;
		uint16_t mv_full;
	} battery_adc;
	/* Keys on GPIOs, pressed low: the power key and a second one, 0
	 * for none. */
	uint8_t key_gpio[2];
	/* Pads another chip drives too, kept inputs whatever asks: 0 for
	 * none. */
	uint8_t input_only[4];
	/* A MAX17050 gauge (or the fuel gauge's bus), the wireless pad line (low on the pad), the
	 * receiver's charge output and its termination (high stops charging)
	 * and reduced rate inputs. */
	struct {
		struct board_i2c i2c;
		uint8_t dock, dock_mpm, charge_n, charge_term, charge_rate;
		/* A receiver that pulses the pad line while it does not
		 * charge, and its line that is high while it does. */
		bool dock_pulses;
		uint8_t charging;
		/* Motorola's factory kill: high keeps the battery connected while
		 * USB is in, 0 for a board without it. */
		uint8_t kill_disable;
		/* The cell model to load when the gauge has lost it. */
		const struct max17050_model *model;
		/* An STC3117's cell. */
		const struct stc3117_cell *cell;
		/* USB power reaches the gauge's side of the cell, so what
		 * it learned with USB in is thrown away when USB goes. */
		bool usb_disturbs;
	} gauge;

	/* Other keys, low while pressed, that do what the power key does,
	 * and whether the PMIC's RESIN input is one. */
	struct {
		uint8_t gpio, mpm;
	} keys[2];
	bool resin_key;

	/* A charger of its own, with the cell's numbers from fuel_gauge or
	 * the hub: its setup, the interrupts enabled
	 * awake and asleep, its input mode and the cell's size. A line held
	 * low for it, 0 for none, and the PMIC's own charger kept off. A
	 * cell measured at ov_mv while charging has its float voltage
	 * lowered to ov_float_mv until the cable goes, 0 for none. With
	 * empty_mv the watch powers off after readings at or below it
	 * instead of at 0 %. */
	struct {
		struct board_i2c i2c;
		uint8_t irq, mpm, suspend;
		const struct board_reg *setup;
		uint8_t setup_n, irqs_awake, irqs_asleep, cmd1;
		uint16_t cell_mah;
		uint8_t hold_low;
		bool pmic_charger_off;
		uint16_t ov_mv, ov_float_mv, empty_mv;
	} charger;
	const struct gauge_driver *fuel_gauge;

	/* Some units of a watch have this controller instead, at alt_addr. */
	const struct touch_driver *touch, *touch_alt;
	struct {
		struct board_i2c i2c;
		uint8_t alt_addr;
		/* The controller's coordinate range, 0 for the driver's. */
		uint16_t range;
		uint8_t irq, reset, mpm;
		/* The LDO a driver switches on for it and its voltage, 0 for none. */
		uint8_t ldo;
		uint32_t ldo_uv;
		/* A second LDO, for the controller's I/O, and a line held high while it is in use. */
		uint8_t ldo_io, power;
		uint32_t ldo_io_uv;
		/* The controller holds this watch's settings for awake
		 * after its reset, instead of the driver's. */
		bool own_settings;
		/* An F12 object wider than this is a palm, 0 for only its
		 * own palm type. */
		uint8_t palm_width;
		/* A line that switches its supply, 0 for none. */
		uint8_t power_gpio;
		/* The controller's grid when it is not the panel's, scaled
		 * to the panel: 0 for the panel's. */
		uint16_t grid_w, grid_h;
	} touch_pins;

	const struct hub_driver *hub;
	struct {
		/* An I2C hub (EM7184, M4): the bus and its interrupt. */
		struct board_i2c i2c;
		uint8_t irq;
		/* A UART hub (STM32): the port and its control lines. */
		uint8_t uart, gpio, func, wake_mcu, mcu_wake, reset, boot0, boot1, ap_status, mcu_status;
		uint8_t mpm;
		/* A line of the hub's that is kept low. */
		uint8_t hold_low;
		/* A supply the hub needs on before it starts, 0 for none. */
		uint8_t ldo;
		uint32_t ldo_uv;
		/* An SPI hub: its QUP and chip select, and the mode lines with
		 * their strobe and the hub's request for them. */
		uint8_t spi, cs, mode_irq, mode_mpm, strobe, mode[4];
	} hub_pins;
	/* Which of the M4 hub's firmwares the watch has. */
	const struct m4_variant *hub_variant;
	/* A heart rate front end on SPI beside the hub: the bus, chip
	 * select and rate, the pads set while it is on (ended by a
	 * function of 0xff), its supply, reset and data ready lines. */
	struct {
		const struct board_mcu_pad *pads;
		uint8_t spi_bus, spi_cs, power, reset, irq;
		uint32_t spi_hz;
	} hr_pins;

	const struct backlight_driver *backlight;
	struct {
		struct board_i2c i2c;
		uint8_t enable;
	} backlight_pins;
	/* The backlight code for the always-on face, 0 for the usual
	 * percentage. */
	uint8_t backlight_aod_code;
	/* The brightness setting until one is chosen, 0 for 100 per cent. */
	uint8_t brightness_init;

	const struct light_driver *light;
	struct {
		struct board_i2c i2c;
		/* The LDO that feeds it and its voltage. */
		uint8_t ldo;
		uint32_t ldo_uv;
		/* TSL2584 lux coefficients for the panel's height and for
		 * height_alt: lux1's channels 0 and 1, then lux2's. */
		uint16_t coeff[2][4];
		/* The TSL258x's size of watch. */
		const struct tsl258x_profile *profile;
	} light_pins;

	const struct motor_driver *motor;
	struct {
		struct board_i2c i2c;	/* DRV2605 */
		uint8_t trigger, enable;
		uint8_t rated, clamp;	/* DRV2605, 0 for sturgeon's */
		uint16_t vib_mv;	/* PMIC vibrator drive */
		/* A motor fed by an LDO of its own, and that LDO's voltage. */
		uint8_t ldo;
		uint32_t ldo_uv;
		/* A motor driven by a general purpose clock's duty cycle: its
		 * pin and function, the clock's N and the drive in percent. */
		uint8_t pwm_gpio, pwm_func, pwm_n, pwm_percent;
	} motor_pins;

	/* The speaker amplifier, addr 0 if there is none. */
	struct board_i2c speaker;

	/* A microcontroller's link to the application processor beside it:
	 * the request and acknowledge lines up to the AP and down from it,
	 * up_int 0 for none. */
	struct {
		uint8_t up_int, up_ack, dn_int, dn_ack;
		/* High while touch and haptics are ours, low while the AP has
		 * them; and a line let go of while the AP has the panel. 0 for
		 * none. */
		uint8_t own_gpio, release_gpio;
	} ap_link;
	/* Low while the BLE radio is on, high otherwise: 0 for none. */
	uint8_t ble_gpio;
};

/* The charger limits have to land on a register step: 10 mV from 3.24 V
 * for the voltages, 50 mV from 4.0 V for the input minimum and 50 mA for
 * the currents. Each board table checks its own. */
#define BOARD_CHG_CHECK(vdd_max, vdd_safe, vin_min, ibat_max, ibat_safe)                                         \
	_Static_assert((vdd_max) >= 3240 && (vdd_max) % 10 == 0 && (vdd_safe) >= (vdd_max) && (vdd_safe) % 10 == 0, \
		       "charger voltage off the 10 mV steps");                                                    \
	_Static_assert((vin_min) >= 4000 && (vin_min) % 50 == 0 && 0x0c + ((vin_min) - 4000) / 50 <= 0x3f,      \
		       "charger input minimum off the 50 mV steps");                                              \
	_Static_assert((ibat_max) % 50 == 0 && (ibat_max) / 50 <= 0x3f && (ibat_safe) % 50 == 0 &&              \
			       (ibat_safe) / 50 <= 0x3f && (ibat_max) <= (ibat_safe),                             \
		       "charger current off the 50 mA steps")

extern const struct board board_sturgeon, board_sparrow, board_smelt, board_catfish, board_minnow, board_dory, board_swift, board_beluga, board_beluga_41;
/* The watch this is, set by board_detect before anything uses it. */
extern const struct board *board;
extern const char *board_detected_by;
/* Nothing named the watch and board is only a guess. */
extern bool board_assumed;
/* Picks the watch from what the bootloader passed in r1 and r2. */
void board_detect(uint32_t machtype, const void *fdt);
#endif

/* What differs from watch to watch, as one table per watch
 * (board/<name>.c); board_detect picks the one the bootloader's device
 * tree names. Pins are TLMM gpio numbers; MPM pins are the MPM's
 * wake-up sources. */
#ifndef BOARD_H
#define BOARD_H
#include <stdbool.h>
#include <stdint.h>

struct dcs_step;
struct touch;

/* An I2C device on a BLSP1 QUP. */
struct board_i2c {
	uint8_t qup, sda, scl, func, addr;
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

/* A driver for one kind of chip; a watch without the part has none. */
struct touch_driver {
	bool (*init)(void);
	bool (*poll)(struct touch *t);
	void (*stats)(uint32_t *attentions, uint32_t *errors);
	void (*inject)(int x0, int y0, int x1, int y1, uint32_t ms);
	void (*sleep)(bool on);
	uint32_t (*next_ms)(void);
};

struct hub_driver {
	bool (*init)(void);
	void (*poll)(void);
	int (*send)(const uint8_t *p, unsigned n);
	void (*set_time)(void);
	uint32_t (*steps)(void);
	void (*heart_rate_on)(bool on);
	uint32_t (*heart_rate)(uint8_t *status, unsigned *readings);
	bool (*tilted)(void);
	void (*accel_on)(bool on);
	bool (*accel)(int16_t v[3]);
	bool (*can_sleep)(void);
	void (*sleep)(bool asleep);
	void (*status)(void (*say)(const char *fmt, ...));
};

struct motor_driver {
	bool (*init)(void);
	void (*set)(bool on);
};

/* A panel lit by a separate chip; level 0 turns it off. Without one the
 * panel takes its brightness over DSI. */
struct backlight_driver {
	bool (*init)(void);
	void (*set)(uint8_t level);
};

/* What feeds the battery and how full it is: the charger's setup and
 * upkeep, the state of charge in percent, whether it is charging,
 * whether a cable is in (a plugged-in interrupt comes with cable_irq),
 * whether that means USB if it does not always, the cell's voltage and current (positive while discharging, the
 * average since the previous call) and a line for the log. */
struct battery_driver {
	void (*init)(void);
	void (*poll)(void);
	uint32_t (*percent)(void);
	bool (*charging)(void);
	bool (*cable)(void);
	bool (*usb)(void);
	void (*cable_irq_init)(void);
	bool (*cable_irq)(void);
	uint32_t (*mv)(void);
	int32_t (*ua)(void);
	int32_t (*avg_ua)(void);
	const char *(*status)(void);
};

extern const struct battery_driver battery_pmic, battery_max17050;
extern const struct touch_driver touch_rmi4, touch_it7260, touch_mxt;
extern const struct hub_driver hub_stm32, hub_em7184, hub_m4;
extern const struct motor_driver motor_drv2605, motor_pm8226;
extern const struct backlight_driver backlight_lm3535;

struct board {
	const char *name, *compatible;
	/* SMEM's hardware platform, where it is enough to name the watch. */
	uint32_t smem_platform;
	bool heart_rate;

	struct {
		uint16_t width, height;
		uint16_t height_alt;	/* a second size of the same panel, or 0 */
		uint16_t lines;		/* with the porches */
		uint8_t hz, te_gpio;
		bool flip_ud, round;
		const struct dcs_step *idle_on, *idle_off;
		uint8_t idle_on_n, idle_off_n;
	} panel;

	/* Where firmware files are on the system partition, NULL-ended. */
	const char *const *firmware_dirs;

	struct {
		uint8_t uart, gpio, func, dev_wake, host_wake, reg_on, mpm;
		bool dev_wake_active, host_wake_active;
		const char *patch;
		uint8_t sleep_mode[12];	/* the 0xfc27 parameters */
	} bt;

	/* The PMIC's interrupt line (power key, charger) as an MPM pin. */
	uint8_t pmic_mpm;

	struct {
		uint16_t vdd_max_mv, vdd_safe_mv, vin_min_mv, ibat_max_ma, ibat_safe_ma, iterm_ma;
	} chg;
	/* The battery current sense resistor. */
	uint32_t rsense_uohm;
	/* USB PHY override registers 0x80-0x83, written after a PHY reset. */
	uint8_t usb_phy_init[4];
	/* The PHY's L10 and L20 go off with USB down. */
	bool usb_rails_off;
	/* LDOs the bootloader leaves on that nothing here uses, 0 ends. */
	uint8_t unused_ldos[4];
	const struct board_battery *battery;
	const struct battery_driver *battery_driver;
	/* A MAX17050 gauge, the wireless pad line (low on the pad) and the
	 * receiver's charge output. */
	struct {
		struct board_i2c i2c;
		uint8_t dock, dock_mpm, charge_n;
	} gauge;

	const struct touch_driver *touch;
	struct {
		struct board_i2c i2c;
		uint8_t irq, reset, mpm;
	} touch_pins;

	const struct hub_driver *hub;
	struct {
		/* An I2C hub (EM7184, M4): the bus and its interrupt. */
		struct board_i2c i2c;
		uint8_t irq;
		/* A UART hub (STM32): the port and its control lines. */
		uint8_t uart, gpio, func, wake_mcu, mcu_wake, reset, boot0, boot1, ap_status, mcu_status;
		uint8_t mpm;
	} hub_pins;

	const struct backlight_driver *backlight;
	struct {
		struct board_i2c i2c;
		uint8_t enable;
	} backlight_pins;

	const struct motor_driver *motor;
	struct {
		struct board_i2c i2c;	/* DRV2605 */
		uint8_t trigger, enable;
		uint16_t vib_mv;	/* PMIC vibrator drive */
	} motor_pins;

	/* The speaker amplifier, addr 0 if there is none. */
	struct board_i2c speaker;
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

extern const struct board board_sturgeon, board_sparrow, board_smelt;
/* The watch this is, set by board_detect before anything uses it. */
extern const struct board *board;
extern const char *board_detected_by;
/* Nothing named the watch and board is only a guess. */
extern bool board_assumed;
/* Picks the watch from what the bootloader passed in r1 and r2. */
void board_detect(uint32_t machtype, const void *fdt);
#endif

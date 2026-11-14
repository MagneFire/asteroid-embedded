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
struct max17050_model;
struct m4_variant;

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

/* A driver for one kind of chip; a watch without the part has none. */
struct touch_driver {
	bool (*init)(void);
	bool (*poll)(struct touch *t);
	void (*stats)(uint32_t *attentions, uint32_t *errors);
	void (*inject)(int x0, int y0, int x1, int y1, uint32_t ms);
	void (*sleep)(bool on);
	uint32_t (*next_ms)(void);
};

struct hub_profile;

/* What a hub's fuel gauge last said about the cell. */
struct hub_gauge {
	uint32_t mv, percent;
	int32_t ua, decidegrees;
};

struct hub_driver {
	bool (*init)(void);
	void (*poll)(void);
	int (*send)(const uint8_t *p, unsigned n);
	void (*set_time)(void);
	uint32_t (*steps)(void);
	bool (*activity)(uint32_t v[2]);
	void (*profile)(const struct hub_profile *p);
	void (*heart_rate_on)(bool on);
	void (*passive_on)(bool on);
	unsigned (*passive)(uint32_t *t, uint8_t *bpm, unsigned max);
	uint32_t (*heart_rate)(uint8_t *status, unsigned *readings);
	bool (*tilted)(void);
	bool (*tilt_on)(bool on);
	void (*accel_on)(bool on);
	bool (*accel)(int16_t v[3]);
	bool (*can_sleep)(void);
	void (*sleep)(bool asleep);
	void (*status)(void (*say)(const char *fmt, ...));
	/* A register of the hub, written then read, for the console. */
	int (*raw)(uint8_t bank, uint8_t offset, const uint8_t *w, unsigned wn, uint8_t *r, unsigned rn);
	/* The watch's screen on or off, for a hub with a display of its own. */
	void (*screen)(bool on);
	bool (*gauge)(struct hub_gauge *g);
	void (*poll_gauge)(void);
	/* Leave the hub showing the time on its own display, before the
	 * watch powers off. */
	void (*timepiece)(void);
	/* One of the hub's display modes, for the console; < 0 if refused. */
	int (*mode)(unsigned code);
};

struct motor_driver {
	bool (*init)(void);
	void (*set)(bool on);
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
 * start has nothing to wait for. */
struct light_driver {
	bool (*init)(void);
	void (*start)(void);
	int32_t (*finish)(void);
	void (*keep)(bool on);
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
	uint32_t (*mv)(void);
	int32_t (*ua)(void);
	int32_t (*avg_ua)(void);
	const char *(*status)(void);
};

extern const struct battery_driver battery_pmic, battery_max17050, battery_smb231;
extern const struct touch_driver touch_rmi4, touch_it7260, touch_mxt, touch_fts;
extern const struct hub_driver hub_stm32, hub_em7184, hub_m4, hub_nanohub;
extern const struct motor_driver motor_drv2605, motor_pm8226, motor_ldo;
extern const struct backlight_driver backlight_lm3535;
extern const struct light_driver light_tsl2584, light_m4;

/* A pad whose mux or pull the bootloader's setup is changed in: its
 * offset in the SoC's pad registers and their value. */
struct board_pad {
	uint16_t off, value;
};

struct board {
	const char *name, *compatible;
	/* Pads to set at boot, ended by an offset of 0, for a SoC that
	 * has the pins muxed apart from their use. */
	const struct board_pad *pads;
	/* The pads that can wake such a SoC, which a driver names by its
	 * place here where another watch has an MPM pin. */
	const uint16_t *wake_pads;
	uint8_t wake_pad_count;
	/* SMEM's hardware platform, where it is enough to name the watch. */
	uint32_t smem_platform;
	bool heart_rate;

	struct {
		uint16_t width, height;
		uint16_t height_alt;	/* a second size of the same panel, or 0 */
		uint16_t lines;		/* with the porches */
		uint8_t hz, te_gpio;
		bool flip_ud, round;
		/* An LCD needs no burn-in protection. */
		bool lcd;
		/* How far from an edge a drag starts from it, 0 for the default. */
		uint8_t edge_px;
		/* For the panel of the first height only. */
		const struct dcs_step *idle_on, *idle_off;
		uint8_t idle_on_n, idle_off_n;
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
		/* Where the chip's ROM gets its UART rates wrong (it assumes another
		 * crystal), the host rates its 115200 and 3000000 really are; 0 if
		 * they are right. */
		uint32_t rom_baud, rom_fast;
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
	/* A MAX17050 gauge, the wireless pad line (low on the pad), the
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
		/* USB power reaches the gauge's side of the cell, so what
		 * it learned with USB in is thrown away when USB goes. */
		bool usb_disturbs;
	} gauge;

	/* A second key, low while pressed, that does what the power key does. */
	struct {
		uint8_t gpio, mpm;
	} key2;

	/* A charger of its own, with the cell's numbers from the hub. */
	struct {
		struct board_i2c i2c;
		uint8_t irq, mpm, suspend;
	} charger;

	const struct touch_driver *touch;
	struct {
		struct board_i2c i2c;
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
		uint8_t spi, cs, mode_irq, strobe, mode[4];
	} hub_pins;
	/* Which of the M4 hub's firmwares the watch has. */
	const struct m4_variant *hub_variant;

	const struct backlight_driver *backlight;
	struct {
		struct board_i2c i2c;
		uint8_t enable;
	} backlight_pins;
	/* The backlight code for the always-on face, 0 for the usual
	 * percentage. */
	uint8_t backlight_aod_code;

	const struct light_driver *light;
	struct {
		struct board_i2c i2c;
		/* The LDO that feeds it and its voltage. */
		uint8_t ldo;
		uint32_t ldo_uv;
		/* TSL2584 lux coefficients for the panel's height and for
		 * height_alt: lux1's channels 0 and 1, then lux2's. */
		uint16_t coeff[2][4];
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

extern const struct board board_sturgeon, board_sparrow, board_smelt, board_catfish, board_minnow;
/* The watch this is, set by board_detect before anything uses it. */
extern const struct board *board;
extern const char *board_detected_by;
/* Nothing named the watch and board is only a guess. */
extern bool board_assumed;
/* Picks the watch from what the bootloader passed in r1 and r2. */
void board_detect(uint32_t machtype, const void *fdt);
#endif

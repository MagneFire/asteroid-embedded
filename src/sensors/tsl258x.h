/* beluga's TSL258x light sensor: its counts as lux, the gain for the
 * next reading and the screen's own light over it, no hardware
 * involved. */
#ifndef TSL258X_H
#define TSL258X_H
#include <stdbool.h>
#include <stdint.h>

/* 1x for bright light, 111x for the rest, the vendor's only gain. */
enum tsl258x_gain { TSL258X_GAIN_1X, TSL258X_GAIN_111X };

/* 2.7 ms cycles in an integration, the vendor's ITIME of 0xdb. */
#define TSL258X_CYCLES	37

/* What one size of watch sees of its own screen and how it turns
 * counts into lux, from the vendor's WearOS sensor service. */
struct tsl258x_profile {
	/* The screen over the sensor in panel pixels: left, top, and
	 * right and bottom just past it. */
	uint16_t crop[4];
	/* Channel 0 counts from red, green, blue and the grey of all
	 * three: x, x² and x³ of the colour's 0-255 value, at the full
	 * colour calibration of ref. */
	float poly[4][3];
	uint16_t ref[4];
	float grey[3];
	/* Lux per count by channel 0 over channel 1: from 8 up (and for
	 * dim light), from 4.5, from 3, and below. */
	float k[4];
	/* Whether each colour has its own curve for the panel's
	 * brightness rather than a straight line. */
	bool colour_curves;
};

extern const struct tsl258x_profile tsl258x_46mm, tsl258x_41mm;

/* The watch's own calibration: what full red, green, blue and white
 * screens give, and the gain in per cent. */
struct tsl258x_cal {
	uint16_t rgbw[4];
	uint16_t gain_pct;
};

/* The size of the calibration record. */
#define TSL258X_CAL_SIZE 20

/* Whether the ID register names a TSL258x. */
bool tsl258x_id_ok(uint8_t id);
/* The calibration from the vendor's factory record: false and the
 * vendor's defaults for one that does not look right. */
bool tsl258x_cal_read(struct tsl258x_cal *cal, const uint8_t rec[TSL258X_CAL_SIZE]);
/* The counts at 111x the screen adds to channel 0: rgb is what crop
 * shows, level the panel's brightness code. */
float tsl258x_screen_counts(const struct tsl258x_profile *p, const struct tsl258x_cal *cal, const uint8_t rgb[3],
			    uint8_t level);
/* The lux for counts c0 and c1 at gain with screen counts at 111x
 * taken off channel 0. */
int32_t tsl258x_lux(const struct tsl258x_profile *p, const struct tsl258x_cal *cal, enum tsl258x_gain gain, uint16_t c0,
		    uint16_t c1, float screen, uint8_t level);
/* The gain for the next reading after a count of c0. */
enum tsl258x_gain tsl258x_next_gain(enum tsl258x_gain gain, uint16_t c0);

/* What the panel shows in a profile's crop: the display hands over
 * each line it sends, and rgb gives the crop's colour. The crop keeps
 * what was last drawn of it, so a partial redraw leaves the rest. */
void tsl258x_crop_set(const struct tsl258x_profile *p);
void tsl258x_crop_line(uint32_t y, uint32_t x, uint32_t w, const uint16_t *rgb565);
void tsl258x_crop_rgb(uint8_t rgb[3]);

/* The last reading, for passing on: the lux, the counts and their gain,
 * channel 0 at 111x and the screen's share of it, and what that came
 * from. */
struct tsl258x_reading {
	int32_t lux;
	uint16_t c0, c1;
	enum tsl258x_gain gain;
	uint32_t c0_111x, c1_111x;
	float screen;
	uint8_t rgb[3], level;
	uint32_t at_ms;
};

/* The light sensor driver's last reading: false before the first. */
bool light_tsl258x_reading(struct tsl258x_reading *r);

/* In the driver: a second user of the readings, the AP's, which keeps
 * the sensor powered like light_keep while either wants it. */
void tsl258x_share(bool on);
#endif

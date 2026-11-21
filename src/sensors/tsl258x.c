/* The vendor's WearOS turns both channels into lux in its sensor
 * service (OppoFusionLight): it predicts what the AMOLED above the
 * sensor adds to channel 0 from the colour of a small crop of the
 * screen and the panel's brightness, takes that off, picks the lux per
 * count by what is left over channel 1 and applies the watch's gain.
 * Its counts are always at 111x; a 1x count here is taken as 111 times
 * that, and only a count full at its own gain skips the ratio.
 *
 * At 111x the count fills up at about 12700 lux. Above four fifths of
 * that the next reading is at 1x, and back at 111x once 1x reads what
 * 111x would see as 22200. */
#include <string.h>
#include "tsl258x.h"

#define ID_MASK		0xf0
#define ID_TSL258X	0x90

#define HIGH_FULL	30000
#define LOW_EMPTY	200

/* Channel 0 above which the vendor skips the ratio, just under full. */
#define SATURATED	37885
/* Corrected counts up to which the vendor keeps the first lux per
 * count. */
#define DIM_COUNTS	1000

static const int32_t gains[] = { 1, 111 };

/* The crops are the vendor's SurfaceFlinger rectangles. Its frame is
 * the panel window the AP sets, columns 40 to 441 on the 46 mm watch
 * and 20 to 339 on the 41 mm one, the same as the one here, so they are
 * taken as they are. */
const struct tsl258x_profile tsl258x_46mm = {
	.crop = { 20, 255, 70, 305 },
	.poly = { { 0.1012f, 0.0018f, 0.000002f }, { -0.0528f, 0.0062f, 0.000010f }, { -0.0198f, 0.0018f, 0.000002f },
		  { -0.1586f, 0.0006f, 0.000003f } },
	.ref = { 180, 573, 150, 48 },
	.grey = { 0.233f, 0.606f, 0.161f },
	.k = { 0.307821f, 0.224090f, 0.164646f, 0.115889f },
};

const struct tsl258x_profile tsl258x_41mm = {
	.crop = { 150, 5, 185, 35 },
	.poly = { { 1.0972f, -0.0010f, 0.000010f }, { 2.0635f, 0.0066f, 0.000010f }, { 0.2163f, 0.0001f, 0.000004f },
		  { -0.5701f, 0.0036f, -0.000003f } },
	.ref = { 377, 1139, 133, 39 },
	.grey = { 0.246f, 0.683f, 0.071f },
	.k = { 0.234730f, 0.182080f, 0.178180f, 0.113120f },
	.colour_curves = true,
};

/* The vendor's defaults for a watch without its factory record. */
static const struct tsl258x_cal cal_default = { { 216, 611, 128, 257 }, 100 };

bool tsl258x_id_ok(uint8_t id)
{
	return (id & ID_MASK) == ID_TSL258X;
}

static uint32_t le32(const uint8_t *b)
{
	return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

/* The limits are the ones the vendor's WearOS puts on its copy. */
bool tsl258x_cal_read(struct tsl258x_cal *cal, const uint8_t rec[TSL258X_CAL_SIZE])
{
	struct tsl258x_cal c;
	for (unsigned i = 0; i < 4; i++) {
		uint32_t v = le32(rec + 4 * i);
		if (v < 2 || v > 4999) {
			*cal = cal_default;
			return false;
		}
		c.rgbw[i] = (uint16_t)v;
	}
	uint32_t bits = le32(rec + 16);
	float gain;
	memcpy(&gain, &bits, sizeof gain);
	if (!(gain >= 0.1f && gain <= 20.0f)) {
		*cal = cal_default;
		return false;
	}
	/* The vendor passes the gain on in whole per cent. */
	c.gain_pct = (uint16_t)(gain * 100);
	*cal = c;
	return true;
}

static float cubic(const float c[3], float x)
{
	return x * (c[0] + x * (c[1] + x * c[2]));
}

/* x^0.4696 and x^0.5636 for the 41 mm watch's red and green below
 * level 60, times 0.0556 and 0.0348, in 65535ths. */
static const uint16_t red_low[60] = {
	0, 3644, 5046, 6104, 6987, 7759, 8452, 9087, 9675, 10225, 10744, 11235, 11704, 12152, 12583,
	12997, 13397, 13784, 14159, 14523, 14877, 15222, 15558, 15886, 16207, 16520, 16827, 17128, 17423, 17713,
	17997, 18276, 18551, 18821, 19087, 19348, 19606, 19860, 20110, 20357, 20600, 20841, 21078, 21312, 21543,
	21772, 21998, 22221, 22442, 22660, 22876, 23090, 23301, 23511, 23718, 23923, 24127, 24328, 24528, 24725,
};
static const uint16_t green_low[60] = {
	0, 2281, 3371, 4236, 4982, 5649, 6261, 6829, 7363, 7868, 8349, 8810, 9253, 9680, 10093,
	10493, 10882, 11260, 11629, 11988, 12340, 12684, 13021, 13351, 13675, 13994, 14306, 14614, 14917, 15215,
	15508, 15797, 16083, 16364, 16642, 16916, 17186, 17454, 17718, 17979, 18238, 18493, 18746, 18997, 19244,
	19490, 19733, 19973, 20212, 20448, 20682, 20914, 21144, 21372, 21599, 21823, 22046, 22267, 22486, 22704,
};

/* Each colour's part is never below zero; the grey's can be. */
float tsl258x_screen_counts(const struct tsl258x_profile *p, const struct tsl258x_cal *cal, const uint8_t rgb[3],
			    uint8_t level)
{
	float part[3], s = (float)level / 255;
	for (unsigned i = 0; i < 3; i++) {
		float v = cubic(p->poly[i], rgb[i]) * cal->rgbw[i] / p->ref[i];
		part[i] = v > 0 ? v : 0;
	}
	float grey = p->grey[0] * rgb[0] + p->grey[1] * rgb[1] + p->grey[2] * rgb[2];
	float white = cubic(p->poly[3], grey) * cal->rgbw[3] / p->ref[3];
	if (!p->colour_curves)
		return (part[0] + part[1] + part[2] - white) * s;
	float red, green, blue = (0.497f * level + 8.321f) / 133;
	if (level < 60) {
		red = (float)red_low[level] / 65535;
		green = (float)green_low[level] / 65535;
	} else {
		red = (1.1776f * level + 78.677f) / 377;
		green = (3.6768f * level + 193) / 1139;
	}
	return part[0] * red + part[1] * green + part[2] * blue - white * s;
}

/* Dim light near the brightest screen gives a tenth of channel 0, as
 * the vendor's does. */
int32_t tsl258x_lux(const struct tsl258x_profile *p, const struct tsl258x_cal *cal, enum tsl258x_gain gain, uint16_t c0,
		    uint16_t c1, float screen, uint8_t level)
{
	int32_t scale = gains[TSL258X_GAIN_111X] / gains[gain];
	float ch0 = (float)c0 * scale, ch1 = (float)c1 * scale, left = ch0 - screen;
	if (left < 0)
		left = 0;
	unsigned k = 0;
	if (c0 <= SATURATED && left > DIM_COUNTS && left < 8 * ch1)
		k = left >= 4.5f * ch1 ? 1 : left >= 3 * ch1 ? 2 : 3;
	float lux = left * p->k[k] * cal->gain_pct / 100;
	if (lux < 0.5f && ch0 <= 400 && level >= 199)
		return (int32_t)ch0 / 10;
	return (int32_t)lux;
}

bool tsl258x_fit(const float *x, const float *y, unsigned n, float *at0, float *per)
{
	float sx = 0, sy = 0, sxx = 0, sxy = 0;
	for (unsigned i = 0; i < n; i++) {
		sx += x[i];
		sy += y[i];
		sxx += x[i] * x[i];
		sxy += x[i] * y[i];
	}
	float d = n * sxx - sx * sx;
	if (n < 2 || d == 0)
		return false;
	*per = (n * sxy - sx * sy) / d;
	*at0 = (sy - *per * sx) / n;
	return true;
}

enum tsl258x_gain tsl258x_next_gain(enum tsl258x_gain gain, uint16_t c0)
{
	if (gain == TSL258X_GAIN_111X)
		return c0 >= HIGH_FULL ? TSL258X_GAIN_1X : gain;
	return c0 < LOW_EMPTY ? TSL258X_GAIN_111X : gain;
}

/* The vendor's SurfaceFlinger samples every third row and column of the
 * crop, averages each colour as light (its table, in hundredths) and
 * takes the 0-255 value nearest that average. */
#define STEP		3
#define GRID		17

static const uint16_t light[256] = {
	0, 0, 0, 0, 0, 2, 4, 4, 26, 49, 71, 94, 117, 140, 164, 187,
	212, 236, 261, 286, 312, 337, 363, 390, 417, 444, 471, 499, 527, 556, 585, 614,
	644, 674, 704, 735, 767, 798, 830, 863, 896, 929, 963, 997, 1031, 1066, 1102, 1138,
	1174, 1211, 1248, 1286, 1324, 1363, 1402, 1442, 1482, 1523, 1564, 1606, 1648, 1691, 1734, 1778,
	1822, 1867, 1912, 1958, 2004, 2051, 2099, 2147, 2196, 2245, 2295, 2345, 2396, 2448, 2500, 2552,
	2606, 2660, 2714, 2770, 2825, 2882, 2939, 2997, 3055, 3114, 3174, 3234, 3295, 3357, 3419, 3482,
	3546, 3610, 3675, 3741, 3807, 3874, 3942, 4010, 4080, 4150, 4220, 4292, 4364, 4437, 4511, 4585,
	4660, 4736, 4813, 4890, 4968, 5047, 5127, 5208, 5289, 5371, 5454, 5538, 5622, 5708, 5794, 5881,
	5969, 6057, 6147, 6237, 6328, 6420, 6513, 6607, 6702, 6797, 6894, 6991, 7089, 7188, 7288, 7389,
	7490, 7593, 7697, 7801, 7906, 8013, 8120, 8228, 8337, 8447, 8558, 8670, 8783, 8897, 9012, 9128,
	9245, 9362, 9481, 9601, 9722, 9843, 9966, 10090, 10215, 10341, 10467, 10595, 10724, 10854, 10985, 11117,
	11250, 11385, 11520, 11656, 11793, 11932, 12071, 12212, 12354, 12496, 12640, 12785, 12931, 13079, 13227, 13376,
	13527, 13679, 13831, 13985, 14141, 14297, 14454, 14613, 14773, 14934, 15096, 15259, 15424, 15589, 15756, 15924,
	16093, 16264, 16436, 16608, 16783, 16958, 17135, 17312, 17491, 17672, 17853, 18036, 18220, 18405, 18592, 18780,
	18969, 19159, 19351, 19544, 19739, 19934, 20131, 20329, 20529, 20730, 20932, 21135, 21340, 21546, 21754, 21963,
	22173, 22385, 22598, 22812, 23028, 23245, 23463, 23683, 23904, 24127, 24351, 24576, 24803, 25031, 25261, 25500,
};

static uint16_t grid[GRID][GRID];
static uint32_t crop_x, crop_y, crop_h, cols, rows;

void tsl258x_crop_set(const struct tsl258x_profile *p)
{
	crop_x = p->crop[0];
	crop_y = p->crop[1];
	crop_h = (uint32_t)(p->crop[3] - p->crop[1]);
	cols = ((uint32_t)(p->crop[2] - p->crop[0]) + STEP - 1) / STEP;
	rows = (crop_h + STEP - 1) / STEP;
	memset(grid, 0, sizeof grid);
}

void tsl258x_crop_line(uint32_t y, uint32_t x, uint32_t w, const uint16_t *rgb565)
{
	uint32_t row = y - crop_y;
	if (row >= crop_h || row % STEP)
		return;
	uint16_t *out = grid[row / STEP];
	for (uint32_t i = x > crop_x ? (x - crop_x + STEP - 1) / STEP : 0; i < cols; i++) {
		uint32_t at = crop_x + i * STEP;
		if (at >= x + w)
			break;
		out[i] = rgb565[at - x];
	}
}

/* The first of equally near values, as the vendor's. */
static uint8_t nearest(uint32_t v)
{
	unsigned best = 0;
	uint32_t best_d = UINT32_MAX;
	for (unsigned i = 0; i < 256; i++) {
		uint32_t d = light[i] > v ? light[i] - v : v - light[i];
		if (d < best_d) {
			best = i;
			best_d = d;
		}
	}
	return (uint8_t)best;
}

void tsl258x_crop_rgb(uint8_t rgb[3])
{
	uint32_t sum[3] = { 0 }, n = rows * cols;
	if (!n) {
		rgb[0] = rgb[1] = rgb[2] = 0;
		return;
	}
	for (uint32_t r = 0; r < rows; r++)
		for (uint32_t c = 0; c < cols; c++) {
			uint32_t v = grid[r][c], r5 = v >> 11, g6 = v >> 5 & 0x3f, b5 = v & 0x1f;
			sum[0] += light[r5 << 3 | r5 >> 2];
			sum[1] += light[g6 << 2 | g6 >> 4];
			sum[2] += light[b5 << 3 | b5 >> 2];
		}
	for (unsigned i = 0; i < 3; i++)
		rgb[i] = nearest(sum[i] / n);
}

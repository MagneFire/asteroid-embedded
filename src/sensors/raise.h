/* Wrist raises and lowers from the accelerometer, as the vendor's hub
 * firmware tells them: no hardware involved. Each sample is low-passed
 * into a ring of the last RAISE_RING, and the last RAISE_WINDOW of it
 * are looked at for a rise of y and of two angles with the screen off,
 * or for a fall of what rose with the screen on. Samples are in m/s²,
 * the watch's axes (z out of the face), one every 40 ms nominally. */
#ifndef RAISE_H
#define RAISE_H
#include <stdbool.h>
#include <stdint.h>

#define RAISE_RING	76
#define RAISE_WINDOW	25
#define RAISE_PERIOD_MS	40

enum { RAISE_NONE, RAISE_UP, RAISE_DOWN };
enum { RAISE_X, RAISE_Y, RAISE_Z, RAISE_XY, RAISE_AZ, RAISE_CHANNELS };

struct raise_trend {
	bool valid;
	int16_t start, end;
};

struct raise {
	float ring[RAISE_CHANNELS][RAISE_RING];
	int16_t at;
	int32_t fed;
	/* Minus the samples since the last raise and lower. */
	int16_t up, down;
	/* What rose in the last raise: y, the xy angle and the z angle. */
	float rose[3];
	struct raise_trend y, xy, z, fall_xy, fall_y;
	/* The screen came on for the last raise. */
	bool by_raise;
};

void raise_init(struct raise *r);
int raise_feed(struct raise *r, const float a[3], bool screen_on);
void raise_screen(struct raise *r, bool on);
/* How many times a sample is fed after elapsed_ms since the last one. */
unsigned raise_repeats(int32_t elapsed_ms);
#endif

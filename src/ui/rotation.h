/* The screen's rotation in nightstand mode, as asteroid-launcher turns
 * its compositor: the whole picture turned clockwise by 0, 90, 180 or
 * 270 degrees about the centre of the square screen. rotation_set sets
 * the angle and turns towards it the shorter way round over
 * ROTATION_MS; rotation_turn gives the cosine and sine of the angle
 * shown at now_ms, for the GPU, and rotation_animating whether it is
 * still turning. rotation_touch maps a touch on the screen to the
 * point of the picture under it, at the angle set. rotation_buffer
 * turns a picture drawn upright on the CPU, in place, by the angle set.
 * rotation_hold fixes the angle whatever rotation_set is given, for
 * testing, until it is given -1. */
#ifndef ROTATION_H
#define ROTATION_H
#include <stdint.h>
#include <stdbool.h>

#define ROTATION_MS 150

void rotation_init(int size);
void rotation_set(int degrees, uint32_t now_ms);
int rotation_angle(void);
void rotation_hold(int degrees, uint32_t now_ms);
bool rotation_animating(uint32_t now_ms);
void rotation_turn(uint32_t now_ms, float *c, float *s);
void rotation_touch(int *x, int *y);
void rotation_buffer(uint32_t *buf, uint32_t stride);
#endif

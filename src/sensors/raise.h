/* Telling a wrist raise from other turns, from accelerometer samples
 * taken for a short while after the hub saw the pose change: no
 * hardware involved. A raise is the face coming to rest facing up
 * after it was seen facing away; turning it away again is not one. A
 * check ends when the arm comes to rest, as a lowered arm can swing for
 * a second or more. */
#ifndef RAISE_H
#define RAISE_H
#include <stdbool.h>
#include <stdint.h>

#define RAISE_CHECK_MS	2500

struct raise {
	int16_t last[3];
	uint32_t until;
	uint8_t low;
	bool have_last, in_view, checking;
};

/* Taken as in view until a sample says otherwise, so arming it with
 * the face up reports nothing. */
void raise_arm(struct raise *r, uint32_t now);
void raise_check(struct raise *r, uint32_t now);
bool raise_checking(struct raise *r, uint32_t now);
/* A new sample in mg, the watch's axes (z out of the face): true for a
 * raise. */
bool raise_sample(struct raise *r, const int16_t mg[3], uint32_t now);
#endif

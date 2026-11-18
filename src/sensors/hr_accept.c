#include <string.h>
#include "sensors/hr_accept.h"

void hr_accept_reset(struct hr_accept *a)
{
	memset(a, 0, sizeof *a);
}

unsigned hr_accept_take(struct hr_accept *a, float bpm, float y, float z, uint32_t now)
{
	if (bpm <= 0) {
		hr_accept_reset(a);
		return 0;
	}
	if (!a->started) {
		a->started = true;
		a->first_t = now;
	}
	if (bpm < 35 || bpm > 200 || y < 0 || z > 0) {
		a->n = 0;
		a->skip = z > 0 ? HR_AFTER_JUMP : 0;
		return 0;
	}
	if (a->skip) {
		a->skip--;
		return 0;
	}
	if (a->n == HR_WINDOW) {
		memmove(a->bpm, a->bpm + 1, HR_WINDOW - 1);
		memmove(a->trusted, a->trusted + 1, (HR_WINDOW - 1) * sizeof a->trusted[0]);
		a->n--;
	}
	a->bpm[a->n] = (uint8_t)(bpm + 0.5f);
	a->trusted[a->n++] = y >= 2;
	if (a->n < HR_WINDOW)
		return 0;
	uint8_t s[HR_WINDOW];
	bool trusted = false;
	memcpy(s, a->bpm, sizeof s);
	for (unsigned i = 0; i < HR_WINDOW; i++) {
		trusted |= a->trusted[i];
		for (unsigned j = i + 1; j < HR_WINDOW; j++)
			if (s[j] < s[i]) {
				uint8_t t = s[i];
				s[i] = s[j];
				s[j] = t;
			}
	}
	uint32_t since = now - a->first_t;
	if (s[HR_WINDOW - 1] - s[0] > HR_SPREAD || since < (trusted ? HR_TRUSTED_MS : HR_STEADY_MS))
		return 0;
	return s[HR_WINDOW / 2];
}

#include "asteroid.h"
#include "prof.h"

#define SAMPLES	8192
#define SHOWN	256

extern const uint32_t *irq_frame;
static uint32_t pcs[SAMPLES];
static volatile uint32_t taken, wanted;

void prof_start(uint32_t samples)
{
	wanted = 0;
	taken = 0;
	wanted = samples > SAMPLES ? SAMPLES : samples;
}

/* From the tick's interrupt: the address it interrupted. */
void prof_sample(void)
{
	if (taken < wanted && irq_frame)
		pcs[taken++] = irq_frame[5] >> 6;
}

void prof_report(void (*say)(const char *fmt, ...))
{
	uint32_t n = taken, top[SHOWN], count[SHOWN], shown = 0;
	if (n < wanted) {
		say("prof: %lu of %lu samples so far\n", n, (uint32_t)wanted);
		return;
	}
	wanted = 0;
	for (uint32_t gap = n / 2; gap; gap /= 2)
		for (uint32_t i = gap; i < n; i++) {
			uint32_t v = pcs[i], j = i;
			for (; j >= gap && pcs[j - gap] > v; j -= gap)
				pcs[j] = pcs[j - gap];
			pcs[j] = v;
		}
	for (uint32_t i = 0; i < n;) {
		uint32_t j = i;
		while (j < n && pcs[j] == pcs[i])
			j++;
		uint32_t c = j - i, at = shown;
		while (at && count[at - 1] < c)
			at--;
		if (at < SHOWN) {
			if (shown < SHOWN)
				shown++;
			for (uint32_t k = shown - 1; k > at; k--) {
				top[k] = top[k - 1];
				count[k] = count[k - 1];
			}
			top[at] = pcs[i];
			count[at] = c;
		}
		i = j;
	}
	say("prof: %lu samples\n", n);
	for (uint32_t i = 0; i < shown; i++)
		say("%08lx %lu\n", top[i] << 6, count[i]);
	taken = 0;
}

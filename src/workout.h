#ifndef WORKOUT_H
#define WORKOUT_H
#include <stdint.h>
#include <stdbool.h>

#define WORKOUTS	8

/* A workout that ended: when it started (RTC), how long it took, its
 * steps and what they came to, and the average and highest heart rate,
 * 0 without readings. */
struct workout {
	uint32_t start, seconds, steps, distance_m, kcal;
	uint8_t avg_bpm, max_bpm;
	uint16_t reserved;
};

void workout_init(void);
bool workout_running(void);
void workout_start(uint32_t rtc);
void workout_stop(uint32_t rtc);
/* A new heart rate reading while it runs. */
void workout_heart_rate(uint32_t bpm);
/* The one running so far, start and seconds included. */
struct workout workout_now(uint32_t rtc);
/* The i-th last one that ended, or NULL. */
const struct workout *workout_past(unsigned i);
#endif

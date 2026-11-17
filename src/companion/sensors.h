/* The sensors the AP's sensor HAL asks for on channel 4, from the hub:
 * the accelerometer in m/s², the step counter and detector and the
 * wrist tilt. The AP's subscriptions only add reports; the watch's own
 * use of the hub goes on as before. Other handles are taken and never
 * reported. */
#ifndef COMPANION_SENSORS_H
#define COMPANION_SENSORS_H
#include <stdbool.h>
#include <stdint.h>

/* After the hub has started: the subscriptions from before a warm
 * reset if keep_subs, else none. */
void sns_start(bool keep_subs);
/* A RequestBody; false if it is malformed. */
bool sns_command(const uint8_t *p, unsigned n);
/* Drops the subscriptions, for an AP that went down. */
void sns_reset(void);
/* Takes the samples that are due. */
void sns_pass(void);
bool sns_due(void);
/* The next report in its channel 4 envelope, as many of the oldest
 * events as fit in link_send: its length, 0 if none. They stay queued
 * until sns_sent, or sns_unsent if the AP did not take it. */
unsigned sns_report(uint8_t *out, unsigned cap);
void sns_sent(void);
void sns_unsent(void);
/* The time to the next sample or report; link_free says whether a
 * report that is due can go out. */
uint32_t sns_next_ms(bool link_free);
void sns_status(void (*say)(const char *fmt, ...));
#endif

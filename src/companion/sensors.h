/* The sensors the AP's sensor HAL asks for on channel 4: the
 * accelerometer in m/s², the gyroscope in rad/s, the magnetometer in
 * µT, the orientation in degrees, the light in lux, the step counter
 * and detector, the wrist tilt and the heart rate. The AP's
 * subscriptions only add reports; the watch's own use of the hub goes
 * on as before. Other handles are taken and never reported. */
#ifndef COMPANION_SENSORS_H
#define COMPANION_SENSORS_H
#include <stdbool.h>
#include <stdint.h>

/* After the hub has started: with lent_boot, the AP kept the panel
 * across a warm reset and its subscriptions from before it are kept;
 * else there are none. */
void sns_start(bool lent_boot);
/* The AP takes the panel (to_ap) or gives it back. */
void sns_lend(bool to_ap);
/* The chips on the touch controller's bus were put to sleep behind
 * their drivers: they start over. */
void sns_chips_off(void);
/* Whether the AP has any sensor on. */
bool sns_active(void);
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

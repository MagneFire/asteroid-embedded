/* The PAH8001 heart rate sensor, run beside a hub driver that hands it
 * its calls (hub_mpu6515 on lenok), on the hub's bus. */
#ifndef PAH8001_H
#define PAH8001_H
#include <stdbool.h>
#include <stdint.h>

/* Powers it down at boot, as it starts out looking for skin with its LED. */
void pah8001_init(void);
void pah8001_on(bool on);
uint32_t pah8001_rate(uint8_t *status, unsigned *readings);
void pah8001_poll(void);
bool pah8001_can_sleep(void);
void pah8001_status(void (*say)(const char *fmt, ...));
#endif

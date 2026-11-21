/* The AFE4900 heart rate front end, run beside a hub driver that hands
 * it its calls (hub_lsm6dso on beluga). */
#ifndef AFE4900_H
#define AFE4900_H
#include <stdbool.h>
#include <stdint.h>

void afe4900_on(bool on);
/* A second user of the readings, the AP's: on while either wants it. */
void afe4900_share(bool on);
uint32_t afe4900_rate(uint8_t *status, unsigned *readings);
void afe4900_poll(void);
bool afe4900_can_sleep(void);
void afe4900_status(void (*say)(const char *fmt, ...));
#endif

/* The display subsystem under the panel's driver: power and clocks,
 * DSI packets either way on virtual channel 0, and frames from memory
 * to the panel's. */
#ifndef DSS_H
#define DSS_H
#include <stdbool.h>
#include <stdint.h>

/* Up for a frame of this size, in low power mode; from_ulps if dss_off
 * left the lanes there. */
bool dss_on(uint32_t width, uint32_t height, bool from_ulps);
void dss_off(bool ulps);
bool dss_up(void);
void dsi_high_speed(bool on);
/* A DCS write (the command, then its parameters) or a generic one;
 * sync waits for the panel to take the bus and give it back, which it
 * does once it has dealt with the packet. */
int dsi_write(bool generic, const uint8_t *d, unsigned n, bool sync);
int dsi_sync(void);
int dsi_read(bool generic, const uint8_t *req, uint8_t *buf, unsigned n);
bool dss_frame_start(const uint32_t *buf, bool te);
bool dss_frame_done(void);
void dss_frame_abort(void);
/* Sleeps until the frame-done event, or any other interrupt, unless it
 * came already. */
void dss_frame_sleep(void);
#endif

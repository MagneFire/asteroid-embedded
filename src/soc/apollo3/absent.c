/* What the application processors have and this chip has not: no USB,
 * no eMMC to read vendor files from, no GPU and no screenshots, no
 * MAX17050 gauge, and no UART in use. The console goes over SWD instead
 * (swdbox.c), and the USB calls of the main loop serve the link to the
 * application processor. */
#include "asteroid.h"
#include "storage/storage.h"
#include "console/console.h"
#include "screenshot/screenshot.h"
#include "gpu/a3xx.h"
#include "companion/companion.h"

void usb_init(const char *serialno) { companion_init(); }
void usb_cable(bool present, bool edge) {}
uint32_t usb_next_ms(void) { return companion_next_ms(); }
bool dcd_stuck(void) { return false; }
void usb_pass(void) { companion_pass(); }

void usb_poll(void)
{
	swdbox_poll();
	companion_poll();
}
bool usb_connected(void) { return false; }
bool usb_resting(void) { return companion_resting(); }
bool usb_busy(void) { return companion_next_ms() == 0; }
bool store_quiet(void) { return companion_store_ok(); }
void usb_stats(uint32_t *rx, uint32_t *tx) { *rx = *tx = 0; }

void screenshot_poll(void) {}
void emmc_idle(void) {}
int soc_dload_mode(bool on) { return -1; }
bool gpu_init(void) { return false; }
bool gpu_selftest(void) { return false; }

const struct battery_driver battery_max17050;
bool gauge_model_load(void) { return false; }

const void *firmware_get(const char *name, uint32_t *size) { return NULL; }
int firmware_read(const char *path, uint32_t off, void *buf, uint32_t len) { return -1; }

int uart_init(unsigned uart, unsigned first_gpio, unsigned func, uint32_t baud, bool flow) { return -1; }
int uart_set_baud(unsigned uart, uint32_t baud) { return -1; }
int uart_write(unsigned uart, const uint8_t *p, unsigned n) { return -1; }
unsigned uart_read(unsigned uart, uint8_t *p, unsigned n) { return 0; }
bool uart_cts(unsigned uart) { return false; }
void uart_stop(unsigned uart) {}
bool uart_idle(unsigned uart) { return true; }
void uart_hold_rx(unsigned uart, bool hold) {}
void uart_stats(unsigned uart, uint32_t *rx, uint32_t *tx, uint32_t *errors) { *rx = *tx = *errors = 0; }

void screenshot_to_file(int layer) { log_printf("shot: no frame buffer to take it from\n"); }
bool screenshot_busy(void) { return false; }

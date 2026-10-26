/* Default for the UI's panic path: log the message, keep it as the fault
 * record for the next boot and reset. */
#include <string.h>
#include "asteroid.h"
#include "ui/ui.h"

__attribute__((weak)) void ui_panic(const char *msg, size_t len)
{
	static char rec[640];
	size_t n = (size_t)snprintf(rec, sizeof rec, "ui panic t=%lu build=%s: ", time_ms(), build_id);
	if (n > sizeof rec - 2)
		n = sizeof rec - 2;
	if (len > sizeof rec - n - 2)
		len = sizeof rec - n - 2;
	memcpy(rec + n, msg, len);
	rec[n + len] = '\n';
	rec[n + len + 1] = 0;
	log_puts("\n!!! ");
	log_puts(rec);
	log_record(rec);
	reboot();
}

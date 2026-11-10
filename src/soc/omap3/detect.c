/* Which watch this is, and why it started. There is one OMAP3 watch, so
 * the bootloader's tree only has to confirm it; what the bootloader
 * adds to the tree's chosen node says how the watch came up. */
#include <string.h>
#include "asteroid.h"
#include "lib/fdt.h"

const struct board *board = &board_minnow;
const char *board_detected_by = "nothing, assumed";
bool board_assumed;

static char reason[48] = "no boot reason";

const char *boot_reason(void) { return reason; }

/* The bootloader names the panel it found and the version of the
 * settings it gave it, which are the ones the display driver has. */
#define PANEL_SETTINGS	0x081214u
static bool panel_lit;

bool boot_panel_lit(void) { return panel_lit; }

/* The value of key= in the kernel command line, up to the next space. */
static unsigned arg(const char *args, uint32_t len, const char *key, char *out, unsigned cap)
{
	size_t klen = strlen(key);
	for (uint32_t i = 0; i + klen < len && args[i]; i++) {
		if ((i && args[i - 1] != ' ') || memcmp(args + i, key, klen))
			continue;
		unsigned n = 0;
		for (i += klen; i < len && args[i] && args[i] != ' ' && n + 1 < cap; i++)
			out[n++] = args[i];
		out[n] = 0;
		return n;
	}
	out[0] = 0;
	return 0;
}

void board_detect(uint32_t machtype, const void *fdt)
{
	const char *model = NULL;
	if (fdt_valid(fdt)) {
		uint32_t len;
		const char *compat = fdt_prop(fdt, "/", "compatible", &len);
		model = fdt_prop(fdt, "/", "model", NULL);
		if (compat && fdt_strlist_has(compat, len, board_minnow.compatible))
			board_detected_by = "device tree";
		else
			board_assumed = true;
		const char *args = fdt_prop(fdt, "/chosen", "bootargs", &len);
		const uint8_t *up = fdt_prop(fdt, "/chosen", "mmi,powerup_reason", NULL);
		char why[20], hwrev[8], panel[24];
		if (args) {
			if (arg(args, len, "panel_minnow.panel_param=", panel, sizeof panel)) {
				uint32_t version = 0;
				const char *v = strchr(panel, '#');
				for (v = v ? v + 1 : ""; *v >= '0' && *v <= '9'; v++)
					version = version * 10 + (uint32_t)(*v - '0');
				panel_lit = panel[0] >= '2' && panel[0] <= '9' && panel[1] == '#' && version == PANEL_SETTINGS;
			}
			arg(args, len, "androidboot.bootreason=", why, sizeof why);
			arg(args, len, "androidboot.hwrev=", hwrev, sizeof hwrev);
			snprintf(reason, sizeof reason, "%s, power-up %02x%02x%02x%02x, hwrev %s", why[0] ? why : "?",
				 up ? up[0] : 0, up ? up[1] : 0, up ? up[2] : 0, up ? up[3] : 0, hwrev[0] ? hwrev : "?");
		}
	} else
		board_assumed = true;
	log_printf("board: %s by %s (tree %08lx %s, machtype %lu)\n", board->name, board_detected_by,
		   (uint32_t)(uintptr_t)fdt, model ? model : "none", machtype);
}

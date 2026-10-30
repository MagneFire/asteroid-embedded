/* The screenshot service: a write to 6001 takes a copy of the frame on
 * screen (the front buffer, as the GPU composed it), encodes it as JPEG a
 * few MCU rows per main loop iteration and hands the file to the GATT
 * layer, which sends it on 6002 the way asteroid-btsyncd does. */
#include <string.h>
#include "asteroid.h"
#include "display/display.h"
#include "bt/bt.h"
#include "jpeg.h"
#include "screenshot.h"
#include "ui/ui.h"
#include "usb/adb.h"

#define QUALITY 75
#define STEP_US 4000

static struct jpeg enc;
static bool encoding, to_file, file_requested;
static int from_layer;
static uint32_t started;

void screenshot_to_file(int layer)
{
	from_layer = layer;
	file_requested = true;
}

void screenshot_poll(void)
{
	bool file = file_requested;
	file_requested = false;
	if (file || bt_gatt_screenshot_requested()) {
		to_file = file;
		uint32_t bytes = fb.stride * fb.height * 4;
		bool busy = encoding || bt_gatt_screenshot_busy();
		if (busy || !fb.width || bytes > SCREENSHOT_SIZE / 2) {
			log_printf("screenshot: %s\n", busy ? "busy" : "no display");
			if (!busy && !file)
				bt_gatt_screenshot_send(NULL, 0);
		} else {
			uint32_t *copy = (uint32_t *)(uintptr_t)SCREENSHOT_BASE;
			memcpy(copy, file && from_layer >= 0 ? (const void *)(uintptr_t)(UI_LAYER_BASE + from_layer * UI_LAYER_STRIDE) : fb.buf[fb.back ^ 1], bytes);
			jpeg_begin(&enc, copy, fb.width, fb.height, fb.stride, (uint8_t *)(uintptr_t)(SCREENSHOT_BASE + bytes),
				   SCREENSHOT_SIZE - bytes, QUALITY);
			encoding = true;
			started = time_ms();
		}
	}
	if (!encoding)
		return;
	bool slow = cpu_clock_is_slow();
	if (slow)
		cpu_clock_slow(false);
	uint32_t t0 = time_us();
	bool done;
	while (!(done = jpeg_step(&enc)) && time_us() - t0 < STEP_US)
		;
	if (slow)
		cpu_clock_slow(true);
	if (!done)
		return;
	encoding = false;
	log_printf("screenshot: %lux%lu jpeg, %lu bytes in %lu ms\n", fb.width, fb.height, enc.len, time_ms() - started);
	if (to_file)
		adb_fs_write("screen.jpg", 0100644, pmic_rtc_read(), enc.out, enc.len);
	else
		bt_gatt_screenshot_send(enc.len ? enc.out : NULL, enc.len);
}

bool screenshot_busy(void)
{
	return encoding;
}

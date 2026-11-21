/* The OPPO Watch size from the panel's ID replies: both sizes read on
 * time and a clock late, and the 46 mm watch for an unknown panel and
 * for one not read. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "detect.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

const struct board board_beluga = { .name = "beluga", .panel = { .width = 402, .height = 476, .id = { 0x09, 0x80, 0x01 } } };
const struct board board_beluga_41 = { .name = "beluga-41", .panel = { .width = 320, .height = 360, .id = { 0x01, 0x80, 0x01 } } };

static char last[256];
void log_printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(last, sizeof last, fmt, ap);
	va_end(ap);
}

static void reply(uint8_t raw[3][2], uint8_t a0, uint8_t a1, uint8_t b0, uint8_t b1, uint8_t c0, uint8_t c1)
{
	raw[0][0] = a0, raw[0][1] = a1, raw[1][0] = b0, raw[1][1] = b1, raw[2][0] = c0, raw[2][1] = c1;
}

int main(void)
{
	uint8_t raw[3][2];
	enum rm69090_framing f;

	reply(raw, 0x09, 0x09, 0x80, 0x80, 0x01, 0x01);
	CHECK(board_by_panel_id(raw, &f) == &board_beluga && f == RM69090_ON_TIME);
	board = &board_beluga_41;
	CHECK(board_detect_panel(raw) == RM69090_ON_TIME && board == &board_beluga);
	CHECK(strstr(last, "beluga by panel ID, id 09 80 01 on time"));

	reply(raw, 0x01, 0x01, 0x80, 0x80, 0x01, 0x01);
	CHECK(board_detect_panel(raw) == RM69090_ON_TIME && board == &board_beluga_41);
	CHECK(strstr(last, "beluga-41 by panel ID, id 01 80 01 on time"));

	/* Late: as the 46 mm watch read it at 48 MHz, a byte at a time,
	 * and with the second bytes. */
	reply(raw, 0x04, 0x00, 0xc0, 0x00, 0x00, 0x00);
	CHECK(board_detect_panel(raw) == RM69090_LATE && board == &board_beluga);
	CHECK(strstr(last, "id 09 80 01 a bit late"));
	reply(raw, 0x04, 0xff, 0xc0, 0x00, 0x00, 0xff);
	CHECK(board_detect_panel(raw) == RM69090_LATE && board == &board_beluga);
	reply(raw, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x00);
	CHECK(board_detect_panel(raw) == RM69090_LATE && board == &board_beluga_41);
	CHECK(strstr(last, "beluga-41 by panel ID, id 01 80 01 a bit late"));
	reply(raw, 0x00, 0xff, 0xc0, 0x00, 0x00, 0xff);
	CHECK(board_detect_panel(raw) == RM69090_LATE && board == &board_beluga_41);

	/* Unknown, failed and skipped reads give the 46 mm watch. */
	reply(raw, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00);
	board = &board_beluga_41;
	CHECK(!board_by_panel_id(raw, &f) && f == RM69090_UNKNOWN);
	CHECK(board_detect_panel(raw) == RM69090_UNKNOWN && board == &board_beluga);
	CHECK(strstr(last, "beluga by nothing, unknown panel ID, id 00 80 00 unknown"));
	memset(raw, 0, sizeof raw);
	board = &board_beluga_41;
	CHECK(board_detect_panel(raw) == RM69090_UNKNOWN && board == &board_beluga);
	memset(raw, 0xff, sizeof raw);
	CHECK(board_detect_panel(raw) == RM69090_UNKNOWN && board == &board_beluga);
	board = &board_beluga_41;
	CHECK(!board_by_panel_id(NULL, &f) && f == RM69090_UNKNOWN);
	CHECK(board_detect_panel(NULL) == RM69090_UNKNOWN && board == &board_beluga);
	CHECK(strstr(last, "beluga by nothing, panel not read"));

	if (failures)
		printf("beluga_detect_test: %d failures\n", failures);
	else
		printf("beluga_detect_test: ok\n");
	return failures != 0;
}

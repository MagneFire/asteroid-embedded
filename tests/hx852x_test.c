/* HX852xES touch report parsing. */
#include <stdio.h>
#include <string.h>
#include "input/hx852x.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static void point(uint8_t *buf, unsigned i, unsigned x, unsigned y)
{
	buf[4 * i] = (uint8_t)(x >> 8);
	buf[4 * i + 1] = (uint8_t)x;
	buf[4 * i + 2] = (uint8_t)(y >> 8);
	buf[4 * i + 3] = (uint8_t)y;
}

static void seal(uint8_t *buf)
{
	uint8_t sum = 0;
	for (unsigned i = 0; i < HX852X_REPORT_LEN - 1; i++)
		sum = (uint8_t)(sum + buf[i]);
	buf[HX852X_REPORT_LEN - 1] = (uint8_t)-sum;
}

static void empty(uint8_t *buf)
{
	memset(buf, 0xff, HX852X_REPORT_LEN);
	buf[13] = buf[14] = 0;
	seal(buf);
}

int main(void)
{
	struct hx852x_report r;
	uint8_t buf[HX852X_REPORT_LEN];

	/* No fingers, as the controller sends after a lift. */
	empty(buf);
	hx852x_parse(buf, &r);
	CHECK(!r.bad && r.n == 0 && !r.palm && !hx852x_follow(&r, -1));

	/* One finger down in slot 0, x and y over 8 bits. */
	empty(buf);
	point(buf, 0, 0x123, 0x18f);
	buf[8] = 0x20;
	buf[12] = 1;
	buf[13] = 0x01;
	seal(buf);
	hx852x_parse(buf, &r);
	CHECK(!r.bad && r.n == 1 && r.p[0].id == 0 && r.p[0].x == 0x123 && r.p[0].y == 0x18f);
	CHECK(hx852x_follow(&r, -1) == &r.p[0] && hx852x_follow(&r, 0) == &r.p[0] && !hx852x_follow(&r, 1));

	/* A bad sum drops the report. */
	buf[1] ^= 1;
	hx852x_parse(buf, &r);
	CHECK(r.bad && r.n == 0);

	/* The first finger lifted, a second one in slot 1 stays. */
	empty(buf);
	point(buf, 1, 10, 390);
	buf[12] = 1;
	buf[13] = 0x02;
	seal(buf);
	hx852x_parse(buf, &r);
	CHECK(r.n == 1 && r.p[0].id == 1 && r.p[0].x == 10 && r.p[0].y == 390);
	CHECK(!hx852x_follow(&r, 0) && hx852x_follow(&r, 1) == &r.p[0]);

	/* Two fingers. */
	point(buf, 0, 200, 200);
	buf[12] = 2;
	buf[13] = 0x03;
	seal(buf);
	hx852x_parse(buf, &r);
	CHECK(r.n == 2 && r.p[0].id == 0 && r.p[1].id == 1 && hx852x_follow(&r, 1) == &r.p[1]);

	/* A palm: no count, the first point at 0xfa5a both ways, width 0. */
	empty(buf);
	point(buf, 0, 0xfa5a, 0xfa5a);
	buf[8] = 0;
	seal(buf);
	hx852x_parse(buf, &r);
	CHECK(!r.bad && r.palm && r.n == 0);

	/* The wake packet. */
	CHECK(hx852x_is_wake((const uint8_t *)"Wake"));
	CHECK(!hx852x_is_wake((const uint8_t *)"Wako"));

	if (failures)
		printf("%d failures\n", failures);
	else
		printf("hx852x: all passed\n");
	return failures != 0;
}

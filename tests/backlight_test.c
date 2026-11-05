/* The LM3535's brightness curve. */
#include <stdio.h>
#include "display/lm3535.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

int main(void)
{
	CHECK(lm3535_code(0) == 0);
	CHECK(lm3535_code(4) == 0);
	CHECK(lm3535_code(5) == LM3535_DIM);
	CHECK(lm3535_code(9) == LM3535_DIM);
	CHECK(lm3535_code(10) == 67);
	CHECK(lm3535_code(255) == 108);
	/* It only goes up and stays in range. */
	for (int l = 11; l < 256; l++) {
		CHECK(lm3535_code((uint8_t)l) >= lm3535_code((uint8_t)(l - 1)));
		CHECK(lm3535_code((uint8_t)l) <= LM3535_MAX);
	}
	puts(failures ? "backlight_test: FAILED" : "backlight_test: ok");
	return failures != 0;
}

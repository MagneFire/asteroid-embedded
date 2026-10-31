/* The calculator on the host: its number formatting and parsing, and
 * the key sequences of asteroid-calculator's engine. */
#include <stdio.h>
#include <string.h>
#include "asteroid.h"
#include "ui/ui.h"
#include "calc.h"
#include "stubs.h"

static void keys(const char *s)
{
	for (; *s; s++) {
		int k = *s >= '0' && *s <= '9' ? *s - '0' :
			*s == '.' ? UI_CALC_POINT : *s == '+' ? UI_CALC_ADD : *s == '-' ? UI_CALC_SUBTRACT :
			*s == '*' ? UI_CALC_MULTIPLY : *s == '/' ? UI_CALC_DIVIDE : *s == '=' ? UI_CALC_EQUALS :
			*s == 'C' ? UI_CALC_CLEAR : *s == '<' ? UI_CALC_BACK : *s == '~' ? UI_CALC_NEGATE : -1;
		calc_key(k);
	}
}

static bool fmt_is(double x, const char *want)
{
	char buf[40];
	calc_format(x, buf, sizeof buf);
	if (strcmp(buf, want))
		fprintf(stderr, "format: got %s, want %s\n", buf, want);
	return !strcmp(buf, want);
}

static bool text_is(const char *typed, const char *text, const char *shown)
{
	calc_reset();
	keys(typed);
	bool ok = !strcmp(calc_text(), text) && !strcmp(calc_shown(), shown);
	if (!ok)
		fprintf(stderr, "%s: got \"%s\" shown \"%s\", want \"%s\" shown \"%s\"\n", typed, calc_text(), calc_shown(), text, shown);
	return ok;
}

int main(void)
{
	CHECK(fmt_is(0.0, "0"));
	CHECK(fmt_is(-0.0, "0"));
	CHECK(fmt_is(5.0, "5"));
	CHECK(fmt_is(-2.5, "-2.5"));
	CHECK(fmt_is(0.1 + 0.2, "0.3"));
	CHECK(fmt_is(1.0 / 3.0, "0.3333333333333"));
	CHECK(fmt_is(2.0 / 3.0, "0.6666666666667"));
	CHECK(fmt_is(1234567890123.0, "1234567890123"));
	CHECK(fmt_is(12345678901234.0, "12345678901230"));
	CHECK(fmt_is(1e21, "1e+21"));
	CHECK(fmt_is(1.5e22, "1.5e+22"));
	CHECK(fmt_is(1e20, "100000000000000000000"));
	CHECK(fmt_is(0.000001, "0.000001"));
	CHECK(fmt_is(1e-7, "1e-7"));
	CHECK(fmt_is(-1.25e-8, "-1.25e-8"));
	CHECK(fmt_is(123.456, "123.456"));
	CHECK(fmt_is(__builtin_inf(), "Infinity"));
	CHECK(fmt_is(-__builtin_inf(), "-Infinity"));
	CHECK(fmt_is(__builtin_nan(""), "NaN"));
	CHECK(calc_parse("") == 0.0);
	CHECK(calc_parse("12.5") == 12.5);
	CHECK(calc_parse("-3") == -3.0);
	CHECK(calc_parse(".5") == 0.5);
	CHECK(calc_parse("1.") == 1.0);
	CHECK(calc_parse("1e+21") == 1e21);
	CHECK(calc_parse("2.5e-3") == 0.0025);
	CHECK(calc_parse("-") != calc_parse("-"));
	CHECK(calc_parse("abc") != calc_parse("abc"));
	CHECK(calc_parse("Infinity") == __builtin_inf());

	CHECK(text_is("", "0", "0"));
	CHECK(text_is("7", "7", "7"));
	CHECK(text_is("07", "7", "7"));
	CHECK(text_is("1234567", "1234567", "1,234,567"));
	CHECK(text_is("1234.5678", "1234.5678", "1,234.5678"));
	CHECK(text_is("1.", "1.", "1"));
	CHECK(text_is("1.50", "1.50", "1.50"));
	CHECK(text_is("1.2.3", "1.23", "1.23"));
	CHECK(text_is("123456789012345", "12345678901234", "12,345,678,901,234"));
	CHECK(text_is("12<", "1", "1"));
	CHECK(text_is("1<<", "0", "0"));
	CHECK(text_is("12C", "0", "0"));
	CHECK(text_is("5~", "-5", "-5"));
	CHECK(text_is("5~~", "5", "5"));
	CHECK(text_is("~", "-", "-"));
	CHECK(text_is("~~", "0", "0"));
	CHECK(text_is("~5", "-5", "-5"));
	CHECK(text_is("2+3=", "5", "5"));
	CHECK(text_is("2+3", "3", "3"));
	CHECK(text_is("2+", "", "2"));
	CHECK(text_is("10-4=", "6", "6"));
	CHECK(text_is("2*3~=", "-6", "-6"));
	CHECK(text_is("10/4=", "2.5", "2.5"));
	CHECK(text_is("1/3=", "0.3333333333333", "0.3333333333333"));
	CHECK(text_is("0.1+0.2=", "0.3", "0.3"));
	CHECK(text_is("2+3*4=", "20", "20"));
	CHECK(text_is("2+3+", "", "5"));
	CHECK(text_is("2+3=4", "4", "4"));
	CHECK(text_is("2+3=+1=", "6", "6"));
	CHECK(text_is("5+=", "5", "5"));
	CHECK(text_is("5+*2=", "10", "10"));
	CHECK(text_is("1/0=", "Infinity", "Infinity"));
	CHECK(text_is("0/0=", "NaN", "NaN"));
	/* Rounded to 13 digits and shown with the locale's 0 decimals. */
	CHECK(text_is("9999999*9999999*9999999=", "999999700000000000000", "999,999,700,000,000,000,000"));
	CHECK(text_is("99999999999999*99999999999999=", "1e+28", "10,000,000,000,000,000,000,000,000,000"));
	CHECK(text_is("1/9999999999=", "1.0000000001e-10", "0"));
	CHECK(text_is("1/99999999999999=", "1e-14", "0"));
	CHECK(text_is("1/999999=", "0.000001000001000001", "0.000001000001000001"));
	CHECK(text_is("12345678901234*10=", "123456789012300", "123,456,789,012,300"));
	if (stub_failures)
		fprintf(stderr, "calc_test: %d failures\n", stub_failures);
	else
		printf("calc_test: ok\n");
	return stub_failures != 0;
}

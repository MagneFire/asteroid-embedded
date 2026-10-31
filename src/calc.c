/* The calculator works as asteroid-calculator's: digits and the point
 * append to the text (14 characters at most, one point), an operator
 * applies the pending one to the previous and current text, keeps the
 * result as the previous text and clears the current, and = applies it
 * and keeps the result as the current text, which the next digit
 * replaces. Results are rounded to 13 significant digits. The shown
 * text groups the integer digits in threes, as the locale formatting
 * does, and keeps the fraction as typed. */
#include <string.h>
#include "asteroid.h"
#include "ui/ui.h"
#include "calc.h"

static char text[CALC_MAX + 16] = "0", previous[CALC_MAX + 16], shown[64];
static int op = -1, last_key = -1;

static const double pow10_tab[] = {
	1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
	1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

static double pow10d(int e)
{
	double r = 1.0;
	bool neg = e < 0;
	if (neg)
		e = -e;
	while (e > 22) {
		r *= 1e22;
		e -= 22;
	}
	r *= pow10_tab[e];
	return neg ? 1.0 / r : r;
}

static bool is_nan(double x) { return x != x; }
static bool is_inf(double x) { return x == __builtin_inf() || x == -__builtin_inf(); }

/* The 13 significant digits of |x| (not 0, finite) and the decimal
 * exponent of the first: |x| = d.dddddddddddd x 10^e. */
static void digits_of(double x, char d[14], int *e)
{
	if (x < 0)
		x = -x;
	int exp = 0;
	double v = x;
	while (v >= 10.0) {
		v /= 10.0;
		exp++;
	}
	while (v < 1.0) {
		v *= 10.0;
		exp--;
	}
	/* Scale to a 13 digit integer in one step from x, so the rounding
	 * does not accumulate over the loop above. */
	double m = x / pow10d(exp - 12);
	uint64_t n = (uint64_t)(m + 0.5);
	if (n >= 10000000000000ull) {
		n /= 10;
		exp++;
	}
	for (int i = 12; i >= 0; i--) {
		d[i] = (char)('0' + n % 10);
		n /= 10;
	}
	d[13] = 0;
	*e = exp;
}

size_t calc_format(double x, char *out, size_t size)
{
	char d[14], buf[32];
	size_t n = 0;
	if (is_nan(x)) {
		strcpy(buf, "NaN");
	} else if (is_inf(x)) {
		strcpy(buf, x < 0 ? "-Infinity" : "Infinity");
	} else if (x == 0.0) {
		strcpy(buf, "0");
	} else {
		int e, len = 13;
		digits_of(x, d, &e);
		while (len > 1 && d[len - 1] == '0')
			len--;
		if (x < 0)
			buf[n++] = '-';
		if (e >= 21 || e <= -7) {
			buf[n++] = d[0];
			if (len > 1) {
				buf[n++] = '.';
				memcpy(buf + n, d + 1, len - 1);
				n += len - 1;
			}
			n += snprintf(buf + n, sizeof buf - n, "e%c%d", e < 0 ? '-' : '+', e < 0 ? -e : e);
		} else if (e < 0) {
			buf[n++] = '0';
			buf[n++] = '.';
			for (int i = 0; i < -e - 1; i++)
				buf[n++] = '0';
			memcpy(buf + n, d, len);
			n += len;
		} else {
			for (int i = 0; i <= e; i++)
				buf[n++] = i < len ? d[i] : '0';
			if (len > e + 1) {
				buf[n++] = '.';
				memcpy(buf + n, d + e + 1, len - e - 1);
				n += len - e - 1;
			}
		}
		buf[n] = 0;
	}
	n = strlen(buf);
	if (n >= size)
		n = size - 1;
	memcpy(out, buf, n);
	out[n] = 0;
	return n;
}

double calc_parse(const char *s)
{
	double nan = __builtin_nan("");
	if (!*s)
		return 0.0;
	bool neg = *s == '-';
	if (neg)
		s++;
	if (!strcmp(s, "Infinity"))
		return neg ? -__builtin_inf() : __builtin_inf();
	double m = 0.0;
	int frac = 0, e = 0, digits = 0;
	bool point = false;
	for (; *s && *s != 'e'; s++) {
		if (*s == '.' && !point) {
			point = true;
		} else if (*s >= '0' && *s <= '9') {
			m = m * 10.0 + (*s - '0');
			digits++;
			if (point)
				frac++;
		} else {
			return nan;
		}
	}
	if (!digits)
		return nan;
	if (*s == 'e') {
		s++;
		bool eneg = *s == '-';
		if (*s == '-' || *s == '+')
			s++;
		if (!*s)
			return nan;
		for (; *s; s++) {
			if (*s < '0' || *s > '9')
				return nan;
			e = e * 10 + (*s - '0');
		}
		if (eneg)
			e = -e;
	}
	double v = m * pow10d(e - frac);
	return neg ? -v : v;
}

/* Rounded to 13 significant digits, as parseFloat(x.toPrecision(13)). */
static double round13(double x)
{
	char buf[32];
	calc_format(x, buf, sizeof buf);
	return calc_parse(buf);
}

/* The shown form of s: the integer digits grouped in threes with
 * commas, the fraction as it is. An exponent form is written out with
 * no fraction, as the locale formatting with its 0 decimals does; the
 * negative ones are below 1. Anything else is shown as it is. */
static void group(const char *s, char *out)
{
	char digits[40];
	size_t n = 0, ints = 0;
	const char *p = s;
	if (*p == '-')
		out[n++] = *p++;
	while (*p >= '0' && *p <= '9')
		digits[ints++] = *p++;
	const char *frac = *p == '.' ? p : NULL;
	if (frac) {
		p++;
		while (*p >= '0' && *p <= '9')
			p++;
	}
	if (*p == 'e' && ints) {
		int ex = 0;
		bool eneg = p[1] == '-';
		for (p += 2; *p >= '0' && *p <= '9'; p++)
			ex = ex * 10 + (*p - '0');
		ints = 0;
		for (const char *q = s + (s[0] == '-'); *q && *q != 'e'; q++)
			if (*q != '.')
				digits[ints++] = *q;
		if (eneg || ex > 30) {
			ints = 1;
			digits[0] = '0';
		} else {
			while (ints < (size_t)ex + 1)
				digits[ints++] = '0';
		}
		frac = NULL;
	} else if (!ints || *p) {
		strcpy(out, s);
		return;
	}
	for (size_t i = 0; i < ints; i++) {
		if (i && (ints - i) % 3 == 0)
			out[n++] = ',';
		out[n++] = digits[i];
	}
	if (frac && frac[1]) {
		size_t k = (size_t)(p - frac);
		memcpy(out + n, frac, k);
		n += k;
	}
	out[n] = 0;
}

static void update_shown(void)
{
	group(text[0] ? text : previous, shown);
}

void calc_reset(void)
{
	strcpy(text, "0");
	previous[0] = 0;
	op = -1;
	last_key = -1;
	update_shown();
}

static void set_number(double x)
{
	calc_format(x, text, sizeof text);
}

void calc_key(int key)
{
	size_t len = strlen(text);
	if (key >= 0 && key <= UI_CALC_POINT) {
		if (key == UI_CALC_POINT && strchr(text, '.'))
			return;
		if (len >= CALC_MAX)
			return;
		char c = key == UI_CALC_POINT ? '.' : (char)('0' + key);
		if (!strcmp(text, "0") || last_key == UI_CALC_EQUALS) {
			text[0] = c;
			text[1] = 0;
		} else {
			text[len] = c;
			text[len + 1] = 0;
		}
		last_key = key;
		update_shown();
		return;
	}
	last_key = key;
	if (key == UI_CALC_NEGATE) {
		double x = calc_parse(text);
		if (!is_nan(x) && x != 0.0)
			set_number(-x);
		else if (!strcmp(text, "-"))
			strcpy(text, "0");
		else
			strcpy(text, "-");
		update_shown();
		return;
	}
	double cur = calc_parse(text), prev = calc_parse(previous);
	if (op == UI_CALC_ADD)
		set_number(round13(cur + prev));
	else if (op == UI_CALC_SUBTRACT)
		set_number(round13(prev - cur));
	else if (op == UI_CALC_MULTIPLY)
		set_number(round13(prev * cur));
	else if (op == UI_CALC_DIVIDE)
		set_number(round13(prev / cur));
	if (key >= UI_CALC_ADD && key <= UI_CALC_DIVIDE) {
		op = key;
		strcpy(previous, text);
		text[0] = 0;
		update_shown();
		return;
	}
	op = -1;
	previous[0] = 0;
	if (key == UI_CALC_BACK) {
		len = strlen(text);
		if (len)
			text[len - 1] = 0;
		if (!text[0])
			strcpy(text, "0");
	} else if (key == UI_CALC_CLEAR) {
		strcpy(text, "0");
	}
	update_shown();
}

const char *calc_text(void) { return text; }
const char *calc_previous(void) { return previous; }
const char *calc_shown(void) { return shown; }
unsigned calc_length(void) { return (unsigned)strlen(text[0] ? text : previous); }

/* The calculator's state machine, with its number formatting: calc_key
 * takes a key (enum ui_calc_key), calc_text is the number as typed or
 * computed, calc_previous the one before the pending operator, and
 * calc_shown what to display for them, its digits grouped in threes,
 * with calc_length the length of the text it shows (for the font
 * size). */
#ifndef CALC_H
#define CALC_H
#include <stddef.h>

#define CALC_MAX	14

void calc_reset(void);
void calc_key(int key);
const char *calc_text(void);
const char *calc_previous(void);
const char *calc_shown(void);
unsigned calc_length(void);
/* x as JavaScript prints a number rounded to 13 significant digits:
 * the shortest digits, exponent form outside 1e-7 <= |x| < 1e21;
 * "Infinity" and "NaN". Returns the length. */
size_t calc_format(double x, char *out, size_t size);
/* A number as calc_format or the keys write it. NaN for anything else. */
double calc_parse(const char *s);
#endif

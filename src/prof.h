/* Where the processor is, sampled on the tick: prof_start asks for a
 * number of samples and prof_report says where they fell, in blocks of
 * 64 bytes of code, most first. */
#ifndef PROF_H
#define PROF_H
#include <stdint.h>

void prof_start(uint32_t samples);
void prof_sample(void);
void prof_report(void (*say)(const char *fmt, ...));
#endif

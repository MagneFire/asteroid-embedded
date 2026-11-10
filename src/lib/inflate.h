#ifndef INFLATE_H
#define INFLATE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INFLATE_E_HEADER	(-1)
#define INFLATE_E_TRUNCATED	(-2)
#define INFLATE_E_CORRUPT	(-3)
#define INFLATE_E_OVERFLOW	(-4)
#define INFLATE_E_ADLER		(-5)

uint32_t adler32(uint32_t adler, const void *buf, size_t len);
int deflate_decode(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len, size_t *in_used);
int zlib_inflate(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len);
const char *inflate_strerror(int err);
#endif

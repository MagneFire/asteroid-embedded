#ifndef LZ4_H
#define LZ4_H
#include <stddef.h>
#include <stdint.h>

#define LZ4_E_TRUNCATED	(-1)
#define LZ4_E_CORRUPT	(-2)
#define LZ4_E_OVERFLOW	(-3)

int lz4_block_decode(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len);
#endif

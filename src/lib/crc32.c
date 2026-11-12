/* CRC-32 as zlib and the GPT have it, a bit at a time. */
#include <stdint.h>
#include "storage/emmc.h"

uint32_t crc32(uint32_t crc, const void *buf, uint32_t n)
{
	const uint8_t *p = buf;
	crc = ~crc;
	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
	}
	return ~crc;
}

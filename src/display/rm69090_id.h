/* Replies of the RM69090 over SPI. Each reply is read two bytes long,
 * so that one which came a clock late, as at 48 MHz, can be put back
 * together from the bit that spilled into the second byte. */
#ifndef RM69090_ID_H
#define RM69090_ID_H
#include <stdint.h>

enum rm69090_framing { RM69090_UNKNOWN, RM69090_ON_TIME, RM69090_LATE };

/* A reply's byte; an unknown framing is taken as on time. */
uint8_t rm69090_byte(const uint8_t raw[2], enum rm69090_framing f);
/* How the replies to DA, DB and DC give id, or RM69090_UNKNOWN when
 * they do not. */
enum rm69090_framing rm69090_match(const uint8_t raw[3][2], const uint8_t id[3]);
const char *rm69090_framing_name(enum rm69090_framing f);
#endif

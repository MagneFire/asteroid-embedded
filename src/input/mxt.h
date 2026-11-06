/* Atmel maXTouch: what the object table says and what a touch message
 * means, no hardware involved. */
#ifndef MXT_H
#define MXT_H
#include <stdbool.h>
#include <stdint.h>

#define MXT_OBJECT_SIZE		6
#define MXT_MAX_OBJECTS		40
#define MXT_T5_MESSAGE		5
#define MXT_T9_TOUCH		9
#define MXT_T7_POWER		7
#define MXT_T42_SUPPRESS	42
#define MXT_T46_CTE		46
#define MXT_T62_NOISE		62

struct mxt_map {
	uint16_t msg_addr;	/* T5, where messages are read */
	uint8_t msg_size;	/* its length without the checksum */
	uint16_t t9_addr;
	uint8_t t9_size;
	uint8_t t9_first_id;	/* report id of the first finger */
	uint8_t t9_ids;
	uint8_t t42_id;		/* 0 without touch suppression */
	uint16_t t7_addr, t42_addr, t46_addr, t62_addr;
};

struct mxt_touch {
	uint8_t finger;
	bool down;
	uint16_t x, y;
};

/* Fills in the map from n object table entries, false without T5 or T9;
 * the entries get sorted by address. */
bool mxt_parse(uint8_t *table, unsigned n, struct mxt_map *m);
/* Whether the x (or y) range bytes of T9 ask for 12 bits. */
bool mxt_wide(uint8_t range_hi);
/* Decodes a message, false if it is not a touch. x and y are in the
 * chip's range, 12 or 10 bits as wide_x and wide_y say. */
bool mxt_decode(const struct mxt_map *m, const uint8_t *msg, bool wide_x, bool wide_y, struct mxt_touch *t);
/* Whether a message says touch suppression, a palm, has just started. */
bool mxt_palm(const struct mxt_map *m, const uint8_t *msg);
/* The vendor's settings for asleep and awake: entry i of
 * MXT_POWER_WRITES, as the register and its value; false if that object
 * is not on this chip. */
#define MXT_POWER_WRITES	12
bool mxt_power_write(const struct mxt_map *m, unsigned i, bool asleep, uint16_t *addr, uint8_t *val);
#endif

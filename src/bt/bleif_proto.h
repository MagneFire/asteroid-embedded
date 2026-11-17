/* What the Apollo3's BLE controller wants that needs no hardware: the
 * patches each chip revision takes and the changes made to them on the
 * way, the packets of their download and the acknowledgements, and what
 * gets fixed up in what the controller sends. */
#ifndef BLEIF_PROTO_H
#define BLEIF_PROTO_H
#include <stdbool.h>
#include <stdint.h>

/* A patch: type 0xbb RAM code, 0xcc functions, 0xdd NVDS parameters. */
struct bleif_patch {
	uint8_t type;
	uint16_t len;
	const uint8_t *data;
};

extern const struct bleif_patch apollo3_ble_a1_copy, apollo3_ble_a1_perf, apollo3_ble_a1_function, apollo3_ble_a1_nvds,
	apollo3_ble_b0_perf, apollo3_ble_b0_function, apollo3_ble_b0_nvds;

/* A revision's patches: on A1 a copy patch goes first and is closed on
 * its own. The INFO1 trims go into `trimmed`, the address into `nvds`.
 * The rest are addresses in the controller's RAM: the 32 kHz clock it
 * measured, the modulation trim and the transmit power level. */
struct bleif_rev {
	const char *name;
	const struct bleif_patch *copy;
	const struct bleif_patch *const *patches;
	unsigned count;
	const struct bleif_patch *trimmed, *nvds;
	uint32_t clock_32k, modex_trim, power_level;
};

/* By MCUCTRL CHIPREV; NULL for a revision without patches. */
const struct bleif_rev *bleif_rev(uint32_t chiprev);

/* Bytes of a patch changed for this chip, applied as it is read out. */
#define BLEIF_EDITS 16
struct bleif_edits {
	unsigned n;
	struct {
		uint16_t off;
		uint8_t val;
	} e[BLEIF_EDITS];
};

void bleif_edit(struct bleif_edits *ed, unsigned off, const uint8_t *v, unsigned n);
void bleif_patch_read(const struct bleif_patch *p, const struct bleif_edits *ed, unsigned off, unsigned n,
		      uint8_t *out);
uint16_t bleif_crc(uint16_t crc, const uint8_t *p, unsigned n);
uint16_t bleif_patch_crc(const struct bleif_patch *p, const struct bleif_edits *ed);
/* The bandgap and retention LDO trims from INFO1, all ones on a part
 * never trimmed. */
void bleif_trims(const struct bleif_patch *p, uint32_t bandgap, uint32_t retention, struct bleif_edits *ed);
/* An NVDS parameter of that length: 0, or -1 if the patch has none. */
int bleif_nvds_set(const struct bleif_patch *p, uint8_t tag, const uint8_t *v, unsigned n, struct bleif_edits *ed);
#define NVDS_BDADDR 0x01
/* The modulation trim in INFO1's third word, 0 for none. */
uint8_t bleif_modex_trim(uint32_t word);
/* An address in display order from the chip's ID registers. */
void bleif_chip_addr(uint32_t id0, uint32_t id1, uint8_t addr[6]);
/* The vendor's address page holds the address LSB first; false when
 * it is erased or zero. */
bool bleif_flash_addr(const uint8_t lsb[6], uint8_t addr[6]);

/* Patch packets: start (with the length), data, CRC; then the end of a
 * copy patch and of all patching, both a start of type 0xee. Each is
 * answered with a 5-byte acknowledgement. */
#define BLEIF_PATCH_CHUNK 128
#define BLEIF_PATCH_START 0xf1
#define BLEIF_PATCH_DATA 0xf2
#define BLEIF_PATCH_CRC 0xf3
#define BLEIF_PATCH_END 0xee
unsigned bleif_patch_packet(uint8_t *out, uint8_t type, uint8_t step, const uint8_t *p, unsigned n);
bool bleif_patch_ack(const uint8_t *r, uint8_t type, uint8_t step);

/* A whole H4 packet as read, type byte first, consistent with itself. */
bool bleif_packet_ok(const uint8_t *p, unsigned n);
/* An event, from its code on, made right before the host sees it. */
void bleif_fix_event(uint8_t *p, unsigned n);
#endif

/* The Apollo3 BLE controller's patches and packets. */
#include <stdio.h>
#include <string.h>
#include "bt/bleif_proto.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static uint32_t le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

int main(void)
{
	/* A1 and B0, by the low byte of CHIPREV only. */
	const struct bleif_rev *a1 = bleif_rev(0x12), *b0 = bleif_rev(0x00012321);
	CHECK(a1 && !strcmp(a1->name, "A1") && a1->copy == &apollo3_ble_a1_copy && a1->count == 3);
	CHECK(b0 && !strcmp(b0->name, "B0") && !b0->copy && b0->count == 3);
	CHECK(!bleif_rev(0x11) && !bleif_rev(0x22) && !bleif_rev(0x31) && !bleif_rev(0));
	CHECK(b0->patches[0]->type == 0xbb && b0->patches[1]->type == 0xcc && b0->patches[2]->type == 0xdd);
	CHECK(b0->trimmed == b0->patches[0] && b0->nvds == b0->patches[2] && a1->trimmed == &apollo3_ble_a1_perf);
	CHECK(b0->clock_32k == 0x20006858 && a1->clock_32k == 0x20006054);
	CHECK(apollo3_ble_b0_perf.len == 0x654 && apollo3_ble_b0_function.len == 0x230 && apollo3_ble_b0_nvds.len == 0xc2);
	CHECK(apollo3_ble_a1_copy.len == 0x912 && apollo3_ble_a1_function.len == 0xd38 && apollo3_ble_a1_nvds.len == 0xbe);

	/* CRC-16/UMTS, and the CRCs the untouched patches are known by. */
	CHECK(bleif_crc(0, (const uint8_t *)"123456789", 9) == 0xfee8);
	CHECK(bleif_patch_crc(&apollo3_ble_b0_function, NULL) == 0x320c);
	CHECK(bleif_patch_crc(&apollo3_ble_a1_copy, NULL) == 0x9516);
	uint8_t all[0x654];
	bleif_patch_read(&apollo3_ble_b0_perf, NULL, 0, sizeof all, all);
	CHECK(bleif_patch_crc(&apollo3_ble_b0_perf, NULL) == bleif_crc(0, all, sizeof all));

	/* Trims into words 2 and 3, byte-swapped; none from an untrimmed part. */
	struct bleif_edits ed = { 0 };
	bleif_trims(&apollo3_ble_b0_perf, 0xffffffff, 0xffffffff, &ed);
	CHECK(ed.n == 0);
	bleif_trims(&apollo3_ble_b0_perf, 0x11223344, 0x00000010, &ed);
	CHECK(ed.n == 8);
	uint8_t w[16];
	bleif_patch_read(&apollo3_ble_b0_perf, &ed, 0, 16, w);
	CHECK(le32(w + 8) == 0x44332211);
	CHECK(le32(w + 12) == ((le32(apollo3_ble_b0_perf.data + 12) & ~0x1f0u) | 0x10000040));
	CHECK(!memcmp(w, apollo3_ble_b0_perf.data, 8));
	/* An edit across a chunk boundary lands in both reads. */
	uint8_t v[4] = { 1, 2, 3, 4 }, c[8];
	struct bleif_edits e2 = { 0 };
	bleif_edit(&e2, 126, v, 4);
	bleif_patch_read(&apollo3_ble_b0_perf, &e2, 124, 4, c);
	bleif_patch_read(&apollo3_ble_b0_perf, &e2, 128, 4, c + 4);
	CHECK(c[2] == 1 && c[3] == 2 && c[4] == 3 && c[5] == 4);
	CHECK(c[0] == apollo3_ble_b0_perf.data[124] && c[6] == apollo3_ble_b0_perf.data[130]);
	CHECK(bleif_patch_crc(&apollo3_ble_b0_perf, &e2) != bleif_patch_crc(&apollo3_ble_b0_perf, NULL));
	bleif_edit(&e2, 127, v + 3, 1);
	CHECK(e2.n == 4);

	/* The address goes in least significant byte first, after the magic
	 * and the parameter's header. */
	uint8_t addr[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff }, nv[13];
	struct bleif_edits e3 = { 0 };
	CHECK(!memcmp(apollo3_ble_b0_nvds.data, "NVDS", 4));
	CHECK(!bleif_nvds_set(&apollo3_ble_b0_nvds, NVDS_BDADDR, addr, 6, &e3));
	bleif_patch_read(&apollo3_ble_b0_nvds, &e3, 0, sizeof nv, nv);
	CHECK(nv[4] == 0x01 && nv[6] == 6 && !memcmp(nv + 7, addr, 6));
	CHECK(bleif_nvds_set(&apollo3_ble_b0_nvds, NVDS_BDADDR, addr, 5, &e3) == -1);
	CHECK(bleif_nvds_set(&apollo3_ble_b0_nvds, 0x7f, addr, 1, &e3) == -1);
	CHECK(!bleif_nvds_set(&apollo3_ble_a1_nvds, 0x2d, addr, 1, &e3));

	/* Display order from CHIPID1 and the middle of CHIPID0. */
	uint8_t a[6];
	bleif_chip_addr(0x00abcd00, 0x44332211, a);
	CHECK(!memcmp(a, "\xab\xcd\x44\x33\x22\x11", 6));
	CHECK(bleif_flash_addr((const uint8_t *)"\xdd\x8f\x57\xbe\x8a\x38", a));
	CHECK(!memcmp(a, "\x38\x8a\xbe\x57\x8f\xdd", 6));
	CHECK(!bleif_flash_addr((const uint8_t *)"\xff\xff\xff\xff\xff\xff", a));
	CHECK(!bleif_flash_addr((const uint8_t *)"\0\0\0\0\0\0", a));
	CHECK(!memcmp(a, "\x38\x8a\xbe\x57\x8f\xdd", 6));

	CHECK(bleif_modex_trim(0x18240634) == 0x34);
	CHECK(!bleif_modex_trim(0x18240610) && !bleif_modex_trim(0x18240641) && !bleif_modex_trim(0x18250634));

	uint8_t pk[4 + BLEIF_PATCH_CHUNK];
	unsigned n = bleif_patch_packet(pk, 0xdd, BLEIF_PATCH_START, (const uint8_t *)"\xc2\x00", 2);
	CHECK(n == 6 && !memcmp(pk, "\x01\xdd\xf1\x02\xc2\x00", 6));
	n = bleif_patch_packet(pk, BLEIF_PATCH_END, BLEIF_PATCH_START, (const uint8_t *)"\0\0", 2);
	CHECK(n == 6 && !memcmp(pk, "\x01\xee\xf1\x02\x00\x00", 6));
	CHECK(bleif_patch_ack((const uint8_t *)"\x04\xdd\xf2\x01\x00", 0xdd, BLEIF_PATCH_DATA));
	CHECK(!bleif_patch_ack((const uint8_t *)"\x04\xdd\xf2\x01\x01", 0xdd, BLEIF_PATCH_DATA));
	CHECK(!bleif_patch_ack((const uint8_t *)"\x04\xcc\xf2\x01\x00", 0xdd, BLEIF_PATCH_DATA));

	CHECK(bleif_packet_ok((const uint8_t *)"\x04\x0e\x04\x01\x03\x0c\x00", 7));
	CHECK(!bleif_packet_ok((const uint8_t *)"\x04\x0e\x04\x01\x03\x0c", 6));
	CHECK(bleif_packet_ok((const uint8_t *)"\x02\x01\x20\x02\x00\xaa\xbb", 7));
	CHECK(!bleif_packet_ok((const uint8_t *)"\x02\x01\x20\x03\x00\xaa\xbb", 7));
	CHECK(!bleif_packet_ok((const uint8_t *)"\x01\x03\x0c\x00", 4) && !bleif_packet_ok(NULL, 0));

	/* Read Local Supported Features gets LE and no BR/EDR; nothing else
	 * changes. */
	uint8_t ev[14] = { 0x0e, 0x0c, 0x01, 0x03, 0x10, 0x00 };
	bleif_fix_event(ev, sizeof ev);
	CHECK(ev[10] == 0x60);
	uint8_t ev2[14] = { 0x0e, 0x0c, 0x01, 0x03, 0x10, 0x01 };
	bleif_fix_event(ev2, sizeof ev2);
	CHECK(ev2[10] == 0);
	uint8_t ev3[14] = { 0x0e, 0x0c, 0x01, 0x09, 0x10, 0x00 };
	bleif_fix_event(ev3, sizeof ev3);
	CHECK(ev3[10] == 0);

	if (failures)
		printf("bleif_test: %d failures\n", failures);
	return failures != 0;
}

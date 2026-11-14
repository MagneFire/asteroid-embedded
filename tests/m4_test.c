/* The M4 hub's protocol helpers. */
#include <stdio.h>
#include <string.h>
#include "sensors/m4_proto.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

/* A hub's passive bank behind a controller that refuses reads longer
 * than its FIFO, as smelt's does, and fails at one offset when asked. */
static uint8_t bank[148];
static unsigned pieces, piece_offset[16], piece_n[16];
static int fail_at = -1;

static int fake_piece(void *ctx, uint8_t b, uint8_t offset, uint8_t *data, unsigned n)
{
	if (pieces < 16) {
		piece_offset[pieces] = offset;
		piece_n[pieces] = n;
	}
	pieces++;
	if (n > M4_READ_MAX)
		return -3;
	if (offset == fail_at)
		return -1;
	memcpy(data, bank + offset, n);
	return 0;
}

static int fake_read(void *ctx, uint8_t b, uint8_t offset, uint8_t *data, unsigned n)
{
	return m4_read(fake_piece, ctx, b, offset, data, n);
}

static void put_slot(unsigned i, uint32_t t, uint8_t bpm, uint8_t confidence)
{
	m4_put_u32(t, bank + M4_PASSIVE_TIME + 4 * i);
	bank[M4_PASSIVE_HEART + 2 * i] = bpm;
	bank[M4_PASSIVE_CONFIDENCE + i] = confidence;
}

int main(void)
{
	uint8_t a[5];
	m4_bl_address(M4_FLASH_BASE, a);
	CHECK(!memcmp(a, "\x08\x00\x00\x00\x08", 5));
	m4_bl_address(m4_smelt.mark_addr, a);
	CHECK(m4_smelt.start == 0x08000000 && m4_smelt.mark_addr == 0x0807fff8);
	CHECK(!memcmp(a, "\x08\x07\xff\xf8", 4) && a[4] == (0x08 ^ 0x07 ^ 0xff ^ 0xf8));

	uint8_t good[8] = { 0xde, 0xc0, 0xce, 0x0a, 0, 0, 0, 0 }, bad[8] = { 0xde, 0xc0, 0xce, 0x0b, 0, 0, 0, 0 };
	CHECK(m4_barker_ok(&m4_smelt, good));
	CHECK(!m4_barker_ok(&m4_smelt, bad));
	/* The older hub: firmware from the middle of the flash, a mark of
	 * four bytes at its very end, so what is before them does not count. */
	uint8_t tail[4] = { 0xde, 0xc0, 0xce, 0x0a };
	CHECK(m4_minnow.start == 0x08040000 && m4_minnow.mark_addr == 0x0807fffc);
	CHECK(m4_barker_ok(&m4_minnow, tail) && !m4_barker_ok(&m4_minnow, bad));
	/* Its interrupts: gesture is bit 7 of the first byte, still and
	 * motion bits 0 and 1 of the second. */
	CHECK(m4_irq_enable(&m4_minnow, true, false) == 0x0380 && m4_irq_enable(&m4_minnow, false, false) == 0x0300);
	CHECK(m4_irq_enable(&m4_minnow, false, true) == 0x4300 && m4_irq_enable(&m4_smelt, false, true) == 0xb);
	CHECK(m4_irq_enable(&m4_smelt, true, false) == 7);
	CHECK(m4_irq_wake(&m4_minnow, 0x4000) == M4_WAKE_PASSIVE);
	CHECK(m4_irq_wake(&m4_minnow, 0x0080) == M4_WAKE_GESTURE);
	CHECK(m4_irq_wake(&m4_minnow, 0x0300) == (M4_WAKE_STILL | M4_WAKE_MOTION));
	CHECK(m4_irq_wake(&m4_minnow, 0x020000) == M4_WAKE_OTHER && !m4_irq_wake(&m4_minnow, 0));
	CHECK(m4_irq_wake(&m4_smelt, 5) == 5);

	uint8_t n[4] = { 0x78, 0x56, 0x34, 0x12 };
	CHECK(m4_u32(n) == 0x12345678);

	CHECK(m4_is_tilt(M4_WAKE_GESTURE, 1, 0));
	CHECK(!m4_is_tilt(M4_WAKE_GESTURE, 1, 1));
	CHECK(m4_is_tilt(M4_WAKE_GESTURE, 2, 3));
	/* A tap is not a tilt, nor a gesture left from before. */
	CHECK(!m4_is_tilt(M4_WAKE_GESTURE, 3, 0));
	CHECK(!m4_is_tilt(0, 1, 0));

	/* Still and motion are always followed, the gesture only when wanted. */
	CHECK(m4_wake_mask(true, false) == 7);
	CHECK(m4_wake_mask(false, false) == 3);

	/* The passive ring: wrapped, with empty slots, a slot without a
	 * rate, one without confidence and one already taken. */
	uint8_t times[4 * M4_PASSIVE_SLOTS] = { 0 }, heart[2 * M4_PASSIVE_SLOTS] = { 0 }, conf[M4_PASSIVE_SLOTS] = { 0 };
	struct m4_passive_reading r[M4_PASSIVE_SLOTS];
	const uint32_t ts[] = { 1900, 2200, 0, 1000, 1300, 1600 };
	const uint8_t bpms[] = { 70, 0, 0, 61, 65, 44 }, cs[] = { 3, 3, 0, 3, 0, 2 };
	for (unsigned i = 0; i < 6; i++) {
		m4_put_u32(ts[i], times + 4 * i);
		heart[2 * i] = bpms[i];
		conf[i] = cs[i];
	}
	heart[2 * 5 + 1] = 1;
	unsigned got = m4_passive_readings(times, heart, conf, 0, r);
	CHECK(got == 3 && r[0].t == 1000 && r[0].bpm == 61 && r[1].t == 1600 && r[1].bpm == 255 && r[2].t == 1900);
	got = m4_passive_readings(times, heart, conf, 1600, r);
	CHECK(got == 1 && r[0].t == 1900 && r[0].bpm == 70);

	uint8_t le[4];
	m4_put_u16(M4_STILL_TIMEOUT_S, le);
	CHECK(le[0] == 0x58 && le[1] == 0x02);
	m4_put_u32(0x12345678, le);
	CHECK(m4_u32(le) == 0x12345678 && le[0] == 0x78);

	/* The pedometer block starts at activity; the count sits two bytes in. */
	uint8_t block[M4_PEDOMETER_BLOCK_SIZE] = { 1, 0, 0x78, 0x56, 0x34, 0x12, [14] = 0x21, 0x43, 0x65, 0x07 };
	CHECK(m4_block_steps(block) == 0x12345678);
	CHECK(m4_block_distance(block) == 0x07654321);

	uint8_t magic[4] = { 0xef, 0xbe, 0xad, 0xde }, healthy[4] = { 0xff, 0xff, 0xff, 0xff }, zeros[4] = { 0 };
	CHECK(m4_panicked(magic));
	CHECK(!m4_panicked(healthy));
	CHECK(!m4_panicked(zeros));

	/* A read longer than the controller takes goes in pieces, each from
	 * where the last ended; the pedometer block still goes in one. */
	CHECK(M4_PEDOMETER_BLOCK_SIZE <= M4_READ_MAX);
	for (unsigned i = 0; i < sizeof bank; i++)
		bank[i] = (uint8_t)i;
	uint8_t got48[48];
	pieces = 0;
	CHECK(!fake_read(NULL, 14, M4_PASSIVE_TIME, got48, sizeof got48));
	CHECK(pieces == 2 && piece_offset[0] == 0x04 && piece_n[0] == 32 && piece_offset[1] == 0x24 && piece_n[1] == 16);
	CHECK(!memcmp(got48, bank + M4_PASSIVE_TIME, sizeof got48));
	fail_at = 0x24;
	CHECK(fake_read(NULL, 14, M4_PASSIVE_TIME, got48, sizeof got48));
	fail_at = -1;

	/* The passive ring: not before five minutes, unless the hub says it
	 * is full. */
	memset(bank, 0, sizeof bank);
	put_slot(0, 3000, 62, 3);
	put_slot(1, 3300, 64, 2);
	struct m4_passive p = { .t = 1000 };
	pieces = 0;
	CHECK(m4_passive_read(&p, 1000 + M4_PASSIVE_MS - 1, fake_read, NULL, 14, r) == 0 && !pieces);
	p.due = true;
	CHECK(m4_passive_read(&p, 2000, fake_read, NULL, 14, r) == 2 && r[0].t == 3000 && r[1].bpm == 64);
	CHECK(!p.due && p.after == 3300 && p.t == 2000);
	CHECK(m4_passive_read(&p, 2000 + M4_PASSIVE_MS, fake_read, NULL, 14, r) == 0);

	/* A failure in any of its reads gives nothing and waits for the next
	 * five minutes, even when the hub said it was full. */
	put_slot(2, 3600, 66, 3);
	const int fails_at[] = { 0x24, M4_PASSIVE_HEART, M4_PASSIVE_CONFIDENCE };
	uint32_t now = 2000 + M4_PASSIVE_MS;
	for (unsigned i = 0; i < 3; i++) {
		fail_at = fails_at[i];
		p.due = true;
		now += 10;
		CHECK(m4_passive_read(&p, now, fake_read, NULL, 14, r) == -1);
		CHECK(!p.due && p.after == 3300);
		fail_at = -1;
		pieces = 0;
		CHECK(m4_passive_read(&p, now + 1000, fake_read, NULL, 14, r) == 0 && !pieces);
	}
	CHECK(m4_passive_read(&p, now + M4_PASSIVE_MS, fake_read, NULL, 14, r) == 1 && r[0].t == 3600);

	/* A restarted hub starts a new ring, and its clock may be behind. */
	p = (struct m4_passive){ 0 };
	memset(bank, 0, sizeof bank);
	put_slot(0, 2700, 58, 3);
	p.due = true;
	CHECK(m4_passive_read(&p, now, fake_read, NULL, 14, r) == 1 && r[0].t == 2700);

	puts(failures ? "m4_test: FAILED" : "m4_test: ok");
	return failures != 0;
}

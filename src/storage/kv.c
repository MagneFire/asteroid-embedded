/* Small key/value records in the store, power-loss safe: two slots of
 * KV_SLOT_SIZE at the start of the store, each a 32-byte header (magic,
 * version, header size, sequence number, payload length, CRC-32 over
 * header and payload) and the records {key length, 0, value length (le16),
 * key, value}. The newest slot with a valid CRC wins. A change rewrites
 * the other slot with the next sequence number and reads it back, so a
 * write cut short leaves the previous slot as the newest valid one. */
#include <string.h>
#include "asteroid.h"
#include "storage.h"

#define KV_MAGIC		0x564b4541u
#define KV_VERSION		1u
#define KV_HDR			32u

static uint8_t img[KV_SLOT_SIZE] __attribute__((aligned(64)));
static uint8_t chk[KV_SLOT_SIZE] __attribute__((aligned(64)));
static bool loaded;
static uint32_t loaded_gen;
static int cur = -1;
static uint32_t seq, len;

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static uint32_t slot_crc(const uint8_t *s, uint32_t n)
{
	uint8_t h[KV_HDR];
	memcpy(h, s, KV_HDR);
	wr32(h + 20, 0);
	return crc32(crc32(0, h, KV_HDR), s + KV_HDR, n);
}

static bool slot_valid(const uint8_t *s, uint32_t *sq, uint32_t *n)
{
	if (rd32(s) != KV_MAGIC || (rd32(s + 4) & 0xffff) != KV_VERSION || rd32(s + 4) >> 16 != KV_HDR)
		return false;
	uint32_t l = rd32(s + 12);
	if (l > KV_SLOT_SIZE - KV_HDR || slot_crc(s, l) != rd32(s + 20))
		return false;
	for (uint32_t o = 0; o < l;) {
		if (l - o < 4 || !s[KV_HDR + o] || s[KV_HDR + o] > KV_KEY_MAX)
			return false;
		uint32_t rec = 4u + s[KV_HDR + o] + (s[KV_HDR + o + 2] | s[KV_HDR + o + 3] << 8);
		if (rec > l - o)
			return false;
		o += rec;
	}
	*sq = rd32(s + 8);
	*n = l;
	return true;
}

int kv_load(void)
{
	int r = store_open();
	if (r)
		return r;
	uint32_t sq[2], n[2];
	bool ok[2];
	for (int i = 0; i < 2; i++) {
		if ((r = store_read(i * KV_SLOT_SIZE, i ? chk : img, KV_SLOT_SIZE)))
			return r;
	}
	ok[0] = slot_valid(img, &sq[0], &n[0]);
	ok[1] = slot_valid(chk, &sq[1], &n[1]);
	if (ok[0] && ok[1])
		cur = (int32_t)(sq[1] - sq[0]) > 0 ? 1 : 0;
	else
		cur = ok[0] ? 0 : ok[1] ? 1 : -1;
	if (cur == 1)
		memcpy(img, chk, KV_SLOT_SIZE);
	if (cur < 0) {
		memset(img, 0, KV_SLOT_SIZE);
		seq = len = 0;
	} else {
		seq = sq[cur];
		len = n[cur];
	}
	loaded = true;
	loaded_gen = emmc_card()->generation;
	log_printf("kv: slots %s/%s, using %d, seq %lu, %lu bytes\n", ok[0] ? "valid" : "empty", ok[1] ? "valid" : "empty", cur, seq,
		   len);
	return 0;
}

static int find(const char *key, uint32_t *at, uint32_t *rec)
{
	size_t kl = strlen(key);
	for (uint32_t o = 0; o < len;) {
		const uint8_t *p = img + KV_HDR + o;
		uint32_t r = 4u + p[0] + (p[2] | p[3] << 8);
		if (p[0] == kl && !memcmp(p + 4, key, kl)) {
			*at = o;
			*rec = r;
			return 0;
		}
		o += r;
	}
	return -1;
}

int kv_get(const char *key, void *buf, unsigned size)
{
	if ((!loaded || loaded_gen != emmc_card()->generation) && kv_load())
		return -1;
	uint32_t at, rec;
	if (find(key, &at, &rec))
		return -2;
	const uint8_t *p = img + KV_HDR + at;
	unsigned vl = (unsigned)(p[2] | p[3] << 8);
	memcpy(buf, p + 4 + p[0], vl < size ? vl : size);
	return (int)vl;
}

int kv_each(kv_each_fn fn, void *ctx)
{
	if ((!loaded || loaded_gen != emmc_card()->generation) && kv_load())
		return -1;
	for (uint32_t o = 0; o < len;) {
		const uint8_t *p = img + KV_HDR + o;
		char key[KV_KEY_MAX + 1];
		memcpy(key, p + 4, p[0]);
		key[p[0]] = 0;
		unsigned vl = (unsigned)(p[2] | p[3] << 8);
		fn(key, p + 4 + p[0], vl, ctx);
		o += 4u + p[0] + vl;
	}
	return 0;
}

uint32_t kv_seq(void)
{
	return seq;
}

static int commit(void)
{
	int next = cur == 0 ? 1 : 0;
	wr32(img, KV_MAGIC);
	wr32(img + 4, KV_VERSION | KV_HDR << 16);
	wr32(img + 8, seq + 1);
	wr32(img + 12, len);
	memset(img + 16, 0, 4);
	memset(img + 24, 0, KV_HDR - 24);
	wr32(img + 20, slot_crc(img, len));
	uint32_t bytes = (KV_HDR + len + EMMC_BLOCK - 1) & ~(EMMC_BLOCK - 1);
	memset(img + KV_HDR + len, 0, bytes - KV_HDR - len);
	int r = store_write(next * KV_SLOT_SIZE, img, bytes);
	if (!r)
		r = store_read(next * KV_SLOT_SIZE, chk, bytes);
	if (!r && memcmp(chk, img, bytes))
		r = -20;
	if (r) {
		log_printf("kv: commit to slot %d failed: %d\n", next, r);
		return r;
	}
	cur = next;
	seq++;
	return 0;
}

static int reload_after_failure(int r)
{
	loaded = false;
	kv_load();
	return r;
}

int kv_set(const char *key, const void *val, unsigned vlen)
{
	size_t kl = strlen(key);
	if (!kl || kl > KV_KEY_MAX || vlen > KV_VALUE_MAX)
		return -3;
	if ((!loaded || loaded_gen != emmc_card()->generation) && kv_load())
		return -1;
	if (!store_info()->writable)
		return -4;
	uint32_t at, rec;
	if (!find(key, &at, &rec)) {
		memmove(img + KV_HDR + at, img + KV_HDR + at + rec, len - at - rec);
		len -= rec;
	}
	if (KV_HDR + len + 4 + kl + vlen > KV_SLOT_SIZE)
		return reload_after_failure(-5);
	uint8_t *p = img + KV_HDR + len;
	p[0] = (uint8_t)kl;
	p[1] = 0;
	p[2] = (uint8_t)vlen;
	p[3] = (uint8_t)(vlen >> 8);
	memcpy(p + 4, key, kl);
	memcpy(p + 4 + kl, val, vlen);
	len += 4 + (uint32_t)kl + vlen;
	int r = commit();
	return r ? reload_after_failure(r) : 0;
}

int kv_del(const char *key)
{
	if ((!loaded || loaded_gen != emmc_card()->generation) && kv_load())
		return -1;
	if (!store_info()->writable)
		return -4;
	uint32_t at, rec;
	if (find(key, &at, &rec))
		return -2;
	memmove(img + KV_HDR + at, img + KV_HDR + at + rec, len - at - rec);
	len -= rec;
	int r = commit();
	return r ? reload_after_failure(r) : 0;
}

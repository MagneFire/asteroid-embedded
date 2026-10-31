/* Bond keys and the BD_ADDR. The host's ble_store callbacks work on RAM
 * tables; every change saves them as one blob in the eMMC key/value
 * store, and the BD_ADDR is a second key. Without a writable store the
 * values only live in RAM. */
#include <string.h>
#include "asteroid.h"
#include "host/ble_hs.h"
#include "mbedtls/aes.h"
#include "bt.h"
#include "storage/storage.h"

#define MAX_BONDS	MYNEWT_VAL(BLE_STORE_MAX_BONDS)
#define MAX_CCCDS	MYNEWT_VAL(BLE_STORE_MAX_CCCDS)
#define KEY_BONDS	"bt.bonds"
#define KEY_ADDR	"bt.addr"
#define BONDS_MAGIC	0x42543031u

static struct bonds {
	uint32_t magic, size;
	uint32_t n_our, n_peer, n_cccd;
	struct ble_store_value_sec our[MAX_BONDS], peer[MAX_BONDS];
	struct ble_store_value_cccd cccd[MAX_CCCDS];
} bonds;

static struct {
	char key[16];
	uint32_t len;
	uint8_t data[sizeof(struct bonds)];
} ram[2];

static int ram_load(const char *key, void *buf, unsigned len)
{
	for (unsigned i = 0; i < ARRAY_SIZE(ram); i++)
		if (ram[i].len && !strcmp(ram[i].key, key)) {
			unsigned n = ram[i].len < len ? ram[i].len : len;
			memcpy(buf, ram[i].data, n);
			return (int)n;
		}
	return -1;
}

static int ram_save(const char *key, const void *buf, unsigned len)
{
	unsigned free_slot = ARRAY_SIZE(ram);
	if (len > sizeof ram[0].data || strlen(key) >= sizeof ram[0].key)
		return -1;
	for (unsigned i = 0; i < ARRAY_SIZE(ram); i++) {
		if (ram[i].len && !strcmp(ram[i].key, key)) {
			free_slot = i;
			break;
		}
		if (!ram[i].len && free_slot == ARRAY_SIZE(ram))
			free_slot = i;
	}
	if (free_slot == ARRAY_SIZE(ram))
		return -1;
	memcpy(ram[free_slot].key, key, strlen(key) + 1);
	memcpy(ram[free_slot].data, buf, len);
	ram[free_slot].len = len;
	return 0;
}

static const struct bt_kv ram_kv = { "ram", ram_load, ram_save };

static int emmc_load(const char *key, void *buf, unsigned len)
{
	int n = kv_get(key, buf, len);
	return n < 0 ? -1 : (unsigned)n < len ? n : (int)len;
}

static int emmc_save(const char *key, const void *buf, unsigned len)
{
	return kv_set(key, buf, len) ? -1 : 0;
}

static const struct bt_kv emmc_kv = { "emmc", emmc_load, emmc_save };
static const struct bt_kv *kv;

_Static_assert(sizeof(struct bonds) <= KV_VALUE_MAX, "bonds do not fit in one key/value entry");

static const struct bt_kv *backend(void)
{
	if (!kv)
		kv = !kv_load() && store_info()->writable ? &emmc_kv : &ram_kv;
	return kv;
}

const char *bt_kv_name(void)
{
	return backend()->name;
}

int bt_addr_load(uint8_t addr[6])
{
	return backend()->load(KEY_ADDR, addr, 6) == 6 ? 0 : -1;
}

int bt_addr_save(const uint8_t addr[6])
{
	return backend()->save(KEY_ADDR, addr, 6);
}

static int persist(void)
{
	bonds.magic = BONDS_MAGIC;
	bonds.size = sizeof bonds;
	return backend()->save(KEY_BONDS, &bonds, sizeof bonds) ? BLE_HS_ESTORE_FAIL : 0;
}

static bool any_addr(const ble_addr_t *a)
{
	return !ble_addr_cmp(a, BLE_ADDR_ANY);
}

static int find_sec(const struct ble_store_key_sec *key, const struct ble_store_value_sec *v, uint32_t n)
{
	if (any_addr(&key->peer_addr))
		return key->idx < n ? key->idx : -1;
	if (key->idx)
		return -1;
	for (uint32_t i = 0; i < n; i++)
		if (!ble_addr_cmp(&v[i].peer_addr, &key->peer_addr))
			return (int)i;
	return -1;
}

static int find_cccd(const struct ble_store_key_cccd *key)
{
	unsigned skip = key->idx;
	for (uint32_t i = 0; i < bonds.n_cccd; i++) {
		const struct ble_store_value_cccd *c = &bonds.cccd[i];
		if (!any_addr(&key->peer_addr) && ble_addr_cmp(&c->peer_addr, &key->peer_addr))
			continue;
		if (key->chr_val_handle && c->chr_val_handle != key->chr_val_handle)
			continue;
		if (skip) {
			skip--;
			continue;
		}
		return (int)i;
	}
	return -1;
}

static void remove_at(void *base, size_t size, uint32_t *n, int idx)
{
	uint8_t *p = (uint8_t *)base + (size_t)idx * size;
	(*n)--;
	memmove(p, p + size, (*n - (uint32_t)idx) * size);
}

static int bond_read(int type, const union ble_store_key *key, union ble_store_value *val)
{
	int i;
	switch (type) {
	case BLE_STORE_OBJ_TYPE_OUR_SEC:
		if ((i = find_sec(&key->sec, bonds.our, bonds.n_our)) < 0)
			return BLE_HS_ENOENT;
		val->sec = bonds.our[i];
		return 0;
	case BLE_STORE_OBJ_TYPE_PEER_SEC:
		if ((i = find_sec(&key->sec, bonds.peer, bonds.n_peer)) < 0)
			return BLE_HS_ENOENT;
		val->sec = bonds.peer[i];
		return 0;
	case BLE_STORE_OBJ_TYPE_CCCD:
		if ((i = find_cccd(&key->cccd)) < 0)
			return BLE_HS_ENOENT;
		val->cccd = bonds.cccd[i];
		return 0;
	default:
		return BLE_HS_ENOTSUP;
	}
}

static int write_sec(struct ble_store_value_sec *table, uint32_t *n, int type, const union ble_store_value *val)
{
	struct ble_store_key_sec key;
	ble_store_key_from_value_sec(&key, &val->sec);
	int i = find_sec(&key, table, *n);
	if (i < 0) {
		if (*n >= MAX_BONDS) {
			ble_store_overflow_event(type, val);
			i = find_sec(&key, table, *n);
			if (i < 0 && *n >= MAX_BONDS)
				return BLE_HS_ESTORE_CAP;
		}
		if (i < 0)
			i = (int)(*n)++;
	}
	table[i] = val->sec;
	return persist();
}

static int bond_write(int type, const union ble_store_value *val)
{
	switch (type) {
	case BLE_STORE_OBJ_TYPE_OUR_SEC:
		return write_sec(bonds.our, &bonds.n_our, type, val);
	case BLE_STORE_OBJ_TYPE_PEER_SEC:
		return write_sec(bonds.peer, &bonds.n_peer, type, val);
	case BLE_STORE_OBJ_TYPE_CCCD: {
		struct ble_store_key_cccd key;
		ble_store_key_from_value_cccd(&key, &val->cccd);
		int i = find_cccd(&key);
		if (i < 0) {
			if (bonds.n_cccd >= MAX_CCCDS)
				return BLE_HS_ESTORE_CAP;
			i = (int)bonds.n_cccd++;
		}
		bonds.cccd[i] = val->cccd;
		return persist();
	}
	default:
		return BLE_HS_ENOTSUP;
	}
}

static int bond_delete(int type, const union ble_store_key *key)
{
	int i;
	switch (type) {
	case BLE_STORE_OBJ_TYPE_OUR_SEC:
		if ((i = find_sec(&key->sec, bonds.our, bonds.n_our)) < 0)
			return BLE_HS_ENOENT;
		remove_at(bonds.our, sizeof bonds.our[0], &bonds.n_our, i);
		break;
	case BLE_STORE_OBJ_TYPE_PEER_SEC:
		if ((i = find_sec(&key->sec, bonds.peer, bonds.n_peer)) < 0)
			return BLE_HS_ENOENT;
		remove_at(bonds.peer, sizeof bonds.peer[0], &bonds.n_peer, i);
		break;
	case BLE_STORE_OBJ_TYPE_CCCD:
		if ((i = find_cccd(&key->cccd)) < 0)
			return BLE_HS_ENOENT;
		remove_at(bonds.cccd, sizeof bonds.cccd[0], &bonds.n_cccd, i);
		break;
	default:
		return BLE_HS_ENOTSUP;
	}
	return persist();
}

void bt_store_init(void)
{
	struct bonds b;
	memset(&bonds, 0, sizeof bonds);
	if (backend()->load(KEY_BONDS, &b, sizeof b) == (int)sizeof b && b.magic == BONDS_MAGIC && b.size == sizeof b &&
	    b.n_our <= MAX_BONDS && b.n_peer <= MAX_BONDS && b.n_cccd <= MAX_CCCDS)
		bonds = b;
	ble_hs_cfg.store_read_cb = bond_read;
	ble_hs_cfg.store_write_cb = bond_write;
	ble_hs_cfg.store_delete_cb = bond_delete;
	ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
	log_printf("bt: key store \"%s\", %lu bonds\n", backend()->name, bonds.n_peer);
}

int bt_store_clear(void)
{
	memset(&bonds, 0, sizeof bonds);
	return persist();
}

bool bt_store_bonded(void)
{
	return bonds.n_peer > 0;
}

static bool rpa_matches(const uint8_t irk[16], const uint8_t rpa[6])
{
	uint8_t key[16], in[16] = { 0 }, out[16];
	for (int i = 0; i < 16; i++)
		key[i] = irk[15 - i];
	in[13] = rpa[5];
	in[14] = rpa[4];
	in[15] = rpa[3];
	mbedtls_aes_context ctx;
	mbedtls_aes_init(&ctx);
	mbedtls_aes_setkey_enc(&ctx, key, 128);
	int r = mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in, out);
	mbedtls_aes_free(&ctx);
	return !r && out[13] == rpa[2] && out[14] == rpa[1] && out[15] == rpa[0];
}

bool bt_store_resolve(const uint8_t rpa[6], uint8_t *type, uint8_t addr[6])
{
	if ((rpa[5] >> 6) != 1)
		return false;
	for (uint32_t i = 0; i < bonds.n_peer; i++)
		if (bonds.peer[i].irk_present && rpa_matches(bonds.peer[i].irk, rpa)) {
			*type = bonds.peer[i].peer_addr.type;
			memcpy(addr, bonds.peer[i].peer_addr.val, 6);
			return true;
		}
	return false;
}

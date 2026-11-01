/* Just enough of a flattened device tree reader to look at the tree the
 * bootloader hands over: walk the structure block for one property. */
#include <string.h>
#include "fdt.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9
#define FDT_MAX_SIZE	0x100000

static uint32_t be32(const void *p)
{
	const uint8_t *b = p;
	return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

bool fdt_valid(const void *fdt)
{
	if (!fdt || ((uintptr_t)fdt & 3))
		return false;
	uint32_t size = be32((const uint8_t *)fdt + 4);
	return be32(fdt) == FDT_MAGIC && size >= 40 && size <= FDT_MAX_SIZE;
}

/* Whether the node name (possibly with a @unit) is the path component
 * comp of length n; a component without @ matches any unit. */
static bool name_is(const char *name, const char *comp, size_t n)
{
	if (strncmp(name, comp, n))
		return false;
	return !name[n] || (name[n] == '@' && !memchr(comp, '@', n));
}

const void *fdt_prop(const void *fdt, const char *path, const char *name, uint32_t *len)
{
	if (!fdt_valid(fdt) || path[0] != '/')
		return NULL;
	const uint8_t *base = fdt;
	uint32_t size = be32(base + 4);
	const uint8_t *p = base + be32(base + 8), *end = base + size;
	const char *strings = (const char *)base + be32(base + 12);
	const char *comp[8];
	size_t comp_len[8];
	int ncomp = 0;
	for (const char *c = path + 1; *c && ncomp < 8;) {
		const char *slash = strchr(c, '/');
		size_t n = slash ? (size_t)(slash - c) : strlen(c);
		comp[ncomp] = c;
		comp_len[ncomp++] = n;
		c += n + (slash ? 1 : 0);
	}
	int depth = -1, matched = -1;
	while (p + 4 <= end) {
		uint32_t tok = be32(p);
		p += 4;
		if (tok == FDT_BEGIN_NODE) {
			const char *node = (const char *)p;
			size_t n = strnlen(node, (size_t)(end - p));
			p += (n + 1 + 3) & ~3u;
			depth++;
			if (depth == 0)
				matched = 0;
			else if (matched == depth - 1 && depth <= ncomp && name_is(node, comp[depth - 1], comp_len[depth - 1]))
				matched = depth;
		} else if (tok == FDT_END_NODE) {
			if (matched == depth)
				matched--;
			depth--;
		} else if (tok == FDT_PROP) {
			if (p + 8 > end)
				return NULL;
			uint32_t plen = be32(p), nameoff = be32(p + 4);
			const uint8_t *val = p + 8;
			p = val + ((plen + 3) & ~3u);
			if (p > end)
				return NULL;
			if (matched == depth && depth == ncomp && !strcmp(strings + nameoff, name)) {
				if (len)
					*len = plen;
				return val;
			}
		} else if (tok != FDT_NOP) {
			break;
		}
	}
	return NULL;
}

bool fdt_strlist_has(const char *list, uint32_t len, const char *s)
{
	for (uint32_t i = 0; i < len;) {
		if (!strcmp(list + i, s))
			return true;
		i += strnlen(list + i, len - i) + 1;
	}
	return false;
}

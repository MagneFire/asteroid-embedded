/* The memory image the SGX530's microkernel starts from. The watch's
 * own software builds it in libsrv_init.so: it copies four programs for
 * the GPU's processor and five small ones for its state processor out
 * of that file, writes the addresses they need into them, and fills a
 * control block. This does the same from the same file, so the bytes in
 * GPU memory are the ones the vendor's driver would have put there. */
#include <string.h>
#include "sgx_image.h"

#define PDE_VALID	1u
#define PTE_VALID	1u
#define PTE_READONLY	4u
#define PTE_CACHE_CONSISTENT 8u
#define PTE_EDM_PROTECT	0x10u

static const uint32_t heap_bases[SGX_HEAPS] = {
	[SGX_HEAP_GENERAL] = 0x00001000,      [SGX_HEAP_TA_DATA] = 0x0c800000,	  [SGX_HEAP_KERNEL_CODE] = 0x0e400000,
	[SGX_HEAP_KERNEL_DATA] = 0x0f000000,  [SGX_HEAP_PIXEL_USSE] = 0x0f400000, [SGX_HEAP_VERTEX_USSE] = 0x0fc00000,
	[SGX_HEAP_PDS_PIXEL] = 0x0dc00000,    [SGX_HEAP_PDS_VERTEX] = 0x0e800000, [SGX_HEAP_SYNC] = 0x0d800000,
	[SGX_HEAP_SHARED_3D] = 0x0b800000,    [SGX_HEAP_3D_PARAMETERS] = 0x0b800000,
};

uint32_t sgx_heap_base(unsigned heap) { return heap_bases[heap]; }

static uint32_t round_up(uint32_t v, uint32_t to) { return (v + to - 1) & ~(to - 1); }

uint32_t sgx_pages(struct sgx_mem *m, uint32_t pages)
{
	if (pages > (m->size - m->used) / SGX_PAGE)
		return 0;
	uint32_t phys = m->phys + m->used;
	memset(m->base + m->used, 0, pages * SGX_PAGE);
	m->used += pages * SGX_PAGE;
	return phys;
}

static void *pool_ptr(const struct sgx_mem *m, uint32_t phys)
{
	return phys >= m->phys && phys - m->phys < m->used ? m->base + (phys - m->phys) : NULL;
}

void sgx_mem_init(struct sgx_mem *m, void *base, uint32_t phys, uint32_t size)
{
	memset(m, 0, sizeof *m);
	m->base = base;
	m->phys = phys;
	m->size = size;
	sgx_pages(m, 1);
	for (unsigned i = 0; i < SGX_HEAPS; i++)
		m->next[i] = heap_bases[i];
}

static uint32_t *pte(struct sgx_mem *m, uint32_t addr, bool make)
{
	uint32_t *pd = (uint32_t *)(void *)m->base, *e = &pd[addr >> 22];
	if (!(*e & PDE_VALID)) {
		uint32_t pt = make ? sgx_pages(m, 1) : 0;
		if (!pt)
			return NULL;
		*e = pt | PDE_VALID;
	}
	uint32_t *pt = pool_ptr(m, *e & ~(SGX_PAGE - 1));
	return pt ? &pt[addr >> 12 & 0x3ff] : NULL;
}

static uint32_t pte_flags(uint32_t flags)
{
	uint32_t f = PTE_VALID;
	if ((flags & (SGX_MEM_READ | SGX_MEM_WRITE)) == SGX_MEM_READ)
		f |= PTE_READONLY;
	if (flags & SGX_MEM_CACHE_CONSISTENT)
		f |= PTE_CACHE_CONSISTENT;
	if (flags & SGX_MEM_EDM_PROTECT)
		f |= PTE_EDM_PROTECT;
	return f;
}

/* The vendor's allocator hands out whole pages in address order, and
 * the image's addresses only match its own if this does too. */
static uint32_t place(struct sgx_mem *m, unsigned heap, uint32_t size, uint32_t align)
{
	uint32_t addr = round_up(round_up(m->next[heap], SGX_PAGE), align > SGX_PAGE ? align : SGX_PAGE);
	m->next[heap] = addr + size;
	return addr;
}

uint32_t sgx_alloc(struct sgx_mem *m, unsigned heap, uint32_t size, uint32_t align, uint32_t flags)
{
	uint32_t pages = round_up(size, SGX_PAGE) / SGX_PAGE, phys;
	if (flags & SGX_MEM_DUMMY) {
		if (!m->dummy && !(m->dummy = sgx_pages(m, 1)))
			return 0;
		phys = m->dummy;
	} else if (!(phys = sgx_pages(m, pages)))
		return 0;
	uint32_t addr = place(m, heap, size, align);
	for (uint32_t i = 0; i < pages; i++) {
		uint32_t *e = pte(m, addr + i * SGX_PAGE, true);
		if (!e)
			return 0;
		*e = (flags & SGX_MEM_DUMMY ? phys : phys + i * SGX_PAGE) | pte_flags(flags);
	}
	return addr;
}

uint32_t sgx_map(struct sgx_mem *m, unsigned heap, uint32_t phys, uint32_t size, uint32_t flags)
{
	uint32_t addr = place(m, heap, size, SGX_PAGE);
	for (uint32_t off = 0; off < size; off += SGX_PAGE) {
		uint32_t *e = pte(m, addr + off, true);
		if (!e)
			return 0;
		*e = (phys + off) | pte_flags(flags);
	}
	return addr;
}

void *sgx_ptr(const struct sgx_mem *m, uint32_t addr)
{
	uint32_t *e = pte((struct sgx_mem *)m, addr, false);
	uint8_t *page = e && (*e & PTE_VALID) ? pool_ptr(m, *e & ~(SGX_PAGE - 1)) : NULL;
	return page ? page + (addr & (SGX_PAGE - 1)) : NULL;
}

uint32_t sgx_crc32(uint32_t crc, const void *data, uint32_t len)
{
	const uint8_t *p = data;
	crc = ~crc;
	while (len--) {
		crc ^= *p++;
		for (unsigned i = 0; i < 8; i++)
			crc = crc >> 1 ^ (crc & 1 ? 0xedb88320u : 0);
	}
	return ~crc;
}

/* Where the programs are in the file. Each copy runs 8 bytes into what
 * follows, as the vendor's does. */
static const struct { uint32_t off, size; } programs[4] = {
	{ 0x2898, 0x6890 }, { 0x9120, 0x6790 }, { 0xf8a8, 0x5990 }, { 0x15230, 0x7130 },
};
static const struct { uint32_t off, size; } pds_programs[5] = {
	{ 0x1c358, 0x38 }, { 0x1c390, 0x48 }, { 0x1c3d8, 0x38 }, { 0x1c410, 0x110 }, { 0x1c520, 0x78 },
};

enum { UKERNEL, TA, SPM, R3D };

void sgx_set_constant(uint32_t *program, uint32_t off, uint32_t v)
{
	uint32_t *ins = program + off / 4;
	ins[0] = (ins[0] & 0xffe00000) | (v & 0x1fffff);
	ins[1] = (ins[1] & ~0x3f1f0u) | (v >> 21 & 0x1f) << 4 | (v >> 26 & 0x3f) << 12;
}

/* The state processor's word that starts a program of the GPU's
 * processor: the 32 KiB page of the kernel code heap it is in, and the
 * instruction in that page. */
static uint32_t pds_run(uint32_t addr)
{
	uint32_t off = addr - heap_bases[SGX_HEAP_KERNEL_CODE];
	return 0x00200000 | (off & 0x7fff) >> 4 << 8 | off >> 15 << 4 | 0xf;
}

bool sgx_image_build(struct sgx_mem *m, struct sgx_image *img, sgx_read_fn read, void *ctx)
{
	const uint32_t rw = SGX_MEM_READ | SGX_MEM_WRITE | SGX_MEM_CACHE_CONSISTENT | SGX_MEM_NO_SYNC | SGX_MEM_EDM_PROTECT;
	const uint32_t ro = SGX_MEM_READ | SGX_MEM_NO_SYNC;
	const uint32_t kdata = heap_bases[SGX_HEAP_KERNEL_DATA], pds_base = heap_bases[SGX_HEAP_PDS_PIXEL];
	static const struct { uint32_t size, align; } data[10] = {
		{ 0x2000, 4096 }, { 8, 4096 }, { 4, 16 }, { 0x120, 32 }, { 0x4c, 32 },
		{ 0x54, 32 },	  { 0x2008, 16 }, { 0x640c, 32 }, { 0x314, 32 }, { 0x724, 32 },
	};
	uint32_t *first = &img->ccb;
	for (unsigned i = 0; i < 10; i++)
		if (!(first[i] = sgx_alloc(m, SGX_HEAP_KERNEL_DATA, data[i].size, data[i].align, rw)))
			return false;
	uint32_t *code[4], *pds[5];
	for (unsigned i = 0; i < 4; i++) {
		img->code[i] = sgx_alloc(m, SGX_HEAP_KERNEL_CODE, programs[i].size, 0x8000, ro);
		if (!img->code[i] || !read(ctx, programs[i].off, code[i] = sgx_ptr(m, img->code[i]), programs[i].size))
			return false;
	}
	for (unsigned i = 0; i < 5; i++) {
		img->pds[i] = sgx_alloc(m, SGX_HEAP_KERNEL_CODE, pds_programs[i].size, 16, ro);
		if (!img->pds[i] || !read(ctx, pds_programs[i].off, pds[i] = sgx_ptr(m, img->pds[i]), pds_programs[i].size))
			return false;
	}
	for (unsigned i = 0; i < 2; i++) {
		img->stencil[i] = sgx_alloc(m, SGX_HEAP_KERNEL_DATA, 0x40000, 4096,
					    SGX_MEM_READ | SGX_MEM_WRITE | SGX_MEM_NO_SYNC | SGX_MEM_DUMMY);
		if (!img->stencil[i])
			return false;
	}
	/* The commands' handlers are numbered from the start of the heap. */
	if (img->code[UKERNEL] != heap_bases[SGX_HEAP_KERNEL_CODE])
		return false;

	uint32_t loopback = (img->pds[4] - pds_base) >> 4;
	sgx_set_constant(code[UKERNEL], 0x2260, 0x41801000);
	sgx_set_constant(code[UKERNEL], 0x2270, (img->pds[1] - pds_base) >> 4);
	sgx_set_constant(code[UKERNEL], 0x2278, img->ta3d_ctl);
	sgx_set_constant(code[UKERNEL], 0x22b0, 0x41800900);
	sgx_set_constant(code[UKERNEL], 0x22c0, (img->pds[2] - pds_base) >> 4);
	sgx_set_constant(code[UKERNEL], 0x2310, (img->pds[3] - pds_base) & ~15u);
	sgx_set_constant(code[UKERNEL], 0x2320, 7);
	static const uint32_t loop_at[4] = { 0x5328, 0x5228, 0x4428, 0x6728 };
	for (unsigned i = 0; i < 4; i++) {
		sgx_set_constant(code[i], loop_at[i], 0x42000900);
		sgx_set_constant(code[i], loop_at[i] + 0x18, loopback);
	}

	uint32_t uk = img->code[UKERNEL];
	pds[0][0] = pds_run(uk + 0x2260);
	pds[0][1] = 0x04000000;
	pds[0][8] = 1;
	pds[1][0] = img->ta3d_ctl;
	pds[1][1] = 0x01e0003f;
	pds[1][2] = pds_run(uk + 0x22e0);
	pds[1][3] = 0x04000000;
	pds[2][0] = pds_run(uk + 0x22f0);
	pds[2][1] = 0x04000000;
	pds[2][8] = 1;
	pds[3][0] = 0x04000000;
	pds[3][1] = 1;
	pds[3][9] = 0x100;
	pds[3][10] = 0x200;
	pds[3][11] = pds_run(img->code[R3D]);
	pds[3][12] = pds_run(img->code[TA]);
	pds[3][13] = pds_run(img->code[SPM]);
	pds[3][14] = pds_run(uk + 0x80);
	pds[4][0] = pds_run(img->code[R3D] + 0x540);
	pds[4][1] = 0x04000000;
	pds[4][2] = pds_run(img->code[TA] + 0x2c0);
	pds[4][3] = 0x04000000;
	pds[4][4] = pds_run(img->code[SPM] + 0x130);
	pds[4][5] = 0x04000000;
	pds[4][9] = 0x200;
	pds[4][10] = 1;
	pds[4][11] = 0x0004000f;
	pds[4][12] = 0x00120080;

	uint32_t *ctl = sgx_ptr(m, img->ta3d_ctl);
	ctl[0x00 / 4] = img->ta3d_ctl;
	ctl[0x04 / 4] = img->host_ctl;
	ctl[0x08 / 4] = img->ccb_ctl;
	ctl[0x18 / 4] = 2;
	ctl[0x80 / 4] = img->trace;
	ctl[0x88 / 4] = 0x120f;
	ctl[0x98 / 4] = img->ccb;
	ctl[0x9c / 4] = m->phys;
	ctl[0xdc / 4] = kdata;
	ctl[0xe0 / 4] = (img->stencil[0] - kdata) | 0x80000000;
	ctl[0xe4 / 4] = (img->stencil[1] - kdata) | 0x80000000;
	ctl[0xe8 / 4] = img->hwperf;
	ctl[0xec / 4] = img->ta_sig;
	ctl[0xf0 / 4] = img->sig_3d;

	/* The registers the two signature buffers sample. */
	static const uint32_t ta_regs[] = { 3, 0, 0x304, 0x26c, 0x270 };
	static const uint32_t regs_3d[] = { 7, 0, 0x438, 0x4cc, 0x4d0, 0x4d4, 0x4d8, 0x308, 0x30c };
	memcpy(sgx_ptr(m, img->ta_sig), ta_regs, sizeof ta_regs);
	memcpy(sgx_ptr(m, img->sig_3d), regs_3d, sizeof regs_3d);
	return true;
}

/* Where in the microkernel each command is handled. */
static const uint16_t handlers[SGX_CMDS] = { 0x1ec, 0x1cd, 0, 0x18d, 0x1ab, 0x21d, 0x40e, 0x3b1 };

void sgx_command_words(uint32_t c[8], unsigned type, uint32_t cache_control, uint32_t arg)
{
	memset(c, 0, 8 * sizeof *c);
	c[0] = handlers[type];
	c[1] = cache_control;
	c[3] = arg;
}

unsigned sgx_script(const struct sgx_image *img, unsigned which, struct sgx_reg *out)
{
	unsigned n = 0;
#define W(o, v) (out[n++] = (struct sgx_reg){ (o), (v) })
	if (which == SGX_SCRIPT_BEFORE_RESET) {
		W(0xa58, 0);
		W(0xacc, 0);
	} else if (which == SGX_SCRIPT_AFTER_RESET) {
		W(0x130, 0x4000);
		W(0x110, 0);
		W(0x13c, 0);
		W(0xa7c, 0);
		W(0xa80, 0);
		W(0xa74, 0x003f8200);
		W(0x4e8, 0);
		W(0xa78, 0);
		W(0xabc, 0x40);
		W(0x804, 0xffff);
		W(0xa00, 0x7c000);
		for (uint32_t i = 0; i < 10; i++)
			W(0xa0c + 4 * i, (heap_bases[SGX_HEAP_PIXEL_USSE] + (i << 19)) >> 8 | 0x01000000);
		for (uint32_t i = 0; i < 4; i++)
			W(0xa34 + 4 * i, (heap_bases[SGX_HEAP_VERTEX_USSE] + (i << 19)) >> 8);
		W(0xa48, heap_bases[SGX_HEAP_KERNEL_CODE] >> 8 | 0x03000000);
		W(0xab8, heap_bases[SGX_HEAP_PDS_PIXEL]);
		W(0xb30, 0x100);
		W(0xac4, img->kicker);
		W(0xa68, (img->pds[0] - heap_bases[SGX_HEAP_PDS_PIXEL]) & ~15u);
		W(0xa6c, 3);
		W(0xa70, 0x00013d01);
		W(0xa58, 0x2204f00f);
	} else {
		static const uint16_t off[] = { 0xa58, 0xa68, 0xa6c, 0xa70, 0xa48, 0x130, 0x110, 0x000 };
		for (unsigned i = 0; i < 8; i++)
			W(off[i], 0);
	}
#undef W
	return n;
}

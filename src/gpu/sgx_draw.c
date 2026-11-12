/* The replayed scene of sgx_draw.h. Where a client of the vendor's
 * driver would call into its kernel driver, this does what that driver
 * does (sgxutils.c, sgxkick.c): the page directory in the render
 * context, the sync counters, the shared part of each TA command and
 * the command that tells the microkernel about it. */
#include <string.h>
#include "asteroid.h"
#include "sgx.h"
#include "sgx_image.h"
#include "sgx_draw.h"
#include "sgx_scene_data.h"

#define WAIT_US			1000000u
#define CCB_SIZE		0x10000u
#define CMD_SIZE		0x540u
#define CMD_BYTES		0x518u

#define CMD_FLAGS_A		0x004
#define CMD_FLAGS_B		0x01c
#define CMD_RTDATA		0x058
#define CMD_END			0x4e0
#define CMD_STREAM		0x4e4
#define CMD_REGIONS		0x4ec
#define CMD_SHARED		0x064
#define SH_FLAGS		0x000
#define SH_TA_COUNT		0x004
#define SH_3D_COUNT		0x008
#define SH_TA3D_PENDING		0x034
#define SH_TA3D_DONE		0x038
#define SH_SRC_COUNT		0x044
#define SH_TA_STATUS		0x348
#define SH_3D_STATUS		0x448
#define SH_READY		1u

enum { WRITE_PENDING, WRITE_DONE, READ_PENDING, READ_DONE, READ2_PENDING, READ2_DONE };

#define DETAILS_REGS		0x00
#define DETAILS_REGIONS		0x18
#define DETAILS_DATASET		0x90
#define DETAILS_RTDATA		0x94
/* Unknown, and cleared by the vendor's driver at every new scene. */
#define DETAILS_SCENE		0xc8

#define END_BYTES		(7 * 4)
#define TEX_PROGRAM_ADDR	10
#define TEX_PROGRAM_SAMPLING	2
#define TEX_PROGRAM_SIZE	3
#define ALPHA_WORD		9

static const uint32_t rtdata[2] = { SGX_SCENE_RTDATA0, SGX_SCENE_RTDATA1 };
static const uint32_t regions[2] = { SGX_SCENE_REGIONS0, SGX_SCENE_REGIONS1 };

static struct sgx_mem *mem;
static bool ready, stuck;
static uint32_t ccb_at, kicks, scenes, rt, target, cache, launches, verts_used;
static struct { uint32_t phys, size, gpu; } maps[4];
static unsigned nmaps;
static char failure[160];

/* Where the frame being written has got to in each ring, and the end
 * of the last item each holds. */
static uint32_t vd, pv, pv_end, mt, pp, pp_end, pc;

static volatile uint32_t *word(uint32_t addr) { return sgx_ptr(mem, addr); }

static void put(uint32_t addr, const uint32_t *w, unsigned n)
{
	volatile uint32_t *p = word(addr);
	for (unsigned i = 0; i < n; i++)
		p[i] = w[i];
}

static void put_runs(void)
{
	for (unsigned i = 0; i < SGX_SCENE_RUNS; i++) {
		const struct sgx_scene_run *r = &sgx_scene_runs[i];
		for (unsigned t = 0; t < r->times; t++)
			put(r->addr + t * r->step, &sgx_scene_words[r->data], r->words);
	}
}

static void put_tables(void)
{
	volatile uint32_t *links = word(SGX_SCENE_PB_LINKS);
	for (uint32_t i = 0; i < SGX_SCENE_PB_PAGES; i++) {
		uint32_t id = SGX_SCENE_PB_FIRST_PAGE + i;
		uint32_t prev = i ? id - 1 : 0xffff, next = i + 1 < SGX_SCENE_PB_PAGES ? id + 1 : 0xffff;
		links[id] = next | prev << 16;
	}
	volatile uint32_t *ix = word(SGX_SCENE_INDICES);
	for (uint32_t i = 0; i < SGX_SCENE_INDEX_TABLE; i += 2)
		ix[i / 2] = i | (i + 1) << 16;
}

bool sgx_draw_setup(void)
{
	if (ready)
		return true;
	if (stuck || !sgx_running() || !(mem = sgx_memory()))
		return false;
	/* What is allocated stays: a failed setup is not tried again. */
	stuck = true;
	for (unsigned i = 0; i < SGX_SCENE_ALLOCS; i++) {
		const struct sgx_scene_alloc *a = &sgx_scene_allocs[i];
		uint32_t addr = sgx_alloc(mem, a->heap, a->size, a->align, a->flags);
		if (addr != a->addr) {
			log_printf("sgx: scene allocation %u at %08lx, recorded at %08lx\n", i, addr, a->addr);
			return false;
		}
	}
	put_runs();
	put_tables();
	*word(SGX_SCENE_HW_CONTEXT + SGX_SCENE_PD_OFFSET) = mem->phys;
	cache = SGX_CACHE_INVAL_PT | SGX_CACHE_INVAL_PD;
	stuck = false;
	ready = true;
	sgx_draw_target(SGX_SCENE_PBUFFER);
	return true;
}

uint32_t sgx_draw_map(uint32_t phys, uint32_t size)
{
	if (!ready || !size || (phys | size) & (SGX_PAGE - 1))
		return 0;
	for (unsigned i = 0; i < nmaps; i++)
		if (phys >= maps[i].phys && phys - maps[i].phys + size <= maps[i].size)
			return maps[i].gpu + phys - maps[i].phys;
	if (nmaps == ARRAY_SIZE(maps))
		return 0;
	uint32_t gpu = sgx_map(mem, SGX_HEAP_GENERAL, phys, size, SGX_MEM_READ | SGX_MEM_WRITE | SGX_MEM_NO_SYNC);
	if (!gpu)
		return 0;
	maps[nmaps].phys = phys;
	maps[nmaps].size = size;
	maps[nmaps++].gpu = gpu;
	cache |= SGX_CACHE_INVAL_PT | SGX_CACHE_INVAL_PD;
	return gpu;
}

uint32_t sgx_draw_alloc(uint32_t size, void **cpu)
{
	if (!ready)
		return 0;
	uint32_t gpu = sgx_alloc(mem, SGX_HEAP_GENERAL, size, SGX_PAGE, SGX_MEM_READ | SGX_MEM_WRITE | SGX_MEM_NO_SYNC);
	if (gpu) {
		*cpu = sgx_ptr(mem, gpu);
		cache |= SGX_CACHE_INVAL_PT | SGX_CACHE_INVAL_PD;
	}
	return gpu;
}

void *sgx_draw_vertices(uint32_t bytes, uint32_t *gpu)
{
	bytes = (bytes + 15) & ~15u;
	if (!ready || bytes > SGX_SCENE_VERTICES_SIZE - verts_used)
		return NULL;
	*gpu = SGX_SCENE_VERTICES + verts_used;
	verts_used += bytes;
	return sgx_ptr(mem, *gpu);
}

void sgx_draw_target(uint32_t gpu_addr)
{
	if (!ready)
		return;
	target = gpu_addr;
	sgx_set_constant(sgx_ptr(mem, SGX_SCENE_TILE_STORE0), 0, gpu_addr);
	sgx_set_constant(sgx_ptr(mem, SGX_SCENE_TILE_STORE1), 0, gpu_addr);
	*word(SGX_SCENE_BG_TEXTURE) = gpu_addr & ~3u;
}

uint32_t sgx_draw_get_target(void) { return target; }

const char *sgx_draw_failure(void) { return failure; }

/* The end of each ring the kick's status entries report. */
static uint32_t ring_end(unsigned list, unsigned i, bool last)
{
	if (list == 0 && i == SGX_SCENE_TA_VDM)
		return vd + END_BYTES - SGX_SCENE_VDM;
	if (list == 0 && i == SGX_SCENE_TA_PV)
		return pv_end - SGX_SCENE_PV;
	if (list == 0 && i == SGX_SCENE_TA_MT)
		return mt - SGX_SCENE_MT;
	if (list == 1 && i == SGX_SCENE_3D_PP)
		return pp_end - SGX_SCENE_PP;
	return list ? sgx_scene_3d_status[last][i].value : sgx_scene_ta_status[last][i].value;
}

static bool kick(bool last)
{
	volatile uint32_t *c = word(SGX_SCENE_CCB + ccb_at);
	for (unsigned i = 0; i < CMD_BYTES / 4; i++)
		c[i] = 0;
	for (unsigned i = 0; i < SGX_SCENE_CMD_WORDS; i++)
		c[sgx_scene_cmd[i].off / 4] = sgx_scene_cmd[i].value;
	c[CMD_FLAGS_A / 4] = last ? SGX_SCENE_A_LAST : scenes == 1 ? SGX_SCENE_A_FIRST_EVER : SGX_SCENE_A_FIRST;
	if (last) {
		c[CMD_FLAGS_B / 4] = SGX_SCENE_B_LAST;
		c[CMD_END / 4] = SGX_SCENE_END_LAST;
	}
	c[CMD_STREAM / 4] = last ? vd : SGX_SCENE_STREAM_FIRST;
	c[CMD_RTDATA / 4] = rtdata[rt];
	c[CMD_REGIONS / 4] = regions[rt];
	ccb_at = (ccb_at + CMD_SIZE) % CCB_SIZE;
	*word(SGX_SCENE_CCB_CTL) = ccb_at;
	*word(SGX_SCENE_RT_PENDING) += 1;
	kicks++;

	volatile uint32_t *sh = c + CMD_SHARED / 4;
	if (last) {
		volatile uint32_t *s = word(SGX_SCENE_TA3D_SYNC);
		sh[SH_TA3D_DONE / 4] = SGX_SCENE_TA3D_SYNC + 4 * WRITE_DONE;
		sh[SH_TA3D_PENDING / 4] = s[WRITE_PENDING]++;
	}
	sh[SH_TA_COUNT / 4] = SGX_SCENE_TA_STATUS;
	sh[SH_3D_COUNT / 4] = SGX_SCENE_3D_STATUS;
	for (unsigned i = 0; i < SGX_SCENE_TA_STATUS; i++) {
		sh[SH_TA_STATUS / 4 + 2 * i] = sgx_scene_ta_status[last][i].addr;
		sh[SH_TA_STATUS / 4 + 2 * i + 1] = i == SGX_SCENE_TA_SEQ ? kicks : ring_end(0, i, last);
	}
	for (unsigned i = 0; i < SGX_SCENE_3D_STATUS; i++) {
		sh[SH_3D_STATUS / 4 + 2 * i] = sgx_scene_3d_status[last][i].addr;
		sh[SH_3D_STATUS / 4 + 2 * i + 1] = i == SGX_SCENE_3D_SEQ ? scenes : ring_end(1, i, last);
	}
	sh[SH_SRC_COUNT / 4] = 0;
	if (!last) {
		volatile uint32_t *l = word(SGX_SCENE_SYNC_LIST), *s = word(SGX_SCENE_SURFACE_SYNC);
		l[1] = 1;
		l[2] = s[READ_PENDING];
		l[3] = SGX_SCENE_SURFACE_SYNC + 4 * READ_DONE;
		l[4] = s[WRITE_PENDING]++;
		l[5] = SGX_SCENE_SURFACE_SYNC + 4 * WRITE_DONE;
		l[6] = s[READ2_PENDING];
		l[7] = SGX_SCENE_SURFACE_SYNC + 4 * READ2_DONE;
	}
	sh[SH_FLAGS / 4] |= SH_READY;
	uint32_t cc = cache;
	cache = 0;
	return sgx_submit_ta(SGX_SCENE_HW_CONTEXT, cc);
}

static bool synced(uint32_t addr)
{
	volatile uint32_t *s = word(addr);
	return s[WRITE_DONE] == s[WRITE_PENDING];
}

static bool done(bool last)
{
	volatile uint32_t *ctl = word(SGX_SCENE_CCB_CTL);
	if (ctl[1] != ctl[0] || (int32_t)(*word(sgx_scene_ta_status[0][SGX_SCENE_TA_SEQ].addr) - kicks) < 0)
		return false;
	return !last || ((int32_t)(*word(sgx_scene_3d_status[0][SGX_SCENE_3D_SEQ].addr) - scenes) >= 0 &&
			 !*word(SGX_SCENE_DETAILS_ACCESS) && !*word(SGX_SCENE_SYNC_LIST_ACCESS) &&
			 synced(SGX_SCENE_TA3D_SYNC) && synced(SGX_SCENE_SURFACE_SYNC));
}

static bool wait(bool last, uint32_t *us)
{
	uint32_t t0 = time_us();
	while (!done(last)) {
		if (time_us() - t0 > WAIT_US) {
			volatile uint32_t *ctl = word(SGX_SCENE_CCB_CTL), *a = word(SGX_SCENE_TA3D_SYNC),
					  *b = word(SGX_SCENE_SURFACE_SYNC);
			snprintf(failure, sizeof failure,
				 "%s kick %lu not done: queue %lu read %lu, kicks done %lu, scenes done %lu of %lu, "
				 "held %lu %lu, syncs %lu/%lu %lu/%lu",
				 last ? "last" : "first", kicks, ctl[0], ctl[1],
				 *word(sgx_scene_ta_status[0][SGX_SCENE_TA_SEQ].addr),
				 *word(sgx_scene_3d_status[0][SGX_SCENE_3D_SEQ].addr), scenes, *word(SGX_SCENE_DETAILS_ACCESS),
				 *word(SGX_SCENE_SYNC_LIST_ACCESS), a[WRITE_DONE], a[WRITE_PENDING], b[WRITE_DONE],
				 b[WRITE_PENDING]);
			stuck = true;
			return false;
		}
		wdt_feed();
	}
	*us = time_us() - t0;
	return true;
}

static uint32_t offset(uint32_t addr, uint32_t base) { return (addr - base) >> 4; }

static void vdm(uint32_t a, uint32_t b)
{
	*word(vd) = a;
	*word(vd + 4) = b;
	vd += 8;
}

/* A state block and the MTE-copy entry that loads it, launched with the
 * toggle the driver flips at every launch. */
static void state(const uint32_t *block, unsigned n)
{
	unsigned k = n == 4 ? 0 : n == 9 ? 1 : 2;
	put(pv, block, n);
	uint32_t entry[32] = { pv, sgx_scene_mte_copy[k][1], 0, 0, sgx_scene_mte_copy[k][2] };
	memcpy(&entry[12], sgx_scene_mte_code, sizeof sgx_scene_mte_code);
	put(mt, entry, 32);
	vdm(SGX_SCENE_LAUNCH | offset(mt, SGX_SCENE_VERTEX_BASE), SGX_SCENE_MTE_WORD | (launches & 1) | (n + 3) >> 2);
	launches++;
	mt += 0x80;
	pv_end = pv + 4 * n;
	pv += (4 * n + 31) & ~31u;
}

static void vertex_program(const uint32_t *data, uint32_t v)
{
	uint32_t p[24] = { 0 };
	memcpy(p, data, 12 * 4);
	memcpy(&p[12], sgx_scene_vprog_code, sizeof sgx_scene_vprog_code);
	p[0] = v;
	p[2] = v + 8;
	put(pv, p, 24);
	pv_end = pv + 18 * 4;
	pv += 0x60;
}

/* A constants block and its upload, 0x80 bytes: the descriptor (the
 * program's address) is the second half. */
static uint32_t secondary(uint32_t at, const uint32_t *consts, const uint32_t *desc)
{
	uint32_t p[32] = { 0 };
	memcpy(p, consts, 16 * 4);
	p[16] = at;
	p[17] = desc[0];
	p[18] = desc[1];
	memcpy(&p[28], sgx_scene_sec_code, sizeof sgx_scene_sec_code);
	put(at, p, 32);
	return at + 0x40;
}

static void vertex_secondary(void)
{
	uint32_t c[16] = { pv - 4 };
	memcpy(&c[1], &sgx_scene_vsec_const[1], 9 * 4);
	uint32_t desc = secondary(pv, c, sgx_scene_vsec_desc);
	pv_end = desc + 0x3c;
	pv += 0x80;
	vdm(SGX_SCENE_LAUNCH | offset(desc, SGX_SCENE_VERTEX_BASE), SGX_SCENE_VSEC_WORD);
}

static void index_list(unsigned count, uint32_t program)
{
	uint32_t w[6];
	memcpy(w, sgx_scene_index_list, sizeof w);
	w[0] |= count;
	w[1] = SGX_SCENE_INDICES;
	w[4] |= offset(program, SGX_SCENE_VERTEX_BASE);
	put(vd, w, 6);
	vd += 24;
}

static uint32_t program_word(uint32_t addr) { return SGX_SCENE_PROGRAM_WORD | offset(addr, SGX_SCENE_PIXEL_BASE); }

static bool power_of_two(uint32_t v) { return !(v & (v - 1)); }

static bool valid(const struct sgx_frame *f)
{
	if (!f->n || f->n > SGX_DRAW_MAX_DRAWS || f->draws[0].kind != SGX_DRAW_FLAT)
		return false;
	unsigned flats = 0;
	for (unsigned i = 0; i < f->n; i++) {
		const struct sgx_draw *d = &f->draws[i];
		if (!d->count || d->count % 3 || d->count > SGX_DRAW_MAX_VERTS)
			return false;
		if (d->kind == SGX_DRAW_FLAT) {
			flats++;
			if (i && (d->blend || f->draws[i - 1].kind != SGX_DRAW_TEXTURED))
				return false;
		} else if (d->kind != SGX_DRAW_TEXTURED || !d->blend || d->tex & (SGX_PAGE - 1) || d->width % 32 ||
			   d->width > 2048 || !d->height || d->height > 2048 || power_of_two(d->width) ||
			   power_of_two(d->height)) {
			return false;
		}
	}
	return !f->draws[0].blend || flats == 1;
}

/* A texture's pixel program, written at its first use in the frame and
 * shared by the draws after. */
static uint32_t texture_program(const struct sgx_draw *d, const struct sgx_draw *draws, unsigned i, uint32_t *slots)
{
	for (unsigned j = 0; j < i; j++) {
		const struct sgx_draw *e = &draws[j];
		if (e->kind == SGX_DRAW_TEXTURED && e->tex == d->tex && e->width == d->width && e->height == d->height &&
		    e->linear == d->linear)
			return slots[j];
	}
	uint32_t p[16];
	memcpy(p, sgx_scene_tex_program, sizeof p);
	p[TEX_PROGRAM_SAMPLING] = sgx_scene_sampling[d->linear];
	p[TEX_PROGRAM_SIZE] = SGX_SCENE_TEX_FORMAT | (uint32_t)(d->width - 1) << 12 | (uint32_t)(d->height - 1);
	p[TEX_PROGRAM_ADDR] = d->tex;
	put(pc, p, 16);
	pc += 0x40;
	return pc - 0x40;
}

static void put_draws(const struct sgx_frame *f)
{
	static uint32_t slots[SGX_DRAW_MAX_DRAWS];
	vd = SGX_SCENE_VDM + SGX_SCENE_VDM_PREFIX;
	pv = SGX_SCENE_PV + SGX_SCENE_PV_PREFIX;
	mt = SGX_SCENE_MT + SGX_SCENE_MT_PREFIX;
	pp = SGX_SCENE_PP + SGX_SCENE_PP_PREFIX;
	pc = SGX_SCENE_PC + SGX_SCENE_PC_PREFIX;
	pp_end = pp;
	/* The clear's two launches: of their toggles only the first one's
	 * shows, the second's word count being odd. */
	*word(SGX_SCENE_MTE_LAUNCH) = SGX_SCENE_MTE_WORD | (launches & 1) | 2;
	launches += SGX_SCENE_BG_LAUNCHES;
	for (unsigned i = 0; i < f->n; i++) {
		const struct sgx_draw *d = &f->draws[i], *prev = i ? &f->draws[i - 1] : NULL;
		uint32_t program;
		if (d->kind == SGX_DRAW_FLAT) {
			if (!i) {
				*word(SGX_SCENE_FLAT_PROGRAM) = d->verts;
				*word(SGX_SCENE_FLAT_PROGRAM + 8) = d->verts + 8;
				uint32_t b[18];
				memcpy(b, sgx_scene_first_flat, sizeof b);
				b[1] = sgx_scene_isp[d->blend];
				state(b, 18);
				program = SGX_SCENE_FLAT_PROGRAM;
			} else {
				program = pv;
				vertex_program(sgx_scene_vprog_flat, d->verts);
				state(sgx_scene_flat_block, 9);
			}
			vertex_secondary();
		} else {
			uint32_t q = slots[i] = texture_program(d, f->draws, i, slots);
			uint32_t c[16] = { 0 };
			c[ALPHA_WORD] = d->alpha;
			uint32_t s = secondary(pp, c, sgx_scene_psec_desc);
			pp_end = s + 0x3c;
			pp += 0x80;
			program = pv;
			vertex_program(sgx_scene_vprog_tex, d->verts);
			if (prev->kind == SGX_DRAW_FLAT) {
				uint32_t b[9];
				memcpy(b, sgx_scene_tex_block9, sizeof b);
				b[3] = program_word(s);
				b[5] = program_word(q);
				state(b, 9);
			} else {
				uint32_t b[4];
				memcpy(b, sgx_scene_tex_block4, sizeof b);
				b[1] = program_word(s);
				b[3] = program_word(q);
				state(b, 4);
			}
			if (prev->kind == SGX_DRAW_FLAT || prev->tex != d->tex || prev->width != d->width ||
			    prev->height != d->height || prev->linear != d->linear)
				vertex_secondary();
		}
		index_list(d->count, program);
	}
	put(vd, sgx_scene_end, 7);
}

static void put_frame(const struct sgx_frame *f)
{
	for (unsigned i = 0; i < SGX_SCENE_CLEARS; i++)
		*word(SGX_SCENE_CLEAR + i * SGX_SCENE_CLEAR_STEP) = f->clear;
	const uint32_t *b = sgx_scene_blend[f->draws[0].blend];
	for (unsigned i = 0; i < 3; i++)
		word(SGX_SCENE_BLEND_CODE)[i] = b[i];
	put_draws(f);

	volatile uint32_t *d = word(SGX_SCENE_DETAILS);
	for (unsigned i = 0; i < 34; i++)
		d[DETAILS_REGS / 4 + i] = sgx_scene_regs3d[i];
	d[DETAILS_REGIONS / 4] = regions[rt];
	d[DETAILS_DATASET / 4] = SGX_SCENE_DATASET;
	d[DETAILS_RTDATA / 4] = rtdata[rt];
	d[DETAILS_SCENE / 4] = 0;
	*word(SGX_SCENE_DETAILS_ACCESS) = 1;
	*word(SGX_SCENE_SYNC_LIST_ACCESS) = 1;
}

static bool frame(const struct sgx_frame *f, struct sgx_times *t)
{
	if (!ready || stuck || !sgx_running()) {
		snprintf(failure, sizeof failure, "no scene, or the GPU is off");
		return false;
	}
	if (!valid(f)) {
		snprintf(failure, sizeof failure, "draws of a kind not recorded");
		return false;
	}
	if (*word(SGX_SCENE_DETAILS_ACCESS) || *word(SGX_SCENE_SYNC_LIST_ACCESS)) {
		snprintf(failure, sizeof failure, "the last scene still holds its render details");
		return false;
	}
	scenes++;
	rt ^= 1;
	put_frame(f);
	bool ok = kick(false) && wait(false, &t->ta_us);
	if (ok) {
		vd += END_BYTES;
		put(vd, sgx_scene_end, 7);
		ok = kick(true) && wait(true, &t->render_us);
	}
	if (!ok)
		stuck = true;
	return ok;
}

bool sgx_draw_frame(const struct sgx_frame *f, struct sgx_times *t)
{
	bool ok = frame(f, t);
	verts_used = 0;
	return ok;
}

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

static const uint32_t rtdata[2] = { SGX_SCENE_RTDATA0, SGX_SCENE_RTDATA1 };
static const uint32_t regions[2] = { SGX_SCENE_REGIONS0, SGX_SCENE_REGIONS1 };

static struct sgx_mem *mem;
static bool ready, stuck;
static uint32_t ccb_at, kicks, scenes, rt, target, cache;
static struct { uint32_t phys, size, gpu; } maps[4];
static unsigned nmaps;

static volatile uint32_t *word(uint32_t addr) { return sgx_ptr(mem, addr); }

static void put_runs(void)
{
	for (unsigned i = 0; i < SGX_SCENE_RUNS; i++) {
		const struct sgx_scene_run *r = &sgx_scene_runs[i];
		for (unsigned t = 0; t < r->times; t++) {
			volatile uint32_t *p = word(r->addr + t * r->step);
			for (unsigned j = 0; j < r->words; j++)
				p[j] = sgx_scene_words[r->data + j];
		}
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
	c[CMD_STREAM / 4] = last ? SGX_SCENE_STREAM_LAST : SGX_SCENE_STREAM_FIRST;
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
	const struct sgx_scene_status *ta = sgx_scene_ta_status[last], *r3d = sgx_scene_3d_status[last];
	sh[SH_TA_COUNT / 4] = SGX_SCENE_TA_STATUS;
	sh[SH_3D_COUNT / 4] = SGX_SCENE_3D_STATUS;
	for (unsigned i = 0; i < SGX_SCENE_TA_STATUS; i++) {
		sh[SH_TA_STATUS / 4 + 2 * i] = ta[i].addr;
		sh[SH_TA_STATUS / 4 + 2 * i + 1] = i == SGX_SCENE_TA_SEQ ? kicks : ta[i].value;
	}
	for (unsigned i = 0; i < SGX_SCENE_3D_STATUS; i++) {
		sh[SH_3D_STATUS / 4 + 2 * i] = r3d[i].addr;
		sh[SH_3D_STATUS / 4 + 2 * i + 1] = i == SGX_SCENE_3D_SEQ ? scenes : r3d[i].value;
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
			log_printf("sgx: %s kick %lu not done: queue %lu read %lu, kicks done %lu, scenes done %lu of %lu, "
				   "held %lu %lu, syncs %lu/%lu %lu/%lu\n",
				   last ? "last" : "first", kicks, ctl[0], ctl[1],
				   *word(sgx_scene_ta_status[0][SGX_SCENE_TA_SEQ].addr),
				   *word(sgx_scene_3d_status[0][SGX_SCENE_3D_SEQ].addr), scenes, *word(SGX_SCENE_DETAILS_ACCESS),
				   *word(SGX_SCENE_SYNC_LIST_ACCESS), a[WRITE_DONE], a[WRITE_PENDING], b[WRITE_DONE],
				   b[WRITE_PENDING]);
			sgx_log_state();
			stuck = true;
			return false;
		}
		wdt_feed();
	}
	*us = time_us() - t0;
	return true;
}

static void put_frame(const struct sgx_frame *f)
{
	volatile uint32_t *v = word(SGX_SCENE_VERTICES);
	for (unsigned i = 0; i < f->tris * 18; i++) {
		uint32_t w;
		memcpy(&w, &f->verts[i], 4);
		v[i] = w;
	}
	*word(SGX_SCENE_INDEX_COUNT) = SGX_SCENE_INDEX_LIST | 3 * f->tris;
	for (unsigned i = 0; i < SGX_SCENE_CLEARS; i++)
		*word(SGX_SCENE_CLEAR + i * SGX_SCENE_CLEAR_STEP) = f->clear;
	/* The driver's state copies alternate between two slots, three a
	 * frame; of the three launches only the first shows it. */
	*word(SGX_SCENE_MTE_LAUNCH) = SGX_SCENE_MTE_WORD | (scenes % 2 == 0);
	const uint32_t *b = sgx_scene_blend[f->blend];
	*word(SGX_SCENE_BLEND_STATE) = b[0];
	for (unsigned i = 0; i < 3; i++)
		word(SGX_SCENE_BLEND_CODE)[i] = b[1 + i];

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

bool sgx_draw_frame(const struct sgx_frame *f, struct sgx_times *t)
{
	if (!ready || stuck || !f->tris || f->tris > SGX_DRAW_MAX_TRIS)
		return false;
	if (*word(SGX_SCENE_DETAILS_ACCESS) || *word(SGX_SCENE_SYNC_LIST_ACCESS)) {
		log_printf("sgx: the last scene still holds its render details\n");
		return false;
	}
	scenes++;
	rt ^= 1;
	put_frame(f);
	bool ok = kick(false) && wait(false, &t->ta_us);
	if (ok) {
		for (unsigned i = 0; i < 7; i++)
			word(SGX_SCENE_TERMINATE)[i] = sgx_scene_terminate[i];
		ok = kick(true) && wait(true, &t->render_us);
	}
	if (!ok)
		stuck = true;
	return ok;
}

/* Drawing on the Adreno 305B straight into a linear buffer in DRAM: the
 * render backend runs with GMEM bypassed, so there is no tiling or
 * binning pass, just the draw calls. Shaders are copied into GPU memory
 * and loaded into the shader processors' instruction buffers with
 * CP_LOAD_STATE (or fetched through the instruction cache when they are
 * too long for the buffers), uniforms are loaded inline. Textures are
 * described to the texture pipe by a sampler (filter, wrap), a texture
 * constant (format, size, pitch) and a table of level addresses, each
 * loaded with CP_LOAD_STATE into the fragment stage's slots. Blending is
 * off or premultiplied source-over; no depth, stencil or culling, and the
 * viewport covers the whole target. A state change after a draw in the
 * same submission first waits for the GPU to go idle. */
#include <string.h>
#include "asteroid.h"
#include "a3xx_ring.h"
#include "draw.h"
#include "shaders.h"

#define REG_UNKNOWN_0C3D		0x0c3d
#define REG_GRAS_TSE_DEBUG_ECO		0x0c81
#define REG_GRAS_CL_USER_PLANE(n)	(0x0ca0 + 4 * (n))
#define REG_RB_FRAME_BUFFER_DIMENSION	0x0ce0
#define REG_HLSQ_PERFCOUNTER0_SELECT	0x0e00
#define REG_UNKNOWN_0E43		0x0e43
#define REG_VFD_PERFCOUNTER0_SELECT	0x0e44
#define REG_UCHE_CACHE_INVALIDATE0	0x0ea0
#define REG_UNKNOWN_0EE0		0x0ee0
#define REG_UNKNOWN_0F03		0x0f03
#define REG_GRAS_CL_CLIP_CNTL		0x2040
#define REG_GRAS_CL_GB_CLIP_ADJ		0x2044
#define REG_GRAS_CL_VPORT_XOFFSET	0x2048
#define REG_GRAS_SU_POINT_MINMAX	0x2068
#define REG_GRAS_SU_POLY_OFFSET_SCALE	0x206c
#define REG_GRAS_SU_MODE_CONTROL	0x2070
#define REG_GRAS_SC_CONTROL		0x2072
#define REG_GRAS_SC_SCREEN_SCISSOR_TL	0x2074
#define REG_GRAS_SC_WINDOW_SCISSOR_TL	0x2079
#define REG_RB_MODE_CONTROL		0x20c0
#define REG_RB_RENDER_CONTROL		0x20c1
#define REG_RB_MSAA_CONTROL		0x20c2
#define REG_RB_ALPHA_REF		0x20c3
#define REG_RB_MRT_CONTROL(n)		(0x20c4 + 4 * (n))
#define REG_RB_MRT_BUF_INFO(n)		(0x20c5 + 4 * (n))
#define REG_RB_MRT_BLEND_CONTROL(n)	(0x20c7 + 4 * (n))
#define REG_RB_BLEND_RED		0x20e4
#define REG_RB_DEPTH_CONTROL		0x2100
#define REG_RB_DEPTH_INFO		0x2102
#define REG_RB_STENCIL_CONTROL		0x2104
#define REG_RB_STENCILREFMASK		0x2108
#define REG_RB_WINDOW_OFFSET		0x210e
#define REG_RB_Z_CLAMP_MIN		0x2114
#define REG_PC_VSTREAM_CONTROL		0x21e4
#define REG_PC_VERTEX_REUSE_BLOCK_CNTL	0x21ea
#define REG_PC_PRIM_VTX_CNTL		0x21ec
#define REG_PC_RESTART_INDEX		0x21ed
#define REG_HLSQ_CONTROL_0_REG		0x2200
#define REG_HLSQ_CONST_VSPRESV_RANGE_REG 0x2206
#define REG_VFD_CONTROL_0		0x2240
#define REG_VFD_INDEX_MIN		0x2242
#define REG_VFD_FETCH_INSTR_0(n)	(0x2246 + 2 * (n))
#define REG_VFD_DECODE_INSTR(n)		(0x2266 + (n))
#define REG_VFD_VS_THREADING_THRESHOLD	0x227e
#define REG_VPC_ATTR			0x2280
#define REG_VPC_VARYING_INTERP_MODE(n)	(0x2282 + (n))
#define REG_VPC_VARYING_PS_REPL_MODE(n)	(0x2286 + (n))
#define REG_VPC_VARY_CYLWRAP_ENABLE_0	0x228a
#define REG_SP_SP_CTRL_REG		0x22c0
#define REG_SP_VS_CTRL_REG0		0x22c4
#define REG_SP_VS_OUT_REG(n)		(0x22c7 + (n))
#define REG_SP_VS_VPC_DST_REG(n)	(0x22d0 + (n))
#define REG_SP_VS_OBJ_OFFSET_REG	0x22d4
#define REG_SP_VS_PVT_MEM_PARAM_REG	0x22d6
#define REG_SP_VS_LENGTH_REG		0x22df
#define REG_SP_FS_CTRL_REG0		0x22e0
#define REG_SP_FS_OBJ_OFFSET_REG	0x22e2
#define REG_SP_FS_PVT_MEM_PARAM_REG	0x22e4
#define REG_SP_FS_FLAT_SHAD_MODE_REG_0	0x22e8
#define REG_SP_FS_OUTPUT_REG		0x22ec
#define REG_SP_FS_MRT_REG(n)		(0x22f0 + (n))
#define REG_SP_FS_IMAGE_OUTPUT_REG(n)	(0x22f4 + (n))
#define REG_SP_FS_LENGTH_REG		0x22ff
#define REG_TPL1_TP_VS_TEX_OFFSET	0x2340
#define REG_TPL1_TP_FS_TEX_OFFSET	0x2342

#define EV_CACHE_FLUSH_TS		0x04
#define EV_CACHE_FLUSH			0x06
#define EV_HLSQ_FLUSH			0x07

#define SB_FRAG_TEX			2
#define SB_FRAG_MIPADDR			3
#define SB_VERT_SHADER			4
#define SB_FRAG_SHADER			6
#define ST_SHADER			0
#define ST_CONSTANTS			1
#define SS_DIRECT			0
#define SS_INDIRECT			4
#define LOAD_STATE_0(off, src, blk, n)	((off) | (src) << 16 | (blk) << 19 | (n) << 22)

#define DI_PT_TRILIST			4
#define DI_SRC_SEL_AUTO_INDEX		2
#define DRAW_PRE_DRAW_INITIATOR_ENABLE	BIT(14)

#define VFMT_32_FLOAT			0x00
#define VFMT_8_UNORM			0x2c
#define SWAP_WZYX			0
#define SWAP_WXYZ			1
#define SWAP_XYZW			3
#define RB_R8G8B8A8_UNORM		0x08
#define ROP_COPY			12
#define FACTOR_ZERO			0
#define FACTOR_ONE			1
#define FACTOR_ONE_MINUS_SRC_ALPHA	7
#define PC_DRAW_TRIANGLES		2
#define INTERP_FLAT			1
#define TWO_QUADS			0
#define FOUR_QUADS			1
#define INSTR_CACHE			0
#define INSTR_BUFFER			1

#define RB_MODE_CONTROL_GMEM_BYPASS	BIT(7)
#define RB_MODE_CONTROL_MARB_CACHE_SPLIT BIT(15)
#define RB_RENDER_CONTROL_BIN_WIDTH(x)	((x) >> 5 << 4)
#define RB_MSAA_CONTROL_DISABLE		BIT(10)
#define RB_MSAA_CONTROL_SAMPLE_MASK(x)	((x) << 16)
#define RB_MRT_CONTROL_READ_DEST_ENABLE	BIT(3)
#define RB_MRT_CONTROL_BLEND		BIT(4)
#define RB_MRT_CONTROL_BLEND2		BIT(5)
#define RB_MRT_CONTROL_ROP_CODE(x)	((x) << 8)
#define RB_MRT_CONTROL_COMPONENT_ENABLE(x) ((x) << 24)
#define RB_MRT_BUF_INFO_SWAP(x)		((x) << 10)
#define RB_MRT_BUF_INFO_PITCH(x)	((x) >> 5 << 17)
#define RB_MRT_BUF_BASE(x)		((x) >> 5 << 4)
#define RB_MRT_BLEND(src, dst)		((src) | (dst) << 8 | (src) << 16 | (dst) << 24)
#define RB_MRT_BLEND_CLAMP_ENABLE	BIT(29)
#define RB_BLEND_COLOR(u, f)		((u) | (f) << 16)
#define XY(x, y)			((x) | (y) << 16)
#define GRAS_CL_CLIP_CNTL_IJ_PERSP_CENTER BIT(12)
#define GRAS_SU_MODE_LINEHALFWIDTH(q)	((q) << 3)
#define PC_PRIM_VTX_CNTL_POLYMODE(f, b)	((f) << 5 | (b) << 8)
#define PC_PRIM_VTX_CNTL_PROVOKING_LAST	BIT(25)
#define HLSQ_CONTROL_0_FSTHREADSIZE(x)	((x) << 4)
#define HLSQ_CONTROL_0_FSSUPERTHREAD	BIT(6)
#define HLSQ_CONTROL_0_SPSHADERRESTART	BIT(9)
#define HLSQ_CONTROL_0_CONSTMODE(x)	((x) << 27)
#define HLSQ_CONTROL_0_SPCONSTFULLUPDATE BIT(29)
#define HLSQ_CONTROL_1_VSTHREADSIZE(x)	((x) << 6)
#define HLSQ_CONTROL_1_VSSUPERTHREAD	BIT(8)
#define HLSQ_STAGE_CONTROL(len, start, instr) ((len) | (start) << 12 | ((instr) & 0xff) << 24)
#define SP_CTRL_CONSTMODE(x)		((x) << 18)
#define SP_CTRL_SLEEPMODE(x)		((x) << 20)
#define SP_CTRL_REG0_BUFFERMODE(x)	((x) << 1)
#define SP_CTRL_REG0_CACHEINVALID	BIT(2)
#define SP_CTRL_REG0_FOOTPRINT(h, f)	((h) << 4 | (f) << 10)
#define SP_FS_CTRL_REG0_INOUTREGOVERLAP	BIT(18)
#define SP_CTRL_REG0_THREADSIZE(x)	((x) << 20)
#define SP_CTRL_REG0_SUPERTHREADMODE	BIT(21)
#define SP_FS_CTRL_REG0_PIXLODENABLE	BIT(22)
#define SP_CTRL_REG0_LENGTH(x)		(((x) & 0xff) << 24)
#define SP_CTRL_REG1_CONST(len)		((len) | ((len) ? (len) - 1 : 0) << 10)
#define SP_VS_CTRL_REG1_INITIALOUTSTANDING(x) ((x) << 24)
#define SP_FS_CTRL_REG1_INITIALOUTSTANDING(x) ((x) << 20)
#define SP_FS_CTRL_REG1_HALFPRECVAROFFSET(x) ((x) << 24)
#define SP_VS_PARAM_REG(pos, psize, nvar) ((pos) | (psize) << 8 | (nvar) << 20)
#define SP_OBJ_OFFSET(c, s)		((c) << 16 | (s) << 25)
#define SP_FS_OUTPUT_REG_DEPTH_REGID(x)	((x) << 8)
#define SP_FS_MRT_REG_HALF_PRECISION	BIT(8)
#define TPL1_TEX_OFFSET(s, m, b)	((s) | (m) << 8 | (b) << 16)
#define FRAG_TEX_OFF			16
#define FRAG_TEX_UNITS			16
#define MIP_LEVELS			14
#define TEX_NEAREST			0
#define TEX_CLAMP_TO_EDGE		1
#define TEX_SAMP_0(filt, wrap)		((filt) << 2 | (filt) << 4 | (wrap) << 6 | (wrap) << 9 | (wrap) << 12)
#define TEX_SAMP_1_MAX_LOD(x)		((x) << 12)
#define TEX_SWIZ_XYZW			(0 << 4 | 1 << 7 | 2 << 10 | 3 << 13)
#define TFMT_8_8_8_8_UNORM		0x33
#define TEX_2D				1
#define TEX_CONST_0(swiz, fmt, type)	((swiz) | (fmt) << 22 | (type) << 30)
#define TEX_CONST_1(w, h, align)	((h) | (w) << 14 | (align) << 28)
#define TEX_PITCHALIGN_128B		3
#define TEX_CONST_2(indx, pitch)	((indx) | (pitch) << 12)
#define VFD_FETCH_INSTR_0(size, stride, next, idx) \
	(((size) - 1) | (stride) << 7 | (next) << 17 | (idx) << 18 | 1u << 24)
#define VFD_DECODE_INSTR(mask, fmt, swap, regid, size, next) \
	((mask) | BIT(4) | (fmt) << 6 | (regid) << 12 | (swap) << 22 | (size) << 24 | BIT(29) | (next) << 30)
#define VFD_CONTROL_0(attrs, n)		((attrs) | 2 << 18 | (n) << 22 | (n) << 27)
#define VFD_CONTROL_1(vtx, inst)	(1 | (vtx) << 16 | (inst) << 24)
#define VPC_ATTR(n)			((n) | 1 << 12 | 1u << 28)
#define VPC_PACK(n)			((n) << 8 | (n) << 16)
#define UCHE_INVALIDATE_ENTIRE		(1u << 28 | BIT(31))

#define QUADS				16
#define QUAD_FLOATS			(6 * 6)

static struct gpu_shader *cur_vs;
static uint32_t fs_total_in, pvt_mem, fence, seqno, target_w, target_h;
static float *quads;
static unsigned quad_i;
static bool drawn;
static volatile uint32_t *fence_ptr;

static void out(uint32_t v) { gpu_ring_emit(v); }
static void pkt3(uint32_t op, unsigned n) { out(CP_TYPE3(op, n)); }
static void cp_wfi(void) { pkt3(CP_WAIT_FOR_IDLE, 1); out(0); }
static void event(uint32_t ev) { pkt3(CP_EVENT_WRITE, 1); out(ev); }
static uint32_t fui(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static void sync_state(void)
{
	if (!drawn)
		return;
	gpu_ring_reserve(2);
	cp_wfi();
	drawn = false;
}

static void regs(uint32_t r, unsigned n, const uint32_t *v)
{
	out(CP_TYPE0(r, n));
	for (unsigned i = 0; i < n; i++)
		out(v[i]);
}
#define reg(r, n, ...)	regs(r, n, (const uint32_t[n]){ __VA_ARGS__ })

void *gpu_alloc(uint32_t size, uint32_t align)
{
	static uint32_t top = GPU_HEAP_BASE;
	uint32_t a = (top + align - 1) & ~(align - 1);
	if (a + size > GPU_MEM_BASE + GPU_MEM_SIZE || a + size < a) {
		log_printf("gpu: out of memory for %lu bytes\n", size);
		return NULL;
	}
	top = a + size;
	return (void *)(uintptr_t)a;
}

static unsigned footprint(int8_t max) { return max + 1; }

static bool upload(struct gpu_shader *s)
{
	if (s->addr)
		return true;
	uint32_t n = s->ndwords > s->instrlen * 8u ? s->ndwords : s->instrlen * 8u;
	uint32_t *p = gpu_alloc(n * 4, 4096);
	if (!p)
		return false;
	memset(p, 0, n * 4);
	memcpy(p, s->code, s->ndwords * 4);
	s->addr = (uint32_t)(uintptr_t)p;
	return true;
}

static void load_shader(const struct gpu_shader *s, uint32_t block)
{
	pkt3(CP_LOAD_STATE, 2);
	out(LOAD_STATE_0(0, SS_INDIRECT, block, s->instrlen));
	out(s->addr | ST_SHADER);
}

static void load_consts(uint32_t block, unsigned first, const uint32_t *v, unsigned nvec4)
{
	gpu_ring_reserve(nvec4 * 4 + 4);
	pkt3(CP_LOAD_STATE, 2 + nvec4 * 4);
	out(LOAD_STATE_0(first * 2, SS_DIRECT, block, nvec4 * 2));
	out(ST_CONSTANTS);
	for (unsigned i = 0; i < nvec4 * 4; i++)
		out(v[i]);
}

static void restore(void)
{
	cp_wfi();
	pkt3(CP_INVALIDATE_STATE, 1);
	out(0x00007fff);
	reg(REG_SP_VS_PVT_MEM_PARAM_REG, 3, 0x08000001, pvt_mem, 0);
	reg(REG_SP_FS_PVT_MEM_PARAM_REG, 3, 0x08000001, pvt_mem + 0x2000, 0);
	reg(REG_PC_VERTEX_REUSE_BLOCK_CNTL, 1, 0x0000000b);
	reg(REG_GRAS_SC_CONTROL, 1, 0);
	reg(REG_RB_MSAA_CONTROL, 2, RB_MSAA_CONTROL_DISABLE | RB_MSAA_CONTROL_SAMPLE_MASK(0xffff), 0);
	reg(REG_GRAS_CL_GB_CLIP_ADJ, 1, 0);
	reg(REG_GRAS_TSE_DEBUG_ECO, 1, 1);
	reg(REG_TPL1_TP_VS_TEX_OFFSET, 1, TPL1_TEX_OFFSET(0, 0, 0));
	reg(REG_TPL1_TP_FS_TEX_OFFSET, 1, TPL1_TEX_OFFSET(FRAG_TEX_OFF, FRAG_TEX_OFF, MIP_LEVELS * FRAG_TEX_OFF));
	reg(REG_VPC_VARY_CYLWRAP_ENABLE_0, 2, 0, 0);
	reg(REG_UNKNOWN_0E43, 1, 1);
	reg(REG_UNKNOWN_0F03, 1, 1);
	reg(REG_UNKNOWN_0EE0, 1, 3);
	reg(REG_UNKNOWN_0C3D, 1, 1);
	reg(REG_HLSQ_PERFCOUNTER0_SELECT, 1, 0);
	reg(REG_HLSQ_CONST_VSPRESV_RANGE_REG, 2, 0, 0);
	cp_wfi();
	reg(REG_UCHE_CACHE_INVALIDATE0, 2, 0, UCHE_INVALIDATE_ENTIRE);
	reg(REG_GRAS_CL_CLIP_CNTL, 1, 0);
	reg(REG_GRAS_SU_POINT_MINMAX, 2, 0xffc00010, 0x00000008);
	reg(REG_PC_RESTART_INDEX, 1, 0xffffffff);
	reg(REG_RB_WINDOW_OFFSET, 1, XY(0, 0));
	reg(REG_RB_BLEND_RED, 4, RB_BLEND_COLOR(0, 0), RB_BLEND_COLOR(0, 0), RB_BLEND_COLOR(0, 0),
	    RB_BLEND_COLOR(0xff, 0x3c00));
	for (int i = 0; i < 6; i++)
		reg(REG_GRAS_CL_USER_PLANE(i), 4, 0, 0, 0, 0);
	reg(REG_PC_VSTREAM_CONTROL, 1, 0);
	event(EV_CACHE_FLUSH);
	pkt3(CP_NOP, 4);
	for (int i = 0; i < 4; i++)
		out(0);
	cp_wfi();
}

void gpu_draw_begin(const struct gpu_target *t)
{
	if (!pvt_mem) {
		pvt_mem = (uint32_t)(uintptr_t)gpu_alloc(0x4000, 4096);
		fence_ptr = gpu_alloc(32, 32);
		fence = (uint32_t)(uintptr_t)fence_ptr;
	}
	uint32_t w = t->width, h = t->height, pitch_px = t->pitch_bytes / 4;
	target_w = w;
	target_h = h;
	quad_i = 0;
	if ((t->base & 31) || (pitch_px & 31))
		log_printf("gpu: target %08lx pitch %lu not 32-byte / 32-pixel aligned\n", t->base, t->pitch_bytes);
	gpu_ring_reserve(512);
	drawn = false;
	restore();

	reg(REG_RB_FRAME_BUFFER_DIMENSION, 1, w | h << 14);
	for (int i = 0; i < 4; i++) {
		if (i == 0)
			reg(REG_RB_MRT_BUF_INFO(0), 2, RB_R8G8B8A8_UNORM | RB_MRT_BUF_INFO_SWAP(SWAP_WXYZ) |
			    RB_MRT_BUF_INFO_PITCH(t->pitch_bytes), RB_MRT_BUF_BASE(t->base));
		else
			reg(REG_RB_MRT_BUF_INFO(i), 2, 0, 0);
		reg(REG_SP_FS_IMAGE_OUTPUT_REG(i), 1, i == 0 ? RB_R8G8B8A8_UNORM : 0);
	}
	reg(REG_RB_WINDOW_OFFSET, 1, XY(0, 0));
	reg(REG_GRAS_SC_SCREEN_SCISSOR_TL, 2, XY(0, 0), XY(w - 1, h - 1));
	reg(REG_RB_MODE_CONTROL, 1, RB_MODE_CONTROL_GMEM_BYPASS | RB_MODE_CONTROL_MARB_CACHE_SPLIT);

	reg(REG_RB_RENDER_CONTROL, 1, RB_RENDER_CONTROL_BIN_WIDTH(pitch_px));
	reg(REG_RB_ALPHA_REF, 1, 0);
	reg(REG_RB_STENCIL_CONTROL, 1, 0);
	reg(REG_RB_STENCILREFMASK, 2, 0, 0);
	reg(REG_RB_DEPTH_CONTROL, 1, 0);
	reg(REG_RB_DEPTH_INFO, 2, 0, 0);
	reg(REG_GRAS_SU_MODE_CONTROL, 1, GRAS_SU_MODE_LINEHALFWIDTH(2));
	reg(REG_GRAS_SU_POLY_OFFSET_SCALE, 2, 0, 0);
	reg(REG_GRAS_SC_WINDOW_SCISSOR_TL, 2, XY(0, 0), XY(w - 1, h - 1));
	cp_wfi();
	reg(REG_GRAS_CL_VPORT_XOFFSET, 6, fui(w / 2.0f - 0.5f), fui(w / 2.0f), fui(h / 2.0f - 0.5f),
	    fui(-(h / 2.0f)), fui(0.5f), fui(0.5f));
	reg(REG_RB_Z_CLAMP_MIN, 2, 0, 0xffffff);
	for (int i = 0; i < 4; i++) {
		reg(REG_RB_MRT_CONTROL(i), 1, RB_MRT_CONTROL_ROP_CODE(ROP_COPY) |
		    RB_MRT_CONTROL_COMPONENT_ENABLE(i == 0 ? 0xf : 0));
		reg(REG_RB_MRT_BLEND_CONTROL(i), 1, RB_MRT_BLEND(FACTOR_ONE, FACTOR_ZERO) | RB_MRT_BLEND_CLAMP_ENABLE);
	}
}

bool gpu_draw_program(struct gpu_shader *vs, struct gpu_shader *fs)
{
	if (!upload(vs) || !upload(fs))
		return false;
	sync_state();
	unsigned vlen = vs->instrlen, flen = fs->instrlen, vsz = vlen, fsz = flen;
	unsigned vmode = INSTR_BUFFER, fmode = INSTR_BUFFER;
	if (vlen + flen > 128) {
		if (flen < 112) {
			vmode = INSTR_CACHE;
			vsz = 256 - flen;
		} else if (vlen < 112) {
			fmode = INSTR_CACHE;
			fsz = 256 - vlen;
		} else {
			vmode = fmode = INSTR_CACHE;
			vsz = fsz = 192;
		}
	}
	unsigned fsoff = (fmode == INSTR_BUFFER ? 128 : 256) - fsz;
	unsigned vconst = vs->constlen, fconst = fs->constlen, cmode = vconst + fconst > 256;
	fs_total_in = fs->total_in;

	struct { uint8_t regid, compmask, loc; } link[GPU_MAX_VARYINGS + 1] = {0};
	uint32_t vinterp[4] = {0}, flat[2] = {0};
	unsigned cnt = 0;
	for (unsigned i = 0; i < fs->nvar; i++) {
		if (!fs->var[i].compmask || fs->var[i].inloc >= fs_total_in)
			continue;
		link[cnt].regid = i < vs->nvar ? vs->var[i].regid : GPU_REG(0, 0);
		link[cnt].compmask = fs->var[i].compmask;
		link[cnt++].loc = fs->var[i].inloc;
		if (fs->var[i].flat)
			for (unsigned c = 0, loc = fs->var[i].inloc; c < 4; c++)
				if (fs->var[i].compmask & BIT(c)) {
					vinterp[loc / 16] |= INTERP_FLAT << (loc % 16 * 2);
					flat[loc / 32] |= BIT(loc % 32);
					loc++;
				}
	}

	gpu_ring_reserve(256);
	reg(REG_GRAS_CL_CLIP_CNTL, 1, fs->ij_regid != GPU_REG_NONE ? GRAS_CL_CLIP_CNTL_IJ_PERSP_CENTER : 0);
	reg(REG_HLSQ_CONTROL_0_REG, 6,
	    HLSQ_CONTROL_0_FSTHREADSIZE(FOUR_QUADS) | HLSQ_CONTROL_0_FSSUPERTHREAD | HLSQ_CONTROL_0_CONSTMODE(cmode) |
	    HLSQ_CONTROL_0_SPSHADERRESTART | HLSQ_CONTROL_0_SPCONSTFULLUPDATE,
	    HLSQ_CONTROL_1_VSTHREADSIZE(TWO_QUADS) | HLSQ_CONTROL_1_VSSUPERTHREAD |
	    GPU_REG_NONE << 16 | GPU_REG_NONE << 24,
	    31u << 26 | GPU_REG_NONE << 2,
	    fs->ij_regid | GPU_REG_NONE << 8 | GPU_REG_NONE << 16 | GPU_REG_NONE << 24,
	    HLSQ_STAGE_CONTROL(vconst, 0, vsz),
	    HLSQ_STAGE_CONTROL(fconst, 128, fsz));
	reg(REG_SP_SP_CTRL_REG, 1, SP_CTRL_CONSTMODE(cmode) | SP_CTRL_SLEEPMODE(1));
	reg(REG_SP_VS_LENGTH_REG, 1, vlen);
	reg(REG_SP_VS_CTRL_REG0, 3,
	    SP_CTRL_REG0_BUFFERMODE(vmode) | (vmode == INSTR_CACHE ? SP_CTRL_REG0_CACHEINVALID : 0) |
	    SP_CTRL_REG0_FOOTPRINT(footprint(vs->max_half_reg), footprint(vs->max_reg)) |
	    SP_CTRL_REG0_THREADSIZE(TWO_QUADS) | SP_CTRL_REG0_SUPERTHREADMODE | SP_CTRL_REG0_LENGTH(vsz),
	    SP_CTRL_REG1_CONST(vconst) | SP_VS_CTRL_REG1_INITIALOUTSTANDING(vs->total_in),
	    SP_VS_PARAM_REG(vs->pos_regid, GPU_REG_NONE, fs->varying_in));
	for (unsigned i = 0; i < (cnt + 1) / 2; i++)
		reg(REG_SP_VS_OUT_REG(i), 1, link[2 * i].regid | link[2 * i].compmask << 9 |
		    link[2 * i + 1].regid << 16 | link[2 * i + 1].compmask << 25);
	for (unsigned i = 0; i < (cnt + 3) / 4; i++) {
		uint32_t v = 0;
		for (unsigned k = 0; k < 4; k++)
			v |= (4 * i + k < cnt ? link[4 * i + k].loc + 8u : 8u) << (8 * k);
		reg(REG_SP_VS_VPC_DST_REG(i), 1, v);
	}
	reg(REG_SP_VS_OBJ_OFFSET_REG, 2, SP_OBJ_OFFSET(0, 0), vs->addr);
	reg(REG_SP_FS_LENGTH_REG, 1, flen);
	reg(REG_SP_FS_CTRL_REG0, 2,
	    SP_CTRL_REG0_BUFFERMODE(fmode) | (fmode == INSTR_CACHE ? SP_CTRL_REG0_CACHEINVALID : 0) |
	    SP_CTRL_REG0_FOOTPRINT(footprint(fs->max_half_reg), footprint(fs->max_reg)) |
	    SP_FS_CTRL_REG0_INOUTREGOVERLAP | SP_CTRL_REG0_THREADSIZE(FOUR_QUADS) |
	    SP_CTRL_REG0_SUPERTHREADMODE | (fs->pixlod ? SP_FS_CTRL_REG0_PIXLODENABLE : 0) | SP_CTRL_REG0_LENGTH(fsz),
	    SP_CTRL_REG1_CONST(fconst) | SP_FS_CTRL_REG1_INITIALOUTSTANDING(fs->sysval_in) |
	    SP_FS_CTRL_REG1_HALFPRECVAROFFSET(63));
	reg(REG_SP_FS_OBJ_OFFSET_REG, 2, SP_OBJ_OFFSET(vconst > 128 ? vconst : 128, fsoff), fs->addr);
	reg(REG_SP_FS_OUTPUT_REG, 1, SP_FS_OUTPUT_REG_DEPTH_REGID(GPU_REG_NONE));
	uint32_t mrt = fs->color_regid | (fs->color_half ? SP_FS_MRT_REG_HALF_PRECISION : 0);
	reg(REG_SP_FS_MRT_REG(0), 4, mrt, mrt, mrt, mrt);
	reg(REG_VPC_ATTR, 2, VPC_ATTR(fs_total_in), VPC_PACK(fs_total_in));
	reg(REG_VPC_VARYING_INTERP_MODE(0), 4, vinterp[0], vinterp[1], vinterp[2], vinterp[3]);
	reg(REG_VPC_VARYING_PS_REPL_MODE(0), 4, 0, 0, 0, 0);
	reg(REG_SP_FS_FLAT_SHAD_MODE_REG_0, 2, flat[0], flat[1]);
	if (vmode == INSTR_BUFFER)
		load_shader(vs, SB_VERT_SHADER);
	reg(REG_VFD_PERFCOUNTER0_SELECT, 1, 0);
	if (fmode == INSTR_BUFFER)
		load_shader(fs, SB_FRAG_SHADER);
	reg(REG_VFD_PERFCOUNTER0_SELECT, 1, 0);
	event(EV_HLSQ_FLUSH);
	if (vs->imm && vs->imm_nvec4)
		load_consts(SB_VERT_SHADER, vs->imm_base, vs->imm, vs->imm_nvec4);
	if (fs->imm && fs->imm_nvec4)
		load_consts(SB_FRAG_SHADER, fs->imm_base, fs->imm, fs->imm_nvec4);
	cur_vs = vs;
	return true;
}

void gpu_draw_consts(int stage, unsigned first_vec4, const float *v, unsigned nvec4)
{
	sync_state();
	gpu_ring_reserve(4);
	event(EV_HLSQ_FLUSH);
	load_consts(stage == GPU_STAGE_VS ? SB_VERT_SHADER : SB_FRAG_SHADER, first_vec4, (const uint32_t *)v, nvec4);
}

void gpu_draw_triangles(uint32_t vbo_addr, unsigned stride, unsigned nattrs, const uint8_t *attr_ncomp, unsigned nverts)
{
	const struct gpu_shader *vs = cur_vs;
	if (!vs) {
		log_printf("gpu: draw without a program\n");
		return;
	}
	if (nattrs > vs->nattr)
		nattrs = vs->nattr;
	int last = -1;
	for (unsigned i = 0; i < nattrs; i++)
		if (vs->attr[i].compmask)
			last = i;

	gpu_ring_reserve(128);
	unsigned j = 0, total = 0;
	for (unsigned i = 0, off = 0; i < nattrs; off += attr_ncomp[i] * 4, i++) {
		if (!vs->attr[i].compmask || (int)i > last)
			continue;
		unsigned size = attr_ncomp[i] * 4, next = (int)i != last;
		reg(REG_VFD_FETCH_INSTR_0(j), 2, VFD_FETCH_INSTR_0(size, stride, next, j), vbo_addr + off);
		reg(REG_VFD_DECODE_INSTR(j), 1, VFD_DECODE_INSTR(vs->attr[i].compmask, VFMT_32_FLOAT + attr_ncomp[i] - 1,
		    SWAP_WZYX, vs->attr[i].regid, size, next));
		total += __builtin_popcount(vs->attr[i].compmask);
		j++;
	}
	if (last < 0) {
		reg(REG_VFD_FETCH_INSTR_0(0), 2, VFD_FETCH_INSTR_0(1, 0, 0, 0), vs->addr);
		reg(REG_VFD_DECODE_INSTR(0), 1, VFD_DECODE_INSTR(1, VFMT_8_UNORM, SWAP_XYZW, GPU_REG(0, 0), 1, 0));
		total = j = 1;
	}
	reg(REG_VFD_CONTROL_0, 2, VFD_CONTROL_0(total, j), VFD_CONTROL_1(GPU_REG_NONE, GPU_REG_NONE));
	reg(REG_VFD_VS_THREADING_THRESHOLD, 1, 15 | GPU_REG_NONE << 8);

	unsigned stride_in_vpc = (fs_total_in + 3) / 4;
	if (stride_in_vpc == 1)
		stride_in_vpc = 2;
	reg(REG_PC_PRIM_VTX_CNTL, 1, stride_in_vpc | PC_PRIM_VTX_CNTL_POLYMODE(PC_DRAW_TRIANGLES, PC_DRAW_TRIANGLES) |
	    PC_PRIM_VTX_CNTL_PROVOKING_LAST);
	reg(REG_PC_VERTEX_REUSE_BLOCK_CNTL, 1, 0x0000000b);
	reg(REG_VFD_INDEX_MIN, 4, 0, 0xffffffff, 0, 0);
	reg(REG_PC_RESTART_INDEX, 1, 0xffffffff);
	pkt3(CP_DRAW_INDX, 3);
	out(0);
	out(DI_PT_TRILIST | DI_SRC_SEL_AUTO_INDEX << 6 | DRAW_PRE_DRAW_INITIATOR_ENABLE);
	out(nverts);
	drawn = true;
}

void gpu_draw_texture(int unit, uint32_t addr, uint32_t width, uint32_t height, uint32_t pitch_bytes)
{
	if (unit < 0 || unit >= FRAG_TEX_UNITS) {
		log_printf("gpu: no texture unit %d\n", unit);
		return;
	}
	if ((addr & 31) || (pitch_bytes & 127))
		log_printf("gpu: texture %08lx pitch %lu not 32-byte / 128-byte aligned\n", addr, pitch_bytes);
	unsigned slot = FRAG_TEX_OFF + unit;
	sync_state();
	gpu_ring_reserve(2 + 4 + 6 + 2 + MIP_LEVELS);
	event(EV_HLSQ_FLUSH);
	pkt3(CP_LOAD_STATE, 4);
	out(LOAD_STATE_0(slot, SS_DIRECT, SB_FRAG_TEX, 1));
	out(ST_SHADER);
	out(TEX_SAMP_0(TEX_NEAREST, TEX_CLAMP_TO_EDGE));
	out(TEX_SAMP_1_MAX_LOD(8));
	pkt3(CP_LOAD_STATE, 6);
	out(LOAD_STATE_0(slot, SS_DIRECT, SB_FRAG_TEX, 1));
	out(ST_CONSTANTS);
	out(TEX_CONST_0(TEX_SWIZ_XYZW, TFMT_8_8_8_8_UNORM, TEX_2D));
	out(TEX_CONST_1(width, height, TEX_PITCHALIGN_128B));
	out(TEX_CONST_2(MIP_LEVELS * unit, pitch_bytes));
	out(0);
	pkt3(CP_LOAD_STATE, 2 + MIP_LEVELS);
	out(LOAD_STATE_0(MIP_LEVELS * slot, SS_DIRECT, SB_FRAG_MIPADDR, MIP_LEVELS));
	out(ST_CONSTANTS);
	out(addr);
	for (int i = 1; i < MIP_LEVELS; i++)
		out(0);
}

void gpu_draw_blend(bool premultiplied_over)
{
	sync_state();
	gpu_ring_reserve(4);
	reg(REG_RB_MRT_CONTROL(0), 1, RB_MRT_CONTROL_ROP_CODE(ROP_COPY) | RB_MRT_CONTROL_COMPONENT_ENABLE(0xf) |
	    (premultiplied_over ? RB_MRT_CONTROL_READ_DEST_ENABLE | RB_MRT_CONTROL_BLEND | RB_MRT_CONTROL_BLEND2 : 0));
	reg(REG_RB_MRT_BLEND_CONTROL(0), 1, RB_MRT_BLEND_CLAMP_ENABLE |
	    (premultiplied_over ? RB_MRT_BLEND(FACTOR_ONE, FACTOR_ONE_MINUS_SRC_ALPHA) : RB_MRT_BLEND(FACTOR_ONE, FACTOR_ZERO)));
}

static float *quad(int x, int y, uint32_t width, uint32_t height)
{
	if (!quads)
		quads = gpu_alloc(QUADS * QUAD_FLOATS * 4, 64);
	if (!quads || quad_i == QUADS) {
		if (quads)
			log_printf("gpu: more than %u quads in a frame\n", QUADS);
		return NULL;
	}
	float x0 = 2.0f * x / target_w - 1.0f, x1 = 2.0f * (x + (int)width) / target_w - 1.0f;
	float y0 = 1.0f - 2.0f * y / target_h, y1 = 1.0f - 2.0f * (y + (int)height) / target_h;
	float v[QUAD_FLOATS] = {
		x0, y0, 0, 1, 0, 0,
		x1, y0, 0, 1, 1, 0,
		x0, y1, 0, 1, 0, 1,
		x1, y0, 0, 1, 1, 0,
		x1, y1, 0, 1, 1, 1,
		x0, y1, 0, 1, 0, 1,
	};
	float *q = quads + quad_i++ * QUAD_FLOATS;
	memcpy(q, v, sizeof v);
	return q;
}

void gpu_draw_layer_at(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y, float alpha)
{
	float *q = quad(x, y, width, height);
	if (!q)
		return;
	bool faded = alpha < 1.0f;
	if (!gpu_draw_program(&shader_d_vs, faded ? &shader_e_fs : &shader_d_fs))
		return;
	if (faded)
		gpu_draw_consts(GPU_STAGE_FS, 0, (const float[]){ alpha, alpha, alpha, alpha }, 1);
	gpu_draw_texture(0, tex_addr, width, height, pitch_bytes);
	gpu_draw_blend(true);
	gpu_draw_triangles((uint32_t)(uintptr_t)q, 24, 2, (const uint8_t[]){4, 2}, 6);
	gpu_draw_blend(false);
}

void gpu_draw_layer(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes)
{
	gpu_draw_layer_at(tex_addr, width, height, pitch_bytes, 0, 0, 1.0f);
}

void gpu_draw_fill(int x, int y, uint32_t width, uint32_t height, float r, float g, float b, float a)
{
	float *q = quad(x, y, width, height);
	if (!q)
		return;
	if (!gpu_draw_program(&shader_a_vs, &shader_b_fs))
		return;
	gpu_draw_consts(GPU_STAGE_FS, 0, (const float[]){ r, g, b, a }, 1);
	gpu_draw_blend(true);
	gpu_draw_triangles((uint32_t)(uintptr_t)q, 24, 1, (const uint8_t[]){4}, 6);
	gpu_draw_blend(false);
}

bool gpu_draw_end(void)
{
	if (!fence_ptr)
		return false;
	uint32_t seq = ++seqno;
	gpu_ring_reserve(16);
	event(EV_HLSQ_FLUSH);
	cp_wfi();
	pkt3(CP_EVENT_WRITE, 3);
	out(EV_CACHE_FLUSH_TS);
	out(fence);
	out(seq);
	bool ok = gpu_ring_flush(1000);
	uint32_t t0 = time_ms();
	while (ok && *fence_ptr != seq)
		if (time_ms() - t0 > 100) {
			log_printf("gpu: cache flush fence %lu, expected %lu\n", *fence_ptr, seq);
			ok = false;
		}
	return ok;
}

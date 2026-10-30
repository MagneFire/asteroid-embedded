/* Triangle drawing on the Adreno 305B into a linear XRGB8888 buffer. A
 * shader stage is an ir3 binary plus what the hardware has to be told
 * about it, all printed by the compiler: instrlen (units of 8 dwords),
 * constlen (vec4s), max_reg / max_half_reg (-1 for none), total_in,
 * sysval_in and varying_in. Register numbers are GPU_REG(n, c) for rn.c
 * and GPU_REG_NONE when absent. imm holds immediates the compiler placed
 * in the constant file ("@const(cN.x)" lines) starting at vec4 imm_base.
 * VS: attr[i] is the register and component mask of vertex attribute i,
 * var[i].regid the register varying i is written from, pos_regid that of
 * the position. FS: var[i].inloc and var[i].compmask are the location
 * and mask of varying i, var[i].flat selects flat shading, ij_regid is
 * where the barycentrics arrive, color_regid/color_half the colour
 * output, pixlod is set when the shader samples a texture with implicit
 * LOD. Varying i of the VS feeds varying i of the FS. addr is set when
 * the code is first copied into GPU memory. Textures are linear
 * R8G8B8A8_UNORM (bytes R, G, B, A in memory), one level, nearest
 * filtering, clamped to the edge; the base address must be 32-byte
 * aligned and the pitch a multiple of 128 bytes (32 pixels).
 * gpu_draw_texture_format can instead read 8-bit texels, or the alpha of
 * RGBA texels, into all four channels, and with GPU_TEX_LINEAR filter
 * linearly. The texture cache is invalidated at gpu_draw_begin, so the
 * CPU only has to clean the D-cache over the texels it wrote before
 * gpu_draw_end, and must not write them again until gpu_draw_end has
 * returned; texels written after a draw of the same submission need
 * gpu_draw_invalidate_textures before the next. Blending stays as set by
 * gpu_draw_blend until the next gpu_draw_begin, which turns it off.
 * gpu_draw_layer_at draws a premultiplied RGBA layer with its top-left
 * corner at pixel (x, y) of the target, source-over blended and faded by
 * alpha (texture unit 0); gpu_draw_layer_parts draws only the given
 * rectangles of it, in layer pixels, in one draw; gpu_draw_layer is the
 * whole layer at (0, 0), opaque. */
#ifndef GPU_DRAW_H
#define GPU_DRAW_H
#include <stdint.h>
#include <stdbool.h>

#define GPU_REG(n, c)		((n) * 4 + (c))
#define GPU_REG_NONE		GPU_REG(63, 0)
#define GPU_MAX_ATTRS		8
#define GPU_MAX_VARYINGS	8
#define GPU_STAGE_VS		0
#define GPU_STAGE_FS		1

struct gpu_shader {
	const uint32_t *code;
	uint32_t ndwords;
	uint8_t instrlen, constlen;
	int8_t max_reg, max_half_reg;
	uint8_t total_in, sysval_in, varying_in;
	const uint32_t *imm;
	uint8_t imm_base, imm_nvec4;
	uint8_t nattr;
	struct { uint8_t regid, compmask; } attr[GPU_MAX_ATTRS];
	uint8_t nvar;
	struct { uint8_t regid, compmask, inloc; bool flat; } var[GPU_MAX_VARYINGS];
	uint8_t pos_regid;
	uint8_t ij_regid;
	uint8_t color_regid;
	bool color_half;
	bool pixlod;
	uint32_t addr;
};

enum gpu_tex { GPU_TEX_RGBA, GPU_TEX_ALPHA, GPU_TEX_RGBA_ALPHA, GPU_TEX_LINEAR = 0x100 };

struct gpu_target {
	uint32_t base;
	uint32_t width, height, pitch_bytes;
};

struct gpu_rect {
	int x, y;
	uint32_t width, height;
};

void *gpu_alloc(uint32_t size, uint32_t align);
void gpu_draw_begin(const struct gpu_target *t);
bool gpu_draw_program(struct gpu_shader *vs, struct gpu_shader *fs);
void gpu_draw_consts(int stage, unsigned first_vec4, const float *v, unsigned nvec4);
void gpu_draw_texture(int unit, uint32_t addr, uint32_t width, uint32_t height, uint32_t pitch_bytes);
void gpu_draw_texture_format(int unit, uint32_t addr, uint32_t width, uint32_t height, uint32_t pitch_bytes,
			     enum gpu_tex format);
void gpu_draw_invalidate_textures(void);
void gpu_draw_blend(bool premultiplied_over);
void gpu_draw_triangles(uint32_t vbo_addr, unsigned stride, unsigned nattrs, const uint8_t *attr_ncomp, unsigned nverts);
void gpu_draw_layer(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes);
void gpu_draw_layer_at(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y, float alpha);
void gpu_draw_layer_parts(uint32_t tex_addr, uint32_t width, uint32_t height, uint32_t pitch_bytes, int x, int y, float alpha,
			  const struct gpu_rect *parts, unsigned n);
bool gpu_draw_end(void);
#endif

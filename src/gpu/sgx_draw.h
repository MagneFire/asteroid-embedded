/* Triangles on the SGX530, drawn the way the vendor's GLES driver draws
 * them on this watch: its whole scene for one 320x320 surface (render
 * context, parameter buffer, render targets, both shaders, state and
 * control streams) is set up at the GPU addresses the driver gave it,
 * from data recorded off the driver (sgx_scene_data.c), and a frame
 * writes what the driver would for its draws: a clear, then a list of
 * draws of flat-coloured or textured triangles, in two kicks, the first
 * with the geometry, the second ending the scene, which the GPU then
 * renders. Frames run one at a time; each waits for its render.
 *
 * Pixels land as 32-bit words 0xAARRGGBB, rows of 1280 bytes, the
 * layout of the display's XRGB8888 frame buffer, with GL's origin: the
 * bottom row of the picture (clip-space y = -1) is the target's first.
 * The CPU must neither hold dirty cache lines of the target while a
 * frame runs nor read it from its cache afterwards without
 * invalidating, and must have cleaned what it wrote of textures.
 *
 * Draws are as recorded: the first is flat, a flat draw after it only
 * after a textured one and without blending, blending of the first only
 * when it is the only flat one; textured draws blend premultiplied
 * (colour + target * (1 - alpha)). A texture is width * height texels
 * of bytes R, G, B, A, rows of width * 4 bytes, page aligned; widths a
 * multiple of 32 and neither side a power of two. */
#ifndef SGX_DRAW_H
#define SGX_DRAW_H
#include <stdint.h>
#include <stdbool.h>

#define SGX_DRAW_SIZE		320
#define SGX_DRAW_MAX_VERTS	2048
#define SGX_DRAW_MAX_DRAWS	64

enum { SGX_DRAW_FLAT, SGX_DRAW_TEXTURED };

struct sgx_draw {
	uint32_t verts;		/* GPU address of floats: flat x, y, r, g, b, a; textured x, y, u, v */
	uint16_t count;		/* vertices, three a triangle */
	uint8_t kind;
	bool blend;
	uint32_t tex;		/* GPU address */
	uint16_t width, height;
	uint8_t alpha;		/* the texels times alpha / 255 */
	bool linear;
};

struct sgx_frame {
	uint32_t clear;		/* 0xAARRGGBB */
	const struct sgx_draw *draws;
	unsigned n;
};

struct sgx_times {
	uint32_t ta_us, render_us;	/* from each kick to its completion */
};

/* Once, after sgx_start. The target is then the scene's own buffer. The
 * scene stays in memory while the GPU is off; after sgx_start again
 * frames go on from where they were. */
bool sgx_draw_setup(void);
/* A GPU address for page-aligned memory outside the GPU's pool, or 0. */
uint32_t sgx_draw_map(uint32_t phys, uint32_t size);
/* Memory of the GPU's own, zeroed, uncached for the CPU, or 0. */
uint32_t sgx_draw_alloc(uint32_t size, void **cpu);
/* Room for vertices in the scene's vertex buffer, for the next frame
 * only, or NULL. */
void *sgx_draw_vertices(uint32_t bytes, uint32_t *gpu);
void sgx_draw_target(uint32_t gpu_addr);
uint32_t sgx_draw_get_target(void);
bool sgx_draw_frame(const struct sgx_frame *f, struct sgx_times *t);
/* After a frame that failed: why, as a line for the log. */
const char *sgx_draw_failure(void);
#endif

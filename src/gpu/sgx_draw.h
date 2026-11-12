/* Triangles on the SGX530, drawn the way the vendor's GLES driver draws
 * them on this watch: its whole scene for one 320x320 surface (render
 * context, parameter buffer, render targets, shaders, state and control
 * streams) is set up at the GPU addresses the driver gave it, from data
 * recorded off the driver (sgx_scene_data.c), and a frame rewrites only
 * what differs: vertices, clear colour, blending, the target, and the
 * counters and commands of its two kicks, the first with the geometry,
 * the second ending the scene, which the GPU then renders. Frames run
 * one at a time; each waits for its render.
 *
 * Pixels land as 32-bit words 0xAARRGGBB, rows of 1280 bytes, the
 * layout of the display's XRGB8888 frame buffer. The CPU must neither
 * hold dirty cache lines of the target while a frame runs nor read it
 * from its cache afterwards without invalidating. */
#ifndef SGX_DRAW_H
#define SGX_DRAW_H
#include <stdint.h>
#include <stdbool.h>

#define SGX_DRAW_SIZE		320
#define SGX_DRAW_MAX_TRIS	56

struct sgx_frame {
	uint32_t clear;		/* 0xAARRGGBB */
	const float *verts;	/* x, y in clip space, r, g, b, a: six floats a corner */
	unsigned tris;
	bool blend;		/* colour + target * (1 - alpha) */
};

struct sgx_times {
	uint32_t ta_us, render_us;	/* from each kick to its completion */
};

/* Once, after sgx_start. The target is then the scene's own buffer. */
bool sgx_draw_setup(void);
/* A GPU address for page-aligned memory outside the GPU's pool, or 0. */
uint32_t sgx_draw_map(uint32_t phys, uint32_t size);
void sgx_draw_target(uint32_t gpu_addr);
uint32_t sgx_draw_get_target(void);
bool sgx_draw_frame(const struct sgx_frame *f, struct sgx_times *t);
#endif

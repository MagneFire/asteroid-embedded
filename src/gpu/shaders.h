/* Test shaders compiled by ir3 for the Adreno 305B (chip id 0x03000520).
 * a: VS, gl_Position = pos (attribute 0, vec4).
 * b: FS, gl_FragColor = color (uniform vec4 in c0).
 * c: VS + FS, gl_Position = pos (attribute 0), colour (attribute 1)
 *    passed as varying 0 and written out unchanged.
 * d: VS + FS, gl_Position = pos (attribute 0), texcoord (attribute 1,
 *    vec2) passed as varying 0, gl_FragColor = texture2D(tex, uv) with
 *    tex on sampler 0.
 * a + b draw a solid triangle, c a per-vertex coloured one, d a textured
 * one. */
#ifndef GPU_SHADERS_H
#define GPU_SHADERS_H
#include "asteroid.h"
#include "draw.h"

static const uint32_t shader_a_vs_code[] = {
	0x00000000, 0x03000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
};

static const uint32_t shader_b_fs_code[] = {
	0x00000000, 0x202cc000,
	0x00000001, 0x202cc001,
	0x00000002, 0x202cc002,
	0x00000003, 0x202cc003,
	0x00000000, 0x03000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
};

static const uint32_t shader_c_vs_code[] = {
	0x00000000, 0x03000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
};

static const uint32_t shader_c_fs_code[] = {
	0x00002000, 0x47300002,
	0x00002001, 0x47300003,
	0x00002002, 0x47300004,
	0x00002003, 0x47308005,
	0x00000000, 0x03000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
};

static const uint32_t shader_d_vs_code[] = {
	0x00000000, 0x03000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
};

static const uint32_t shader_d_fs_code[] = {
	0x00002000, 0x47300002,
	0x00002001, 0x47308003,
	0x00000000, 0x00000500,
	0x00000005, 0xa0c01f04,
	0x00000000, 0x13000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
};

static struct gpu_shader shader_a_vs __attribute__((unused)) = {
	.code = shader_a_vs_code, .ndwords = ARRAY_SIZE(shader_a_vs_code),
	.instrlen = 1, .constlen = 0, .max_reg = 0, .max_half_reg = -1,
	.total_in = 4, .sysval_in = 0, .varying_in = 0,
	.nattr = 1, .attr = { { GPU_REG(0, 0), 0xf } },
	.pos_regid = GPU_REG(0, 0),
	.ij_regid = GPU_REG_NONE, .color_regid = GPU_REG_NONE,
};

static struct gpu_shader shader_b_fs __attribute__((unused)) = {
	.code = shader_b_fs_code, .ndwords = ARRAY_SIZE(shader_b_fs_code),
	.instrlen = 2, .constlen = 1, .max_reg = 0, .max_half_reg = -1,
	.total_in = 0, .sysval_in = 2, .varying_in = 0,
	.pos_regid = GPU_REG_NONE,
	.ij_regid = GPU_REG_NONE, .color_regid = GPU_REG(0, 0),
};

static struct gpu_shader shader_c_vs __attribute__((unused)) = {
	.code = shader_c_vs_code, .ndwords = ARRAY_SIZE(shader_c_vs_code),
	.instrlen = 1, .constlen = 0, .max_reg = 1, .max_half_reg = -1,
	.total_in = 8, .sysval_in = 0, .varying_in = 0,
	.nattr = 2, .attr = { { GPU_REG(1, 0), 0xf }, { GPU_REG(0, 0), 0xf } },
	.nvar = 1, .var = { { .regid = GPU_REG(0, 0), .compmask = 0xf } },
	.pos_regid = GPU_REG(1, 0),
	.ij_regid = GPU_REG_NONE, .color_regid = GPU_REG_NONE,
};

static struct gpu_shader shader_c_fs __attribute__((unused)) = {
	.code = shader_c_fs_code, .ndwords = ARRAY_SIZE(shader_c_fs_code),
	.instrlen = 2, .constlen = 0, .max_reg = 1, .max_half_reg = -1,
	.total_in = 4, .sysval_in = 2, .varying_in = 1,
	.nvar = 1, .var = { { .regid = GPU_REG_NONE, .compmask = 0xf, .inloc = 0 } },
	.pos_regid = GPU_REG_NONE,
	.ij_regid = GPU_REG(0, 0), .color_regid = GPU_REG(0, 2),
};

static struct gpu_shader shader_d_vs __attribute__((unused)) = {
	.code = shader_d_vs_code, .ndwords = ARRAY_SIZE(shader_d_vs_code),
	.instrlen = 1, .constlen = 0, .max_reg = 1, .max_half_reg = -1,
	.total_in = 6, .sysval_in = 0, .varying_in = 0,
	.nattr = 2, .attr = { { GPU_REG(0, 2), 0xf }, { GPU_REG(0, 0), 0x3 } },
	.nvar = 1, .var = { { .regid = GPU_REG(0, 0), .compmask = 0x3 } },
	.pos_regid = GPU_REG(0, 2),
	.ij_regid = GPU_REG_NONE, .color_regid = GPU_REG_NONE,
};

static struct gpu_shader shader_d_fs __attribute__((unused)) = {
	.code = shader_d_fs_code, .ndwords = ARRAY_SIZE(shader_d_fs_code),
	.instrlen = 2, .constlen = 0, .max_reg = 1, .max_half_reg = -1,
	.total_in = 2, .sysval_in = 2, .varying_in = 1,
	.nvar = 1, .var = { { .regid = GPU_REG_NONE, .compmask = 0x3, .inloc = 0 } },
	.pos_regid = GPU_REG_NONE,
	.ij_regid = GPU_REG(0, 0), .color_regid = GPU_REG(1, 0),
	.pixlod = true,
};
#endif

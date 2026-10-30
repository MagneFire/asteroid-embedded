/* Test shaders compiled by ir3 for the Adreno 305B (chip id 0x03000520).
 * a: VS, gl_Position = pos (attribute 0, vec4).
 * b: FS, gl_FragColor = color (uniform vec4 in c0).
 * c: VS + FS, gl_Position = pos (attribute 0), colour (attribute 1)
 *    passed as varying 0 and written out unchanged.
 * d: VS + FS, gl_Position = pos (attribute 0), texcoord (attribute 1,
 *    vec2) passed as varying 0, gl_FragColor = texture2D(tex, uv) with
 *    tex on sampler 0.
 * e: FS for d's VS, gl_FragColor = texture2D(tex, uv) * tint with tint
 *    a uniform vec4 in c0.
 * f: VS + FS, gl_Position = pos (attribute 0), texcoord (attribute 1,
 *    vec2) and colour (attribute 2) passed as varyings 1 and 0,
 *    gl_FragColor = texture2D(tex, uv) * colour.
 * g: f with the pixel position passed on (to_px, a uniform vec4 in the
 *    VS's c0, maps clip space to pixels) and the colour multiplied by the
 *    coverage of a rounded rectangle: centre in the VS's c1.xy, half size
 *    less the radius in the FS's c0.zw, radius in the FS's c1.x.
 * a + b draw a solid triangle, c a per-vertex coloured one, d a textured
 * one, d's VS + e a textured one scaled by a colour, f one scaled by
 * a per-vertex colour and g that clipped to rounded corners. */
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

static const uint32_t shader_e_fs_code[] = {
	0x00002000, 0x47300002,
	0x00002001, 0x47308003,
	0x00000000, 0x00000500,
	0x00000005, 0xa0c01f04,
	0x10000004, 0x50780b08,
	0x00000000, 0x03000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
};

static const uint32_t shader_f_vs_code[] = {
	0x00000000, 0x03000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
};

static const uint32_t shader_f_fs_code[] = {
	0x00002004, 0x47300002,
	0x00002005, 0x47300003,
	0x00002000, 0x47300004,
	0x00002001, 0x47300005,
	0x00002002, 0x47300006,
	0x00002003, 0x47308007,
	0x00000000, 0x00000100,
	0x00000005, 0xa0c01f08,
	0x00040008, 0x50780b0c,
	0x00000000, 0x03000000,
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

static struct gpu_shader shader_e_fs __attribute__((unused)) = {
	.code = shader_e_fs_code, .ndwords = ARRAY_SIZE(shader_e_fs_code),
	.instrlen = 2, .constlen = 1, .max_reg = 2, .max_half_reg = -1,
	.total_in = 2, .sysval_in = 2, .varying_in = 1,
	.nvar = 1, .var = { { .regid = GPU_REG_NONE, .compmask = 0x3, .inloc = 0 } },
	.pos_regid = GPU_REG_NONE,
	.ij_regid = GPU_REG(0, 0), .color_regid = GPU_REG(2, 0),
	.pixlod = true,
};

static struct gpu_shader shader_f_vs __attribute__((unused)) = {
	.code = shader_f_vs_code, .ndwords = ARRAY_SIZE(shader_f_vs_code),
	.instrlen = 1, .constlen = 0, .max_reg = 2, .max_half_reg = -1,
	.total_in = 10, .sysval_in = 0, .varying_in = 0,
	.nattr = 3, .attr = { { GPU_REG(0, 0), 0xf }, { GPU_REG(1, 0), 0x3 }, { GPU_REG(1, 2), 0xf } },
	.nvar = 2, .var = { { .regid = GPU_REG(1, 2), .compmask = 0xf }, { .regid = GPU_REG(1, 0), .compmask = 0x3 } },
	.pos_regid = GPU_REG(0, 0),
	.ij_regid = GPU_REG_NONE, .color_regid = GPU_REG_NONE,
};

static struct gpu_shader shader_f_fs __attribute__((unused)) = {
	.code = shader_f_fs_code, .ndwords = ARRAY_SIZE(shader_f_fs_code),
	.instrlen = 3, .constlen = 0, .max_reg = 3, .max_half_reg = -1,
	.total_in = 6, .sysval_in = 2, .varying_in = 2,
	.nvar = 2, .var = { { .regid = GPU_REG_NONE, .compmask = 0xf, .inloc = 0 },
			    { .regid = GPU_REG_NONE, .compmask = 0x3, .inloc = 4 } },
	.pos_regid = GPU_REG_NONE,
	.ij_regid = GPU_REG(0, 0), .color_regid = GPU_REG(3, 0),
	.pixlod = true,
};
static const uint32_t shader_g_vs_code[] = {
	0x10021000, 0x6384000c,
	0x10031001, 0x6384800d,
	0x00000000, 0x00000100,
	0x5004000c, 0x40100006,
	0x5005000d, 0x40100007,
	0x00000000, 0x03000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
};

static const uint32_t shader_g_fs_code[] = {
	0x00002004, 0x47300004,
	0x00002005, 0x47300005,
	0x00002007, 0x47300003,
	0x00002006, 0x47300002,
	0x00002000, 0x47300006,
	0x00002001, 0x47300007,
	0x00002002, 0x47300008,
	0x00002003, 0x47308009,
	0x00000009, 0xa0c01f0a,
	0x50038003, 0x40100003,
	0x50028002, 0x40100002,
	0x28011004, 0x4010000e,
	0x00000000, 0x00000000,
	0x28000003, 0x40500003,
	0x28000002, 0x40500002,
	0x00000000, 0x00000100,
	0x00030003, 0x40700003,
	0x00000000, 0x00000000,
	0x00030002, 0x63810002,
	0x00000000, 0x00000500,
	0x00000002, 0x80d00002,
	0x4002000e, 0x4010140e,
	0x0006000a, 0x50780b0a,
	0x000e000a, 0x40700b0f,
	0x00000000, 0x03000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
	0x00000000, 0x00000000,
};

static struct gpu_shader shader_g_vs __attribute__((unused)) = {
	.code = shader_g_vs_code, .ndwords = ARRAY_SIZE(shader_g_vs_code),
	.instrlen = 2, .constlen = 2, .max_reg = 3, .max_half_reg = -1,
	.total_in = 10, .sysval_in = 0, .varying_in = 0,
	.nattr = 3, .attr = { { GPU_REG(2, 0), 0xf }, { GPU_REG(1, 0), 0x3 }, { GPU_REG(0, 0), 0xf } },
	.nvar = 2, .var = { { .regid = GPU_REG(0, 0), .compmask = 0xf }, { .regid = GPU_REG(1, 0), .compmask = 0xf } },
	.pos_regid = GPU_REG(2, 0),
	.ij_regid = GPU_REG_NONE, .color_regid = GPU_REG_NONE,
};

static struct gpu_shader shader_g_fs __attribute__((unused)) = {
	.code = shader_g_fs_code, .ndwords = ARRAY_SIZE(shader_g_fs_code),
	.instrlen = 7, .constlen = 2, .max_reg = 4, .max_half_reg = -1,
	.total_in = 8, .sysval_in = 2, .varying_in = 2,
	.nvar = 2, .var = { { .regid = GPU_REG_NONE, .compmask = 0xf, .inloc = 0 },
			    { .regid = GPU_REG_NONE, .compmask = 0xf, .inloc = 4 } },
	.pos_regid = GPU_REG_NONE,
	.ij_regid = GPU_REG(0, 0), .color_regid = GPU_REG(3, 3),
	.pixlod = true,
};

#endif

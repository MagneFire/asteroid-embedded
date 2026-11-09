#ifndef DISPLAY_H
#define DISPLAY_H
#include <stdint.h>
#include <stdbool.h>

struct fb {
	uint32_t *buf[2];
	/* height is the round picture, panel_height what the panel shows of
	 * it: less with a flat tire, the rest cut off at the bottom. */
	uint32_t width, height, stride, panel_height;
	unsigned back;
};
extern struct fb fb;

void display_splash_size(uint32_t *w, uint32_t *h);
bool display_init(void);
uint32_t *display_backbuffer(void);
void display_flip(void);
void display_wait_idle(void);
void display_set_idle_hook(void (*fn)(void));
uint32_t display_frames(void);
uint32_t display_waits(void);
uint32_t display_timeouts(void);
void display_set_brightness(uint8_t level);
void display_set_backlight_code(uint8_t code);
void display_sleep(void);
void display_wake(void);
void display_wake_hidden(void);
void display_show(void);
void display_idle(bool on);
bool display_rest(void);
void display_active(void);

int dsi_dcs_write(uint8_t cmd, const uint8_t *params, unsigned n);
/* Commands go out in low power mode while on, as a panel fresh from reset
 * needs them; the bootloader's high speed mode otherwise. */
void dsi_low_power(bool on);

void fbcon_init(void);
void fbcon_draw(uint32_t *buf);
void fbcon_set_overlay(bool on);

void flatmesh_render(uint32_t *buf, uint32_t t_ms);
/* The mesh, and a frame-filling quad in front of it on a square panel. */
#define FLATMESH_GPU_MESH 516
#define FLATMESH_GPU_VERTICES (FLATMESH_GPU_MESH + 6)
#define FLATMESH_GPU_ROUND_VERTICES (FLATMESH_GPU_VERTICES * 4)
unsigned flatmesh_vertices_gpu(float *out, uint32_t t_ms);
void flatmesh_view(float scale, float dx, float dy, float shade);
void flatmesh_colors(uint32_t center, uint32_t outer);
unsigned flatmesh_vertices_gpu_look(float *out, uint32_t t_ms, uint32_t center, uint32_t outer, float dx, float dy,
				    float alpha);
/* The round window's outline around its centre, for a window 1 across. */
#define FLATMESH_ROUND_SIDES 64
const float (*flatmesh_round_edge(void))[2];
/* One DCS write of a panel command sequence, then a pause. */
struct dcs_step {
	uint8_t cmd, n, delay_ms;
	uint8_t data[12];
};
#endif

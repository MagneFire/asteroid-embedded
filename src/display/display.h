#ifndef DISPLAY_H
#define DISPLAY_H
#include <stdint.h>
#include <stdbool.h>

struct fb {
	uint32_t *buf[2];
	uint32_t width, height, stride;
	unsigned back;
};
extern struct fb fb;

bool display_init(void);
uint32_t *display_backbuffer(void);
void display_flip(void);
void display_wait_idle(void);
void display_set_idle_hook(void (*fn)(void));
uint32_t display_frames(void);
uint32_t display_waits(void);
uint32_t display_timeouts(void);
void display_set_brightness(uint8_t level);
void display_sleep(void);
void display_wake(void);

int dsi_dcs_write(uint8_t cmd, const uint8_t *params, unsigned n);

void fbcon_init(void);
void fbcon_draw(uint32_t *buf);
void fbcon_set_overlay(bool on);

void flatmesh_render(uint32_t *buf, uint32_t t_ms);
#define FLATMESH_GPU_VERTICES 516
unsigned flatmesh_vertices_gpu(float *out, uint32_t t_ms);
void flatmesh_view(float scale, float dx, float dy);
#endif

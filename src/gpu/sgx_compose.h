/* The frame put together on the SGX530 (sgx_compose.c): the drawing
 * calls of draw.h and a3xx.h on minnow, each frame on the GPU while it
 * is on and works, otherwise on the CPU (soft.c). */
#ifndef SGX_COMPOSE_H
#define SGX_COMPOSE_H
#include <stdint.h>
#include <stdbool.h>

/* Composites on the GPU from the next frame, starting it, or on the CPU
 * with the GPU off; false if the GPU cannot be used. */
bool sgx_compose(bool on);
bool sgx_compose_on(void);

/* Memory the addresses the drawing calls are given lie in, which the GPU
 * maps: on the watch the frame buffers, the layers and soft_alloc's
 * pool; a host test sets its own. */
struct sgx_region {
	uint32_t addr, size;
};
void sgx_compose_regions(const struct sgx_region *r, unsigned n);
#endif

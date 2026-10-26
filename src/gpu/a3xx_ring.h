/* Adreno 305B command ring, shared by the bring-up code and the drawing
 * code. Type-0 packets write consecutive registers, type-3 packets carry
 * an opcode for the micro engine. GPU memory past the ring and the
 * selftest scratch word is free for buffers and shaders. */
#ifndef A3XX_RING_H
#define A3XX_RING_H
#include <stdint.h>
#include <stdbool.h>
#include "soc/msm8226.h"

#define CP_TYPE0(reg, n)	((((n) - 1) << 16) | ((reg) & 0x7fff))
#define CP_TYPE3(op, n)		(0xc0000000u | (((n) - 1) << 16) | ((op) << 8))
#define CP_NOP			0x10
#define CP_DRAW_INDX		0x22
#define CP_WAIT_FOR_IDLE	0x26
#define CP_LOAD_STATE		0x30
#define CP_INVALIDATE_STATE	0x3b
#define CP_MEM_WRITE		0x3d
#define CP_EVENT_WRITE		0x46
#define CP_ME_INIT		0x48

#define GPU_HEAP_BASE		(GPU_MEM_BASE + 0x20000)

void gpu_ring_emit(uint32_t v);
void gpu_ring_reserve(uint32_t n);
bool gpu_ring_flush(uint32_t timeout_ms);
#endif

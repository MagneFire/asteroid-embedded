/* The compositor on the real draw path (sgx_compose.c over sgx_draw.c),
 * with only sgx.c faked: kicks complete as the recording harness
 * completes them, or never when a case hangs the GPU. Refused frames,
 * however many, must neither use up the frame's vertex room nor turn the
 * GPU off, and a retry after a failure must set everything up again. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "gpu/a3xx.h"
#include "gpu/soft.h"
#include "gpu/sgx.h"
#include "gpu/sgx_image.h"
#include "gpu/sgx_draw.h"
#include "gpu/sgx_compose.h"
#include "gpu/sgx_scene_data.h"
#include "display/display.h"

pid_t fork(void);
void _exit(int status) __attribute__((noreturn));

#define SIZE		SGX_DRAW_SIZE
#define PITCH		(SIZE * 4)
#define NMESH		516
#define POOL		(16u << 20)
#define CMD_SIZE	0x540u

static struct {
	uint32_t frame[SIZE * SIZE] __attribute__((aligned(4096)));
	uint32_t layer[SIZE * SIZE] __attribute__((aligned(4096)));
	float mesh[NMESH * 8];
} mem __attribute__((aligned(4096)));
uintptr_t gpu_host_base;
#define ADDR(p)	((uint32_t)((uintptr_t)(p) - gpu_host_base))

static int failures;
#define CHECK(cond, ...) \
	do { \
		if (!(cond)) { \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
			failures++; \
		} \
	} while (0)

/* --- the firmware around it */

static unsigned nlogs;
void rotation_buffer(uint32_t *buf, uint32_t stride) {}
uint32_t time_ms(void) { return 0; }
uint32_t time_us(void)
{
	static uint32_t t;
	return t += 7;
}
void wdt_feed(void) {}
void sgx_wait_irq(void) {}
void dcache_clean_invalidate_range(const void *addr, uint32_t size) {}
void log_printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	printf("  log: ");
	vprintf(fmt, ap);
	va_end(ap);
	nlogs++;
}

const float (*flatmesh_round_edge(void))[2]
{
	static float edge[FLATMESH_ROUND_SIDES][2];
	for (int k = 0; k < FLATMESH_ROUND_SIDES; k++)
		edge[k][0] = 0.5f, edge[k][1] = 0;
	return edge;
}

/* --- sgx.c, faked */

static struct sgx_mem gmem;
static uint8_t *pool;
static bool running, failed, hang;
static uint32_t generation;
static unsigned kicks, halts;

static bool read_zero(void *ctx, uint32_t off, void *buf, uint32_t len)
{
	memset(buf, 0, len);
	return true;
}

bool sgx_start(void)
{
	if (running)
		return true;
	if (failed || !generation) {
		struct sgx_image img;
		sgx_mem_init(&gmem, pool, 0x8f000000u, POOL);
		if (!sgx_image_build(&gmem, &img, read_zero, NULL))
			return false;
		generation++;
	}
	failed = false;
	return running = true;
}
bool sgx_stop(void)
{
	running = false;
	return true;
}
void sgx_halt(void)
{
	halts++;
	running = false;
	failed = true;
}
bool sgx_running(void) { return running; }
bool sgx_failed(void) { return failed; }
uint32_t sgx_generation(void) { return generation; }
struct sgx_mem *sgx_memory(void) { return generation ? &gmem : NULL; }
void sgx_log_state(void) {}

static uint32_t *P(uint32_t addr) { return sgx_ptr(&gmem, addr); }

/* Where a GPU address lands, through the GPU's page tables. */
static uint32_t phys_of(uint32_t addr)
{
	uint32_t pde = ((uint32_t *)(void *)gmem.base)[addr >> 22];
	if (!(pde & 1))
		return 0;
	uint32_t pte = ((uint32_t *)(void *)(gmem.base + (pde & ~0xfffu) - gmem.phys))[addr >> 12 & 0x3ff];
	return pte & 1 ? (pte & ~0xfffu) + (addr & 0xfff) : 0;
}

/* What the recording harness wrote when a kick completed. */
static void complete(void)
{
	uint32_t *ctl = P(SGX_SCENE_CCB_CTL);
	uint32_t *cmd = P(SGX_SCENE_CCB + (ctl[0] + 0x10000 - CMD_SIZE) % 0x10000), *sh = cmd + 0x64 / 4;
	for (uint32_t i = 0; i < sh[1]; i++)
		*P(sh[0x348 / 4 + 2 * i]) = sh[0x348 / 4 + 2 * i + 1];
	for (uint32_t i = 0; i < sh[2]; i++)
		*P(sh[0x448 / 4 + 2 * i]) = sh[0x448 / 4 + 2 * i + 1];
	for (unsigned i = 0; i < SGX_SCENE_ALLOCS; i++)
		if (sgx_scene_allocs[i].kind == SGX_SCENE_SYNC) {
			uint32_t *s = P(sgx_scene_allocs[i].addr);
			s[1] = s[0];
			s[3] = s[2];
			s[5] = s[4];
		}
	if (cmd[1] & 2) {
		*P(*P(cmd[0x4c / 4] + 0x88)) = 0;
		*P(*P(cmd[0x50 / 4])) = 0;
	}
	ctl[1] = ctl[0];
}

bool sgx_submit_ta(uint32_t context, uint32_t cache)
{
	kicks++;
	if (!hang)
		complete();
	return running;
}

/* --- frames */

static const uint8_t nc[2] = { 4, 4 };

static void fill(void)
{
	for (unsigned i = 0; i < NMESH; i++) {
		float *v = mem.mesh + i * 8;
		v[0] = (float)(i % 17) / 8.5f - 1.0f;
		v[1] = (float)(i % 13) / 6.5f - 1.0f;
		v[2] = 0;
		v[3] = 1;
		v[4] = v[5] = v[6] = 0.5f;
		v[7] = 1;
	}
}

static struct gpu_target target(void) { return (struct gpu_target){ ADDR(mem.frame), SIZE, SIZE, PITCH, false }; }

/* The mesh and a layer over it, as frame.c draws. */
static bool supported(void)
{
	struct gpu_target t = target();
	gpu_draw_begin(&t);
	gpu_draw_turn(1, 0);
	gpu_draw_triangles(ADDR(mem.mesh), 32, 2, nc, NMESH);
	gpu_draw_layer_at(ADDR(mem.layer), SIZE, SIZE, PITCH, 0, 0, 0.5f);
	return gpu_draw_end();
}

/* The same with a layer of a size the GPU's textures do not take. */
static bool refused(void)
{
	struct gpu_target t = target();
	gpu_draw_begin(&t);
	gpu_draw_turn(1, 0);
	gpu_draw_triangles(ADDR(mem.mesh), 32, 2, nc, NMESH);
	gpu_draw_layer_at(ADDR(mem.layer), 64, 64, 256, 20, 30, 0.5f);
	return gpu_draw_end();
}

static void setup(void)
{
	uint32_t size;
	void *spool = soft_pool(&size);
	const struct sgx_region r[] = { { ADDR(mem.frame), sizeof mem.frame }, { ADDR(mem.layer), sizeof mem.layer },
					{ ADDR(spool), size } };
	sgx_compose_regions(r, 3);
	fill();
	CHECK(gpu_init() && running && gpu_ready(), "on at boot");
}

static void refusals(void)
{
	setup();
	CHECK(supported() && kicks == 2, "a GPU frame: %u kicks", kicks);
	CHECK(phys_of(sgx_draw_get_target()) == ADDR(mem.frame), "the target mapped");
	for (int i = 0; i < 300; i++)
		CHECK(refused(), "refused frame %d", i);
	CHECK(kicks == 2 && !halts && gpu_ready() && nlogs == 1, "after the refused frames: %u kicks, %u logs", kicks, nlogs);
	CHECK(supported() && kicks == 4 && !halts, "a GPU frame after them: %u kicks, %u halts", kicks, halts);
	/* Refused by the driver itself, without a kick. */
	*P(SGX_SCENE_DETAILS_ACCESS) = 1;
	for (int i = 0; i < 300; i++)
		CHECK(supported(), "frame %d the driver refuses", i);
	*P(SGX_SCENE_DETAILS_ACCESS) = 0;
	CHECK(kicks == 4 && !halts && gpu_ready() && nlogs == 2, "after the driver's refusals: %u kicks, %u logs", kicks, nlogs);
	CHECK(supported() && kicks == 6 && !halts, "a GPU frame after them: %u kicks", kicks);
}

static void failure(void)
{
	setup();
	CHECK(supported() && supported() && kicks == 4, "two GPU frames");
	hang = true;
	printf("  (a timeout is expected here)\n");
	CHECK(supported() && halts == 1 && !gpu_ready(), "a frame that times out: %u halts", halts);
	hang = false;
	unsigned k = kicks;
	CHECK(supported() && kicks == k, "a CPU frame after it");
	CHECK(gpu_init() && kicks == k && !running, "no retry on wake");
	CHECK(sgx_compose(true) && gpu_ready() && generation == 2, "on again by hand");
	CHECK(supported() && kicks == k + 2 && halts == 1, "a GPU frame after the retry: %u kicks", kicks - k);
	CHECK(*P(SGX_SCENE_CCB_CTL) == 2 * CMD_SIZE && P(SGX_SCENE_CCB)[1] == SGX_SCENE_A_FIRST_EVER,
	      "the scene from scratch: queue %x", *P(SGX_SCENE_CCB_CTL));
	CHECK(phys_of(sgx_draw_get_target()) == ADDR(mem.frame), "the target mapped again: %08x at %08x",
	      sgx_draw_get_target(), phys_of(sgx_draw_get_target()));
	for (int i = 0; i < 40; i++)
		CHECK(supported(), "frame %d after the retry", i);
	CHECK(kicks == k + 82 && halts == 1, "%u kicks", kicks - k);
}

static void run(const char *name, void (*fn)(void))
{
	fflush(stdout);
	pid_t pid = fork();
	if (!pid) {
		failures = 0;
		fn();
		fflush(stdout);
		_exit(failures ? 1 : 0);
	}
	int st;
	waitpid(pid, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) {
		printf("FAIL %s\n", name);
		failures++;
	} else
		printf("%s: ok\n", name);
}

int main(void)
{
	uint32_t psize;
	uintptr_t spool = (uintptr_t)soft_pool(&psize), lo = spool < (uintptr_t)&mem ? spool : (uintptr_t)&mem;
	uintptr_t hi = spool + psize > (uintptr_t)&mem + sizeof mem ? spool + psize : (uintptr_t)&mem + sizeof mem;
	gpu_host_base = (lo & ~(uintptr_t)4095) - 4096;
	if (hi - gpu_host_base > 0xf0000000u) {
		printf("sgx_stack_test: memory too far apart for 32-bit addresses\n");
		return 1;
	}
	pool = aligned_alloc(4096, POOL);
	run("refused frames", refusals);
	run("a failure and the retry", failure);
	if (failures)
		return 1;
	printf("sgx_stack_test: ok\n");
	return 0;
}

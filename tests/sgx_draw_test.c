/* The native SGX triangle path (sgx_draw.c) against the vendor's GLES
 * driver as tools/sgx/glrec.py recorded it: at every kick the GPU memory
 * the native code built must equal the recording's, byte for byte, and
 * where it deliberately does not, each byte is named here with what it
 * must equal instead. The microkernel is replaced by the completion the
 * recording harness applied (pvrkm.py complete_kick), so the memory a
 * second frame starts from is the recording's too. Needs the vendor's
 * libsrv_init.so (SGX_LIBSRV_INIT) and the recordings with their
 * flat.txt (SGX_RECORDINGS, build/sgx/draw; tools/sgx/flatten.py draw);
 * without them it checks the tables and runs frames on its own. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "gpu/sgx.h"
#include "gpu/sgx_image.h"
#include "gpu/sgx_draw.h"
#include "gpu/sgx_scene_data.h"

/* unistd.h would clash with the firmware's reboot(). */
pid_t fork(void);
void _exit(int status) __attribute__((noreturn));

#define POOL		(16u << 20)
#define PHYS		0x8f000000u
#define CMD_SIZE	0x540u
#define SHARED		0x64u

static struct sgx_mem mem;
static uint8_t *pool, *lib;
static long lib_size;
static int failures;
static const char *recordings;

#define CHECK(cond, ...) \
	do { \
		if (!(cond)) { \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
			failures++; \
		} \
	} while (0)

uint32_t time_us(void)
{
	static uint32_t t;
	return t += 7;
}
void wdt_feed(void) {}
void sgx_log_state(void) { printf("  log: microkernel state\n"); }
bool sgx_running(void) { return true; }
struct sgx_mem *sgx_memory(void) { return &mem; }

void log_printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	printf("  log: ");
	vprintf(fmt, ap);
	va_end(ap);
}

static uint32_t *P(uint32_t addr) { return sgx_ptr(&mem, addr); }

static uint8_t *slurp(const char *path, long *size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	rewind(f);
	uint8_t *p = malloc((size_t)n + 1);
	if (fread(p, 1, (size_t)n, f) != (size_t)n)
		exit(2);
	fclose(f);
	if (size)
		*size = n;
	return p;
}

static bool read_lib(void *ctx, uint32_t off, void *buf, uint32_t len)
{
	if (!lib) {
		memset(buf, 0, len);
		return true;
	}
	if (off + len > (uint32_t)lib_size)
		return false;
	memcpy(buf, lib + off, len);
	return true;
}

/* --- the recording harness's completion, after the snapshot of a kick */

static unsigned kick_no;
static void (*on_kick)(unsigned n, const uint32_t cmd[8]);

static void complete(void)
{
	uint32_t *ctl = P(SGX_SCENE_CCB_CTL);
	uint32_t *cmd = P(SGX_SCENE_CCB + (ctl[0] + 0x10000 - CMD_SIZE) % 0x10000), *sh = cmd + SHARED / 4;
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
	uint32_t c[8];
	sgx_command_words(c, SGX_CMD_TA, cache, context);
	if (on_kick)
		on_kick(kick_no, c);
	complete();
	kick_no++;
	return true;
}

static bool fresh(void)
{
	struct sgx_image img;
	if (!pool)
		pool = malloc(POOL);
	memset(pool, 0, POOL);
	sgx_mem_init(&mem, pool, PHYS, POOL);
	return sgx_image_build(&mem, &img, read_lib, NULL);
}

/* Each scene in a process of its own: sgx_draw.c sets up once. */
static void run(const char *name, void (*fn)(void))
{
	fflush(stdout);
	pid_t pid = fork();
	if (!pid) {
		failures = 0;
		if (!fresh()) {
			printf("FAIL %s: no start-up image\n", name);
			_exit(1);
		}
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

/* --- frames */

static const float tri_v[] = {
	-0.8f, -0.8f, 1, 0, 0, 1, 0.8f, -0.8f, 0, 1, 0, 1, 0.0f, 0.8f, 0, 0, 1, 1,
};
static const float moved_v[] = {
	-0.6f, -0.8f, 1, 0, 0, 1, 0.8f, -0.6f, 0, 1, 0, 1, 0.2f, 0.8f, 0, 0, 1, 1,
};
static const float two_v[] = {
	-0.8f, -0.8f, 1, 0, 0, 1, 0.8f, -0.8f, 0, 1, 0, 1, 0.0f, 0.8f, 0, 0, 1, 1,
	-0.8f, 0.8f, 1, 1, 0, 1, -0.4f, 0.8f, 1, 1, 0, 1, -0.8f, 0.4f, 1, 1, 0, 1,
};

static bool frame(const float *v, unsigned tris, uint32_t clear, bool blend)
{
	struct sgx_frame f = { clear, v, tris, blend };
	struct sgx_times t;
	return sgx_draw_frame(&f, &t);
}

/* --- recordings */

struct rec_alloc {
	unsigned index, heap;
	uint32_t size, addr;
	char file[40];
};

struct rec {
	char dir[400];
	struct rec_alloc a[128];
	unsigned n, nk;
	struct { uint32_t off, data[6]; unsigned first, last; } k[8];
	uint32_t hostkick;
};

static bool load(struct rec *r, const char *name)
{
	char path[512], line[256];
	memset(r, 0, sizeof *r);
	snprintf(r->dir, sizeof r->dir, "%s/%s", recordings, name);
	snprintf(path, sizeof path, "%s/flat.txt", r->dir);
	FILE *f = fopen(path, "r");
	if (!f) {
		printf("FAIL no %s (run tools/sgx/glrec.py, then tools/sgx/flatten.py draw)\n", path);
		failures++;
		return false;
	}
	while (fgets(line, sizeof line, f)) {
		struct rec_alloc *a = &r->a[r->n];
		unsigned i, first, last;
		uint32_t off, d[6];
		if (r->n < 128 && sscanf(line, "alloc %u %u %x %x %39s", &a->index, &a->heap, &a->size, &a->addr, a->file) == 5)
			r->n++;
		else if (sscanf(line, "kick %u %x %u %u %x %x %x %x %x %x", &i, &off, &first, &last, &d[0], &d[1], &d[2], &d[3],
				&d[4], &d[5]) == 10 && i < 8) {
			r->k[i].off = off;
			r->k[i].first = first;
			r->k[i].last = last;
			memcpy(r->k[i].data, d, sizeof d);
			r->nk = i + 1;
		} else
			sscanf(line, "hostkick %x", &r->hostkick);
	}
	fclose(f);
	return true;
}

static const struct rec_alloc *rec_at(const struct rec *r, uint32_t addr)
{
	for (unsigned i = 0; i < r->n; i++)
		if (addr >= r->a[i].addr && addr - r->a[i].addr < r->a[i].size)
			return &r->a[i];
	return NULL;
}

/* The bytes of a recorded kick image at addr, cached per file. */
static const uint8_t *rec_bytes(const struct rec *r, unsigned kick, uint32_t addr)
{
	static struct { const struct rec *r; unsigned kick; const struct rec_alloc *a; uint8_t *data; } cache[512];
	static unsigned ncache;
	const struct rec_alloc *a = rec_at(r, addr);
	if (!a)
		return NULL;
	for (unsigned i = 0; i < ncache; i++)
		if (cache[i].r == r && cache[i].kick == kick && cache[i].a == a)
			return cache[i].data + (addr - a->addr);
	char path[600];
	snprintf(path, sizeof path, "%s/kick%02u_pre/%s", r->dir, kick, a->file);
	long n;
	uint8_t *d = slurp(path, &n);
	if (!d || n != (long)a->size || ncache == 512) {
		printf("FAIL unreadable %s\n", path);
		exit(1);
	}
	cache[ncache].r = r;
	cache[ncache].kick = kick;
	cache[ncache].a = a;
	cache[ncache++].data = d;
	return d + (addr - a->addr);
}

static uint32_t rec_word(const struct rec *r, unsigned kick, uint32_t addr)
{
	uint32_t w;
	memcpy(&w, rec_bytes(r, kick, addr), 4);
	return w;
}

/* Bytes a native image is compared against something else than the same
 * address of the same kick: another kick, another address, another
 * recording, or nothing (ref NULL). */
struct sub {
	uint32_t addr, len;
	const struct rec *r;
	unsigned kick;
	uint32_t ref;
	const char *why;
};

#define MAX_SUBS 64
static struct sub subs[MAX_SUBS];
static unsigned nsubs;

static void sub(uint32_t addr, uint32_t len, const struct rec *r, unsigned kick, uint32_t ref, const char *why)
{
	if (nsubs == MAX_SUBS) {
		printf("FAIL more than %d substitutions\n", MAX_SUBS);
		exit(1);
	}
	subs[nsubs++] = (struct sub){ addr, len, r, kick, ref, why };
}

static void compare(const struct rec *r, unsigned kick, const char *what)
{
	for (unsigned i = 0; i < r->n; i++) {
		const struct rec_alloc *a = &r->a[i];
		const uint8_t *got = sgx_ptr(&mem, a->addr), *same = rec_bytes(r, kick, a->addr);
		if (!got) {
			CHECK(0, "%s: allocation #%u at %08x is not mapped", what, a->index, a->addr);
			continue;
		}
		const struct sub *mine[MAX_SUBS];
		unsigned n = 0, bad = 0;
		for (unsigned j = 0; j < nsubs; j++)
			if (subs[j].addr < a->addr + a->size && subs[j].addr + subs[j].len > a->addr)
				mine[n++] = &subs[j];
		if (!n && !memcmp(got, same, a->size))
			continue;
		for (uint32_t o = 0; o < a->size; o++) {
			uint32_t addr = a->addr + o;
			const struct sub *s = NULL;
			for (unsigned j = 0; j < n && !s; j++)
				if (addr - mine[j]->addr < mine[j]->len)
					s = mine[j];
			if (s && !s->r)
				continue;
			uint8_t want = s ? *rec_bytes(s->r, s->kick, s->ref + (addr - s->addr)) : same[o];
			if (got[o] != want && bad++ < 4)
				printf("  %s: #%u %08x: %02x, recorded %02x%s%s\n", what, a->index, addr, got[o], want,
				       s ? " against " : "", s ? s->why : "");
		}
		CHECK(!bad, "%s: allocation #%u (%08x) differs in %u bytes", what, a->index, a->addr, bad);
	}
}

/* --- one recorded scene */

static const struct rec *cur;
static unsigned frames_in_rec;
static void (*subs_for)(unsigned n);

static uint32_t cmd_at(unsigned n) { return SGX_SCENE_CCB + cur->k[n].off - SHARED; }

static void check_kick(unsigned n, const uint32_t c[8])
{
	if (n >= cur->nk)
		return;
	char what[64];
	snprintf(what, sizeof what, "%s kick %u", cur->dir + strlen(recordings) + 1, n);
	uint32_t *cmd = P(cmd_at(n)), *ctl = P(SGX_SCENE_CCB_CTL);
	CHECK(ctl[0] == (cur->k[n].off - SHARED + CMD_SIZE) % 0x10000, "%s: command not at the recorded offset", what);
	CHECK(((cmd[1] & 5) != 0) == cur->k[n].first && ((cmd[1] & 3) >> 1) == cur->k[n].last, "%s: first/last", what);
	CHECK(c[0] == cur->hostkick, "%s: command handler %x, microkernel's TA entry %x", what, c[0], cur->hostkick);
	CHECK(c[1] == (n ? 0 : (SGX_CACHE_INVAL_PT | SGX_CACHE_INVAL_PD)), "%s: cache control %x", what, c[1]);
	CHECK(!memcmp(&c[2], cur->k[n].data, sizeof cur->k[n].data), "%s: command data", what);
	nsubs = 0;
	/* The lab's kernel puts 0x8f100000 >> 12 there; the real one puts
	 * the page directory's address unshifted (sgxutils.c, no 36-bit MMU),
	 * here the pool's. */
	sub(SGX_SCENE_HW_CONTEXT + SGX_SCENE_PD_OFFSET, 4, NULL, 0, 0, "page directory");
	CHECK(*P(SGX_SCENE_HW_CONTEXT + SGX_SCENE_PD_OFFSET) == PHYS, "%s: page directory", what);
	if (subs_for)
		subs_for(n);
	compare(cur, n, what);
}

static void scene(const char *name, const struct sgx_frame *f, unsigned nf, void (*subs_fn)(unsigned), uint32_t target)
{
	static struct rec r;
	if (!load(&r, name))
		return;
	cur = &r;
	frames_in_rec = nf;
	subs_for = subs_fn;
	on_kick = check_kick;
	CHECK(sgx_draw_setup(), "%s: setup", name);
	if (target)
		sgx_draw_target(target);
	for (unsigned i = 0; i < nf; i++) {
		struct sgx_times t;
		CHECK(sgx_draw_frame(&f[i], &t), "%s: frame %u", name, i);
	}
	CHECK(kick_no >= r.nk, "%s: %u kicks recorded, %u made", name, r.nk, kick_no);
}

#define FRAME(v, n, clear, blend) ((struct sgx_frame){ (clear), (v), (n), (blend) })

static void tri(void)
{
	struct sgx_frame f = FRAME(tri_v, 1, 0xff4080bf, false);
	scene("tri", &f, 1, NULL, 0);
}

static void tri_read(void)
{
	struct sgx_frame f = FRAME(tri_v, 1, 0xff4080bf, false);
	scene("tri_read", &f, 1, NULL, 0);
}

static void tri_moved(void)
{
	struct sgx_frame f = FRAME(moved_v, 1, 0xff4080bf, false);
	scene("tri_moved", &f, 1, NULL, 0);
}

static void tri_color(void)
{
	struct sgx_frame f = FRAME(tri_v, 1, 0xffbf4080, false);
	scene("tri_color", &f, 1, NULL, 0);
}

static void tri_blend(void)
{
	struct sgx_frame f = FRAME(tri_v, 1, 0xff4080bf, true);
	scene("tri_blend", &f, 1, NULL, 0);
}

static void tri_two(void)
{
	struct sgx_frame f = FRAME(two_v, 2, 0xff4080bf, false);
	scene("tri_two", &f, 1, NULL, 0);
}

/* The vendor appends every frame to its rings and the native code
 * rewrites frame one's place in them, so where a second frame's bytes
 * depend on where in a ring it is, they must equal the first frame's. */
static const struct { uint32_t addr, size; const char *name; } rings[] = {
	{ 0x00078000, 0x5000, "VDM control stream ring" },
	{ 0x0007d000, 0x32000, "vertex ring" },
	{ 0x000af000, 0x32000, "index ring" },
	{ 0x0e802000, 0xc800, "PDS vertex ring" },
	{ 0x0e810000, 0xc800, "MTE copy ring" },
	{ 0x0dc08000, 0x19000, "PDS pixel ring" },
	{ 0x0f400000, 0x400, "tile-store code ring" },
};

static void second_frame(unsigned n)
{
	if (n < 2)
		return;
	unsigned fa = n & ~1u;
	uint32_t stream = rec_word(cur, fa, cmd_at(fa) + 0x4e4);
	sub(SGX_SCENE_MTE_LAUNCH, 4, cur, fa, stream + 4, "the vendor's frame 2 first MTE launch");
	for (unsigned i = 0; i < sizeof rings / sizeof rings[0]; i++)
		sub(rings[i].addr, rings[i].size, cur, 1, rings[i].addr, rings[i].name);
	for (unsigned k = 2; k <= n; k++) {
		uint32_t at = cmd_at(k), first = cmd_at(k - 2);
		sub(at + 0x010, 4, cur, k - 2, first + 0x010, "frame 1 background PDS offset");
		sub(at + 0x4e4, 4, cur, k - 2, first + 0x4e4, "frame 1 stream offset");
		for (unsigned i = 0; i < SGX_SCENE_TA_STATUS; i++)
			if (i != SGX_SCENE_TA_SEQ)
				sub(at + SHARED + 0x34c + 8 * i, 4, cur, k - 2, first + SHARED + 0x34c + 8 * i, "frame 1 ring status");
		for (unsigned i = 0; i < SGX_SCENE_3D_STATUS; i++)
			if (i != SGX_SCENE_3D_SEQ)
				sub(at + SHARED + 0x44c + 8 * i, 4, cur, k - 2, first + SHARED + 0x44c + 8 * i, "frame 1 ring status");
	}
	/* What the completion of the frame's first kick stored from them. */
	if (n > 2) {
		for (unsigned i = 0; i < SGX_SCENE_TA_STATUS; i++)
			if (i != SGX_SCENE_TA_SEQ)
				sub(sgx_scene_ta_status[0][i].addr, 4, cur, n - 2, sgx_scene_ta_status[0][i].addr, "frame 1 status word");
		for (unsigned i = 0; i < SGX_SCENE_3D_STATUS; i++)
			if (i != SGX_SCENE_3D_SEQ)
				sub(sgx_scene_3d_status[0][i].addr, 4, cur, n - 2, sgx_scene_3d_status[0][i].addr, "frame 1 status word");
	}
	sub(SGX_SCENE_DETAILS + 0x54, 4, cur, 0, SGX_SCENE_DETAILS + 0x54, "frame 1 pixel-event PDS offset");
}

static void tri_read2(void)
{
	struct sgx_frame f[2] = { FRAME(tri_v, 1, 0xff4080bf, false), FRAME(tri_v, 1, 0xff4080bf, false) };
	scene("tri_read2", f, 2, second_frame, 0);
}

static struct rec gap;

static void moved_target(unsigned n)
{
	sub(SGX_SCENE_TILE_STORE0, 8, &gap, n, SGX_SCENE_TILE_STORE0, "tri_read_gap");
	sub(SGX_SCENE_TILE_STORE1, 8, &gap, n, SGX_SCENE_TILE_STORE1, "tri_read_gap");
	sub(SGX_SCENE_BG_TEXTURE, 4, &gap, n, SGX_SCENE_BG_TEXTURE, "tri_read_gap");
}

/* tri_read_gap moved the whole General heap, the render target first,
 * 1 MiB up; of what it changed, only the target's words concern the
 * scene here, which keeps its other buffers where tri_read had them. */
static void retarget(void)
{
	if (!load(&gap, "tri_read_gap"))
		return;
	uint32_t moved = 0;
	for (unsigned i = 0; i < gap.n; i++)
		if (gap.a[i].index == 10)
			moved = gap.a[i].addr;
	CHECK(moved == SGX_SCENE_PBUFFER + 0x100000, "tri_read_gap target at %08x", moved);
	struct sgx_frame f = FRAME(tri_v, 1, 0xff4080bf, false);
	scene("tri_read", &f, 1, moved_target, moved);
	CHECK(sgx_draw_get_target() == moved, "target");
}

/* --- what holds without recordings */

static uint32_t immediate(const uint32_t *ins)
{
	return (ins[0] & 0x1fffff) | ((ins[1] >> 4) & 0x1f) << 21 | ((ins[1] >> 12) & 0x3f) << 26;
}

static void tables(void)
{
	uint32_t words = 0;
	for (unsigned i = 0; i < SGX_SCENE_RUNS; i++) {
		const struct sgx_scene_run *r = &sgx_scene_runs[i];
		if (r->data + r->words > words)
			words = r->data + r->words;
		const struct sgx_scene_alloc *a = NULL;
		for (unsigned j = 0; j < SGX_SCENE_ALLOCS; j++)
			if (r->addr - sgx_scene_allocs[j].addr < sgx_scene_allocs[j].size)
				a = &sgx_scene_allocs[j];
		CHECK(a && r->times && r->addr + (r->times - 1) * r->step + 4 * r->words <= a->addr + a->size,
		      "run %u outside its allocation", i);
	}
	CHECK(words * 4 + SGX_SCENE_RUNS * sizeof(struct sgx_scene_run) < 8192, "scene data of %u words", words);
	for (unsigned i = 0; i < SGX_SCENE_ALLOCS; i++) {
		const struct sgx_scene_alloc *a = &sgx_scene_allocs[i];
		CHECK(a->heap < SGX_HEAPS && !(a->align & (a->align - 1)) && !(a->flags & ~0x4fu) && a->size, "allocation %u", i);
	}
}

static void own_frames(void)
{
	CHECK(sgx_draw_setup(), "setup");
	CHECK(sgx_draw_get_target() == SGX_SCENE_PBUFFER, "first target");
	CHECK(!frame(tri_v, 0, 0, false) && !frame(two_v, SGX_DRAW_MAX_TRIS + 1, 0, false), "triangle count limits");
	CHECK(frame(tri_v, 1, 0xff4080bf, false), "frame 1");
	CHECK(*P(SGX_SCENE_CCB_CTL) == 2 * CMD_SIZE && *P(SGX_SCENE_RT_PENDING) == 2, "kicks of a frame");
	CHECK(P(SGX_SCENE_CCB)[SHARED / 4] & 1 && P(SGX_SCENE_CCB + CMD_SIZE)[SHARED / 4] & 1, "ready");
	CHECK(P(SGX_SCENE_SURFACE_SYNC)[0] == 1 && P(SGX_SCENE_TA3D_SYNC)[0] == 1, "sync counters");
	uint32_t rt1 = P(SGX_SCENE_CCB)[0x58 / 4];
	CHECK(frame(two_v, 2, 0, true), "frame 2");
	CHECK(P(SGX_SCENE_CCB + 2 * CMD_SIZE)[0x58 / 4] != rt1, "render targets alternate");
	/* Enough frames to wrap the TA command ring twice. */
	for (unsigned i = 0; i < 60; i++)
		CHECK(frame(tri_v, 1, i, i & 1), "frame %u", i + 3);
	CHECK(*P(SGX_SCENE_CCB_CTL) == (124 * CMD_SIZE) % 0x10000, "ring offset");
	CHECK(P(SGX_SCENE_SURFACE_SYNC)[0] == 62 && P(SGX_SCENE_SURFACE_SYNC)[1] == 62, "surface sync");
	uint32_t gpu = sgx_draw_map(0x8a000000u, 2 * 409600);
	CHECK(gpu && sgx_draw_map(0x8a000000u + 409600, 409600) == gpu + 409600 && !sgx_draw_map(0x8a000001u, 4096),
	      "mapping");
	sgx_draw_target(gpu + 409600);
	CHECK(immediate(P(SGX_SCENE_TILE_STORE0)) == gpu + 409600 && immediate(P(SGX_SCENE_TILE_STORE1)) == gpu + 409600 &&
	      *P(SGX_SCENE_BG_TEXTURE) == gpu + 409600, "target words");
	CHECK(P(SGX_SCENE_TILE_STORE0)[0] >> 21 == 3, "tile-store register kept");
	struct sgx_frame f = FRAME(tri_v, 1, 0, false);
	struct sgx_times t;
	uint32_t at = *P(SGX_SCENE_CCB_CTL);
	CHECK(sgx_draw_frame(&f, &t), "frame into a mapped buffer");
	(void)at;
}

static void wrong_place(void)
{
	sgx_alloc(&mem, SGX_HEAP_KERNEL_DATA, 4, 4, SGX_MEM_READ | SGX_MEM_WRITE | SGX_MEM_NO_SYNC);
	printf("  (a refusal is expected here)\n");
	CHECK(!sgx_draw_setup(), "setup on a moved heap");
	CHECK(!sgx_draw_setup(), "setup again");
}

int main(void)
{
	const char *path = getenv("SGX_LIBSRV_INIT");
	recordings = getenv("SGX_RECORDINGS") ? getenv("SGX_RECORDINGS") : "build/sgx/draw";
	if (path)
		lib = slurp(path, &lib_size);
	tables();
	run("own frames", own_frames);
	run("refusal", wrong_place);
	char flat[512];
	snprintf(flat, sizeof flat, "%s/tri/manifest.json", recordings);
	FILE *f = fopen(flat, "r");
	if (f)
		fclose(f);
	if (!lib || !f) {
		printf("sgx_draw_test: %s, recordings not compared\n", !lib ? "no vendor library" : "no recordings");
	} else {
		CHECK(sgx_crc32(0, lib + SGX_FILE_FIRST, SGX_FILE_END - SGX_FILE_FIRST) == SGX_FILE_CRC, "library");
		run("tri", tri);
		run("tri_read", tri_read);
		run("tri_moved", tri_moved);
		run("tri_color", tri_color);
		run("tri_blend", tri_blend);
		run("tri_two", tri_two);
		run("tri_read2", tri_read2);
		run("tri_read_gap", retarget);
	}
	if (failures)
		return 1;
	printf("sgx_draw_test: ok\n");
	return 0;
}

/* The native SGX path (sgx_draw.c) against the vendor's GLES driver as
 * tools/sgx/glrec.py recorded it: at every kick the GPU memory the
 * native code built must equal the recording's, byte for byte, and
 * where it deliberately does not, each byte is named here with what it
 * must equal instead. The microkernel is replaced by the completion the
 * recording harness applied (pvrkm.py complete_kick), so the memory a
 * second frame starts from is the recording's too. Needs the vendor's
 * libsrv_init.so (SGX_LIBSRV_INIT) and the recordings with their
 * flat.txt (SGX_RECORDINGS, build/sgx/draw; tools/sgx/flatten.py draw);
 * without them it checks the tables and runs frames on its own.
 *
 * The scene is the mix recordings' GL context, so the tri scenes are the
 * ones recorded with the transfer context (*_tq), which have its
 * addresses. Allocations of the transfer context, and textures, are the
 * caller's or nobody's and are not compared. */
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
static bool running = true, hang;
static uint32_t generation = 1, caches[8];

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
void sgx_wait_irq(void) {}
void sgx_log_state(void) { printf("  log: microkernel state\n"); }
bool sgx_running(void) { return running; }
uint32_t sgx_generation(void) { return generation; }
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
	if (!hang)
		complete();
	caches[kick_no++ % 8] = cache;
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

/* --- recordings */

struct rec_alloc {
	unsigned index, heap;
	uint32_t size, addr;
	char file[40];
	bool tq;
};

struct rec {
	char name[64], dir[400];
	struct rec_alloc a[160];
	unsigned n, nk;
	struct { uint32_t off, data[6]; unsigned first, last; } k[8];
	uint32_t hostkick;
};

static bool load(struct rec *r, const char *name)
{
	char path[512], line[256], tq[8];
	memset(r, 0, sizeof *r);
	snprintf(r->name, sizeof r->name, "%s", name);
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
		int got = r->n < 160 ? sscanf(line, "alloc %u %u %x %x %39s %7s", &a->index, &a->heap, &a->size, &a->addr,
					      a->file, tq) : 0;
		if (got >= 5) {
			a->tq = got == 6 && !strcmp(tq, "tq");
			r->n++;
		} else if (sscanf(line, "kick %u %x %u %u %x %x %x %x %x %x", &i, &off, &first, &last, &d[0], &d[1], &d[2],
				  &d[3], &d[4], &d[5]) == 10 && i < 8) {
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
	static struct { const struct rec *r; unsigned kick; const struct rec_alloc *a; uint8_t *data; } cache[1024];
	static unsigned ncache;
	const struct rec_alloc *a = rec_at(r, addr);
	if (!a) {
		printf("FAIL %s: nothing recorded at %08x\n", r->name, addr);
		exit(1);
	}
	for (unsigned i = 0; i < ncache; i++)
		if (cache[i].r == r && cache[i].kick == kick && cache[i].a == a)
			return cache[i].data + (addr - a->addr);
	char path[600];
	snprintf(path, sizeof path, "%s/kick%02u_pre/%s", r->dir, kick, a->file);
	long n;
	uint8_t *d = slurp(path, &n);
	if (!d || n != (long)a->size || ncache == 1024) {
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

/* Bytes compared against something else than the same address of the
 * same kick: another kick, address or recording (ref), or with the
 * native bytes taken from elsewhere (native, for what the native scene
 * keeps at another place), or nothing at all (r and native 0). */
struct sub {
	uint32_t addr, len;
	const struct rec *r;
	unsigned kick;
	uint32_t ref, native;
	const char *why;
};

#define MAX_SUBS 128
static struct sub subs[MAX_SUBS];
static unsigned nsubs;

static void sub(uint32_t addr, uint32_t len, const struct rec *r, unsigned kick, uint32_t ref, const char *why)
{
	if (nsubs == MAX_SUBS) {
		printf("FAIL more than %d substitutions\n", MAX_SUBS);
		exit(1);
	}
	subs[nsubs++] = (struct sub){ addr, len, r, kick, ref, 0, why };
}

static void moved(uint32_t addr, uint32_t len, uint32_t native, const char *why)
{
	sub(addr, len, NULL, 0, 0, why);
	subs[nsubs - 1].native = native;
}

/* Whether the native scene has the recorded allocation: inside one of
 * its own, not in a hole. */
static bool native_has(const struct rec_alloc *a)
{
	for (unsigned i = 0; i < SGX_SCENE_ALLOCS; i++) {
		const struct sgx_scene_alloc *s = &sgx_scene_allocs[i];
		if (s->kind != SGX_SCENE_HOLE && a->addr >= s->addr && a->addr + a->size <= s->addr + s->size)
			return true;
	}
	return false;
}

static unsigned compared;

static void compare(const struct rec *r, unsigned kick, const char *what)
{
	for (unsigned i = 0; i < r->n; i++) {
		const struct rec_alloc *a = &r->a[i];
		if (a->tq || !native_has(a))
			continue;
		const uint8_t *got = sgx_ptr(&mem, a->addr), *same = rec_bytes(r, kick, a->addr);
		const struct sub *mine[MAX_SUBS];
		unsigned n = 0, bad = 0;
		for (unsigned j = 0; j < nsubs; j++)
			if (subs[j].addr < a->addr + a->size && subs[j].addr + subs[j].len > a->addr)
				mine[n++] = &subs[j];
		compared += a->size;
		if (!n && !memcmp(got, same, a->size))
			continue;
		for (uint32_t o = 0; o < a->size; o++) {
			uint32_t addr = a->addr + o;
			const struct sub *s = NULL;
			for (unsigned j = 0; j < n && !s; j++)
				if (addr - mine[j]->addr < mine[j]->len)
					s = mine[j];
			if (s && !s->r && !s->native)
				continue;
			uint8_t g = s && s->native ? *(uint8_t *)sgx_ptr(&mem, s->native + (addr - s->addr)) : got[o];
			uint8_t want = s && s->r ? *rec_bytes(s->r, s->kick, s->ref + (addr - s->addr)) : same[o];
			if (g != want && bad++ < 6)
				printf("  %s: #%u %08x: %02x, recorded %02x%s%s\n", what, a->index, addr, g, want,
				       s ? " against " : "", s ? s->why : "");
		}
		CHECK(!bad, "%s: allocation #%u (%08x) differs in %u bytes", what, a->index, a->addr, bad);
	}
}

/* --- one recorded scene */

static const struct rec *cur;
static struct rec mixref, meshref, notq;
static void (*subs_for)(unsigned n);

static uint32_t cmd_at(unsigned n) { return SGX_SCENE_CCB + cur->k[n].off - SHARED; }

static void check_kick(unsigned n, const uint32_t c[8])
{
	if (n >= cur->nk)
		return;
	char what[96];
	snprintf(what, sizeof what, "%s kick %u", cur->name, n);
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
	/* With the transfer context the last kick waits for its 3D work, the
	 * texture uploads (h3DSyncInfo: read and write pending values,
	 * complete addresses); the native path has no transfer context, so
	 * these words are as in the recording made without one. */
	for (unsigned k = 0; k <= n; k++)
		sub(cmd_at(k) + SHARED + 0x18, 0x14, &notq, n, cmd_at(k) + SHARED + 0x18,
		    "tri_read2 without the transfer context");
	if (subs_for)
		subs_for(n);
	compare(cur, n, what);
}

/* A draw as the test gives it: vertices at a recorded address. */
struct tdraw {
	uint32_t verts;
	uint16_t count;
	uint32_t tex;
	uint8_t alpha;
	bool blend;
};
#define FLAT(v, n)		{ (v), (n), 0, 0, false }
#define FLAT_BLEND(v, n)	{ (v), (n), 0, 0, true }
#define TEX(v, n, t, a)		{ (v), (n), (t), (a), true }

static struct sgx_frame frame_of(const struct tdraw *td, unsigned n, uint32_t clear)
{
	static struct sgx_draw d[SGX_DRAW_MAX_DRAWS];
	for (unsigned i = 0; i < n; i++)
		d[i] = (struct sgx_draw){ .verts = td[i].verts, .count = td[i].count, .kind = td[i].tex ? SGX_DRAW_TEXTURED : 0,
					  .blend = td[i].blend, .tex = td[i].tex, .width = 320, .height = 320,
					  .alpha = td[i].alpha, .tiled = true };
	return (struct sgx_frame){ clear, d, n };
}

/* The application's vertices, which the vendor's driver never wrote:
 * copied from the recording into the same place. */
static void vertices(const struct rec *r, const struct tdraw *td, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		const struct rec_alloc *a = rec_at(r, td[i].verts);
		memcpy(P(a->addr), rec_bytes(r, 0, a->addr), a->size);
	}
}

static void (*between)(unsigned frame);

static void scene(const char *name, const struct tdraw *td, unsigned n, uint32_t clear, unsigned frames,
		  void (*subs_fn)(unsigned), uint32_t target)
{
	static struct rec r;
	if (!load(&r, name) || !load(&notq, "tri_read2"))
		return;
	cur = &r;
	subs_for = subs_fn;
	on_kick = check_kick;
	CHECK(sgx_draw_setup(), "%s: setup", name);
	if (target)
		sgx_draw_target(target);
	vertices(&r, td, n);
	for (unsigned i = 0; i < frames; i++) {
		struct sgx_frame f = frame_of(td, n, clear);
		struct sgx_times t;
		if (between)
			between(i);
		CHECK(sgx_draw_frame(&f, &t), "%s: frame %u: %s", name, i, sgx_draw_failure());
	}
	CHECK(kick_no >= r.nk, "%s: %u kicks recorded, %u made", name, r.nk, kick_no);
}

#define CLEAR		0xff4080bf
#define MESH_VBO	0x00121000
#define QUAD_VBO	0x00125000
#define TA		0x00127000
#define TB		0x0018b000
#define TC		0x001ef000
#define Q(first)	(QUAD_VBO + 16 * (first))

/* Where the first draw's index list has the table's address, and the
 * flat draws' vertex-secondary constants. */
#define FIRST_INDEX_ADDR	(SGX_SCENE_VDM + SGX_SCENE_VDM_PREFIX + 16 + 4)
#define FIRST_FLAT_CONSTS	(SGX_SCENE_PV + SGX_SCENE_PV_PREFIX + 0x60)

/* Scenes that only compiled the flat program have no textured code,
 * and their identity index table lies behind their own vertex buffer:
 * compared with mix's, and the native table with theirs. */
static uint32_t rec_table;

static void flat_only(unsigned n)
{
	sub(SGX_SCENE_TEX_VS_CODE, 4 * SGX_SCENE_TEX_VS_WORDS, &mixref, 0, SGX_SCENE_TEX_VS_CODE, "mix's textured code");
	sub(SGX_SCENE_TEX_FS_CODE, 4 * SGX_SCENE_TEX_FS_WORDS, &mixref, 0, SGX_SCENE_TEX_FS_CODE, "mix's textured code");
	moved(rec_table, 2048, SGX_SCENE_INDICES, "the native index table");
	sub(FIRST_INDEX_ADDR, 4, &mixref, 0, FIRST_INDEX_ADDR, "mix's index table address");
}

static uint32_t table_of(const char *name)
{
	struct rec r;
	if (!load(&r, name))
		return 0;
	for (unsigned i = 0; i < r.n; i++)
		if (r.a[i].heap == 0 && r.a[i].size == 2048 && r.a[i].addr > MESH_VBO)
			return r.a[i].addr;
	return 0;
}

static void tri_scene(const char *name, const struct tdraw *td, unsigned n, uint32_t clear, unsigned frames,
		      void (*more)(unsigned))
{
	load(&mixref, "mix");
	rec_table = table_of(name);
	CHECK(rec_table, "%s: no index table", name);
	scene(name, td, n, clear, frames, more, 0);
}

static const struct tdraw tri_d[] = { FLAT(MESH_VBO, 3) };
static const struct tdraw two_d[] = { FLAT(MESH_VBO, 6) };
static const struct tdraw blend_d[] = { FLAT_BLEND(MESH_VBO, 3) };

static void tri_tq(void) { tri_scene("tri_tq", tri_d, 1, CLEAR, 1, flat_only); }
static void tri_read(void) { tri_scene("tri_read_tq", tri_d, 1, CLEAR, 1, flat_only); }
static void tri_moved(void) { tri_scene("tri_moved_tq", tri_d, 1, CLEAR, 1, flat_only); }
static void tri_color(void) { tri_scene("tri_color_tq", tri_d, 1, 0xffbf4080, 1, flat_only); }
static void tri_blend(void) { tri_scene("tri_blend_tq", blend_d, 1, CLEAR, 1, flat_only); }
static void tri_two(void) { tri_scene("tri_two_tq", two_d, 1, CLEAR, 1, flat_only); }

/* The vendor appends every frame to its rings and the native code
 * rewrites frame one's place in them, so where a second frame's bytes
 * depend on where in a ring it is, they must equal the first frame's. */
static const struct { uint32_t addr, size; const char *name; } rings[] = {
	{ SGX_SCENE_VDM, SGX_SCENE_VDM_SIZE, "VDM control stream ring" },
	{ SGX_SCENE_CLEAR & ~0xfffu, 0x32000, "vertex ring" },
	{ 0x000ef000, 0x32000, "index ring" },
	{ SGX_SCENE_PV, SGX_SCENE_PV_SIZE, "PDS vertex ring" },
	{ SGX_SCENE_MT, SGX_SCENE_MT_SIZE, "MTE copy ring" },
	{ SGX_SCENE_PP, SGX_SCENE_PP_SIZE, "PDS pixel ring" },
	{ SGX_SCENE_TILE_STORE0 & ~0xfffu, 0x400, "tile-store code ring" },
};

static void second_frame(unsigned n)
{
	if (n < 2)
		return;
	unsigned fa = n & ~1u;
	uint32_t stream = rec_word(cur, fa, cmd_at(fa) + 0x4e4);
	sub(SGX_SCENE_MTE_LAUNCH, 4, cur, fa, stream + 4, "the vendor's frame 2 first MTE launch");
	for (unsigned i = 0; i < sizeof rings / sizeof rings[0]; i++)
		if (rings[i].addr != SGX_SCENE_VDM)
			sub(rings[i].addr, rings[i].size, cur, 1, rings[i].addr, rings[i].name);
		else {
			sub(rings[i].addr, 4, cur, 1, rings[i].addr, rings[i].name);
			sub(rings[i].addr + 8, rings[i].size - 8, cur, 1, rings[i].addr + 8, rings[i].name);
		}
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

static void tri_frame2(unsigned n)
{
	flat_only(n);
	second_frame(n);
}

static void tri_read2(void) { tri_scene("tri_read2_tq", tri_d, 1, CLEAR, 2, tri_frame2); }

static struct rec gap;

static void moved_target(unsigned n)
{
	flat_only(n);
	sub(SGX_SCENE_TILE_STORE0, 8, &gap, n, SGX_SCENE_TILE_STORE0, "tri_read_tq_gap");
	sub(SGX_SCENE_TILE_STORE1, 8, &gap, n, SGX_SCENE_TILE_STORE1, "tri_read_tq_gap");
	sub(SGX_SCENE_BG_TEXTURE, 4, &gap, n, SGX_SCENE_BG_TEXTURE, "tri_read_tq_gap");
}

/* tri_read_tq_gap moved the whole General heap, the render target with
 * it, 1 MiB up; of what it changed, only the target's words concern the
 * scene here, which keeps its other buffers where tri_read_tq had them. */
static void retarget(void)
{
	if (!load(&gap, "tri_read_tq_gap"))
		return;
	uint32_t moved_to = 0;
	for (unsigned i = 0; i < gap.n; i++)
		if (gap.a[i].index == 77)
			moved_to = gap.a[i].addr;
	CHECK(moved_to == SGX_SCENE_PBUFFER + 0x100000, "tri_read_tq_gap target at %08x", moved_to);
	load(&mixref, "mix");
	rec_table = table_of("tri_read_tq");
	scene("tri_read_tq", tri_d, 1, CLEAR, 1, moved_target, moved_to);
	CHECK(sgx_draw_get_target() == moved_to, "target");
}

/* --- the mesh and the textured draws */

/* mix's flat draws hold stale driver memory in five words of their
 * vertex-secondary constants (a CPU address of the lab and four small
 * numbers); mesh, the same draw, has the clean words, which the native
 * code writes. */
static unsigned flat_consts[4], nflat;

static void clean_consts(unsigned n)
{
	static const uint8_t stale[] = { 1, 6, 7, 8, 9 };
	for (unsigned i = 0; i < nflat; i++)
		for (unsigned k = 0; k < sizeof stale; k++)
			sub(flat_consts[i] + 4 * stale[k], 4, &meshref, 0, FIRST_FLAT_CONSTS + 4 * stale[k], "mesh's clean constants");
}

static void mesh_subs(unsigned n)
{
	flat_only(n);
}

static void mix_subs(unsigned n)
{
	clean_consts(n);
}

static void mix2_subs(unsigned n)
{
	clean_consts(n);
	second_frame(n);
}

static const struct tdraw mesh_d[] = { FLAT(MESH_VBO, 516) };
static const struct tdraw mix_d[] = { FLAT(MESH_VBO, 516), TEX(Q(0), 6, TA, 0xff), TEX(Q(6), 6, TB, 0x80),
				      TEX(Q(18), 192, TA, 0xbf) };
static const struct tdraw minus_d[] = { FLAT(MESH_VBO, 516), TEX(Q(0), 6, TA, 0xff), TEX(Q(18), 192, TA, 0xbf) };
static const struct tdraw plus_d[] = { FLAT(MESH_VBO, 516), TEX(Q(0), 6, TA, 0xff), TEX(Q(6), 6, TB, 0x80),
				       TEX(Q(12), 6, TB, 0x80), TEX(Q(18), 192, TA, 0xbf) };
static const struct tdraw tex3_d[] = { FLAT(MESH_VBO, 516), TEX(Q(0), 6, TA, 0xff), TEX(Q(6), 6, TB, 0x80),
				       TEX(Q(12), 6, TC, 0x80), TEX(Q(18), 192, TA, 0xbf) };
/* Textures get their memory in the order they are first drawn. */
static const struct tdraw swap_d[] = { FLAT(MESH_VBO, 516), TEX(Q(6), 6, TA, 0x80), TEX(Q(0), 6, TB, 0xff),
				       TEX(Q(18), 192, TB, 0xbf) };
static const struct tdraw alpha_d[] = { FLAT(MESH_VBO, 516), TEX(Q(0), 6, TA, 0xff), TEX(Q(6), 6, TB, 0x40),
					TEX(Q(18), 192, TA, 0xbf) };
static const struct tdraw back_d[] = { FLAT(MESH_VBO, 516), TEX(Q(0), 6, TA, 0xff), FLAT(MESH_VBO, 180),
				       TEX(Q(6), 6, TB, 0x80), TEX(Q(18), 192, TA, 0xbf) };

static void mix_scene(const char *name, const struct tdraw *td, unsigned n, unsigned frames, void (*more)(unsigned))
{
	load(&meshref, "mesh");
	load(&mixref, "mix");
	nflat = 0;
	/* Where each flat draw's constants land: the first after its state
	 * block, a later one after its vertex program and block. */
	uint32_t pv = SGX_SCENE_PV + SGX_SCENE_PV_PREFIX;
	for (unsigned i = 0; i < n; i++) {
		if (!td[i].tex && !i) {
			flat_consts[nflat++] = pv + 0x60;
			pv += 0x60 + 0x80;
		} else if (!td[i].tex) {
			flat_consts[nflat++] = pv + 0x60 + 0x40;
			pv += 0x60 + 0x40 + 0x80;
		} else {
			bool after_flat = !td[i - 1].tex;
			pv += 0x60 + (after_flat ? 0x40 : 0x20);
			if (after_flat || td[i - 1].tex != td[i].tex)
				pv += 0x80;
		}
	}
	scene(name, td, n, CLEAR, frames, more, 0);
}

static void mesh(void)
{
	load(&mixref, "mix");
	rec_table = table_of("mesh");
	scene("mesh", mesh_d, 1, CLEAR, 1, mesh_subs, 0);
}

static void mix(void) { mix_scene("mix", mix_d, 4, 1, mix_subs); }
static void mix_minus(void) { mix_scene("mix_minus", minus_d, 3, 1, mix_subs); }
static void mix_plus(void) { mix_scene("mix_plus", plus_d, 5, 1, mix_subs); }
static void mix_tex(void) { mix_scene("mix_tex", mix_d, 4, 1, mix_subs); }
static void mix_tex3(void) { mix_scene("mix_tex3", tex3_d, 5, 1, mix_subs); }
static void mix_swap(void) { mix_scene("mix_swap", swap_d, 4, 1, mix_subs); }
static void mix_alpha(void) { mix_scene("mix_alpha", alpha_d, 4, 1, mix_subs); }
static void mix_back(void) { mix_scene("mix_back", back_d, 5, 1, mix_subs); }
static void mix2(void) { mix_scene("mix2", mix_d, 4, 2, mix2_subs); }

/* The GPU goes off between the frames, as when the watch sleeps: a
 * frame asked for meanwhile is refused without touching anything, and
 * the next frame after the start goes on from the scene in memory as if
 * nothing had happened. */
static void power_cycle(unsigned frame)
{
	if (frame != 1)
		return;
	running = false;
	uint32_t ctl = *P(SGX_SCENE_CCB_CTL), kicks = kick_no, details = *P(SGX_SCENE_DETAILS + 0x18);
	struct sgx_frame f = frame_of(mix_d, 4, CLEAR);
	struct sgx_times t;
	CHECK(!sgx_draw_frame(&f, &t), "a frame while the GPU is off");
	CHECK(*P(SGX_SCENE_CCB_CTL) == ctl && kick_no == kicks && *P(SGX_SCENE_DETAILS + 0x18) == details,
	      "the refused frame touched the scene");
	running = true;
}

static void mix2_power(void)
{
	between = power_cycle;
	mix_scene("mix2", mix_d, 4, 2, mix2_subs);
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
		CHECK(a && a->kind != SGX_SCENE_HOLE && r->times &&
		      r->addr + (r->times - 1) * r->step + 4 * r->words <= a->addr + a->size, "run %u outside its allocation", i);
	}
	CHECK(words * 4 + SGX_SCENE_RUNS * sizeof(struct sgx_scene_run) < 8192, "scene data of %u words", words);
	for (unsigned i = 0; i < SGX_SCENE_ALLOCS; i++) {
		const struct sgx_scene_alloc *a = &sgx_scene_allocs[i];
		CHECK(a->heap < SGX_HEAPS && !(a->align & (a->align - 1)) && !(a->flags & ~0x4fu) && a->size, "allocation %u", i);
	}
	CHECK(SGX_DRAW_MAX_VERTS <= SGX_SCENE_INDEX_TABLE, "vertices a draw may have");
	/* The most a frame can put into each ring. */
	CHECK(SGX_SCENE_VDM_PREFIX + SGX_DRAW_MAX_DRAWS * 40 + 2 * 28 <= SGX_SCENE_VDM_SIZE &&
	      SGX_SCENE_PV_PREFIX + SGX_DRAW_MAX_DRAWS * 0x140 <= SGX_SCENE_PV_SIZE &&
	      SGX_SCENE_MT_PREFIX + SGX_DRAW_MAX_DRAWS * 0x80 <= SGX_SCENE_MT_SIZE &&
	      SGX_SCENE_PP_PREFIX + SGX_DRAW_MAX_DRAWS * 0x80 <= SGX_SCENE_PP_SIZE &&
	      SGX_SCENE_PC_PREFIX + SGX_DRAW_MAX_DRAWS * 0x40 <= SGX_SCENE_PC_SIZE, "ring sizes");
}

static bool frame(const struct tdraw *td, unsigned n, uint32_t clear)
{
	struct sgx_frame f = frame_of(td, n, clear);
	struct sgx_times t;
	return sgx_draw_frame(&f, &t);
}

static void own_frames(void)
{
	CHECK(sgx_draw_setup(), "setup");
	CHECK(sgx_draw_get_target() == SGX_SCENE_PBUFFER, "first target");
	uint32_t vgpu;
	float *v = sgx_draw_vertices(2048 * 24, &vgpu);
	CHECK(v && vgpu == SGX_SCENE_VERTICES, "vertex room");
	for (unsigned i = 0; i < 2048 * 6; i++)
		v[i] = (float)(i % 7) * 0.1f;
	const struct tdraw first_tex[] = { TEX(vgpu, 6, TA, 255) };
	const struct tdraw flat_flat[] = { FLAT(vgpu, 3), FLAT(vgpu, 3) };
	const struct tdraw two_blends[] = { FLAT_BLEND(vgpu, 3), TEX(vgpu, 6, TA, 1), FLAT(vgpu, 3) };
	const struct tdraw odd[] = { FLAT(vgpu, 4) };
	const struct tdraw many[] = { FLAT(vgpu, SGX_DRAW_MAX_VERTS + 3) };
	CHECK(!frame(first_tex, 1, 0) && !frame(flat_flat, 2, 0) && !frame(two_blends, 3, 0) && !frame(odd, 1, 0) &&
	      !frame(many, 1, 0) && !frame(tri_d, 0, 0) && sgx_draw_refused() && !kick_no, "refusals");
	struct sgx_draw bad = { .verts = vgpu, .count = 6, .kind = SGX_DRAW_TEXTURED, .blend = true, .tex = TA,
				.width = 320, .height = 320, .alpha = 9, .tiled = true };
	struct sgx_draw fl = { .verts = vgpu, .count = 3 };
	struct sgx_draw pair[2] = { fl, bad };
	struct sgx_frame f = { 0, pair, 2 };
	struct sgx_times t;
	pair[1].blend = false;
	CHECK(!sgx_draw_frame(&f, &t), "a textured draw without blending");
	pair[1] = bad;
	pair[1].width = 256;
	CHECK(!sgx_draw_frame(&f, &t), "a power-of-two texture");
	pair[1] = bad;
	pair[1].tex += 64;
	CHECK(!sgx_draw_frame(&f, &t), "a texture off a page");
	pair[1] = bad;
	pair[1].tiled = false;
	pair[1].linear = true;
	CHECK(!sgx_draw_frame(&f, &t), "a linear texture of rows");
	pair[1] = bad;
	pair[1].linear = true;
	CHECK(sgx_draw_frame(&f, &t), "a linear texture");
	CHECK(P(SGX_SCENE_PC + SGX_SCENE_PC_PREFIX)[2] == sgx_scene_sampling[1], "linear sampling word");
	CHECK(kick_no == 2 && *P(SGX_SCENE_CCB_CTL) == 2 * CMD_SIZE && *P(SGX_SCENE_RT_PENDING) == 2, "kicks of a frame");
	CHECK(P(SGX_SCENE_CCB)[SHARED / 4] & 1 && P(SGX_SCENE_CCB + CMD_SIZE)[SHARED / 4] & 1, "ready");
	CHECK(P(SGX_SCENE_SURFACE_SYNC)[0] == 1 && P(SGX_SCENE_TA3D_SYNC)[0] == 1, "sync counters");
	uint32_t rt1 = P(SGX_SCENE_CCB)[0x58 / 4];
	/* The most draws a frame may have, each the biggest kind. */
	struct sgx_draw most[SGX_DRAW_MAX_DRAWS];
	most[0] = fl;
	for (unsigned i = 1; i < SGX_DRAW_MAX_DRAWS; i++) {
		most[i] = i % 2 ? bad : fl;
		most[i].count = SGX_DRAW_MAX_VERTS - 2;
		most[i].tex += 0x1000 * i;
	}
	f = (struct sgx_frame){ 0xff000000, most, SGX_DRAW_MAX_DRAWS };
	CHECK(sgx_draw_frame(&f, &t), "frame of %u draws", SGX_DRAW_MAX_DRAWS);
	CHECK(P(SGX_SCENE_CCB + 2 * CMD_SIZE)[0x58 / 4] != rt1, "render targets alternate");
	/* Enough frames to wrap the TA command ring twice. */
	for (unsigned i = 0; i < 60; i++)
		CHECK(frame(i & 1 ? mix_d : tri_d, i & 1 ? 4 : 1, i), "frame %u", i + 3);
	CHECK(*P(SGX_SCENE_CCB_CTL) == (124 * CMD_SIZE) % 0x10000, "ring offset");
	CHECK(P(SGX_SCENE_SURFACE_SYNC)[0] == 62 && P(SGX_SCENE_SURFACE_SYNC)[1] == 62, "surface sync");
	CHECK(!sgx_draw_vertices(SGX_SCENE_VERTICES_SIZE + 1, &vgpu) && sgx_draw_vertices(16, &vgpu) &&
	      vgpu == SGX_SCENE_VERTICES, "vertex room starts again after a frame");
	void *cpu;
	uint32_t aux = sgx_draw_alloc(409600, &cpu);
	CHECK(aux && cpu == P(aux) && aux >= 0x00127000, "memory of the GPU's own");
	uint32_t gpu = sgx_draw_map(0x8a000000u, 2 * 409600);
	CHECK(gpu && sgx_draw_map(0x8a000000u + 409600, 409600) == gpu + 409600 && !sgx_draw_map(0x8a000001u, 4096),
	      "mapping");
	sgx_draw_target(gpu + 409600);
	CHECK(immediate(P(SGX_SCENE_TILE_STORE0)) == gpu + 409600 && immediate(P(SGX_SCENE_TILE_STORE1)) == gpu + 409600 &&
	      *P(SGX_SCENE_BG_TEXTURE) == gpu + 409600, "target words");
	CHECK(P(SGX_SCENE_TILE_STORE0)[0] >> 21 == 3, "tile-store register kept");
	CHECK(frame(tri_d, 1, 0), "frame into a mapped buffer");
	CHECK(caches[(kick_no - 2) % 8] == (SGX_CACHE_INVAL_PT | SGX_CACHE_INVAL_PD) && !caches[(kick_no - 1) % 8],
	      "page tables invalidated after a mapping, once");
	sgx_draw_inval_data(true);
	CHECK(frame(tri_d, 1, 0) && caches[(kick_no - 2) % 8] == SGX_CACHE_INVAL_DATA && !caches[(kick_no - 1) % 8],
	      "data caches invalidated by the first kick: %x %x", caches[(kick_no - 2) % 8], caches[(kick_no - 1) % 8]);
	sgx_draw_inval_data(false);
	CHECK(frame(tri_d, 1, 0) && !caches[(kick_no - 2) % 8], "data caches no longer invalidated");
	pair[1] = bad;
	pair[1].tiled = false;
	f = (struct sgx_frame){ 0, pair, 2 };
	CHECK(sgx_draw_frame(&f, &t), "a texture of rows");
	CHECK(P(SGX_SCENE_PC + SGX_SCENE_PC_PREFIX)[2] == 0x80020138 &&
	      P(SGX_SCENE_PC + SGX_SCENE_PC_PREFIX)[3] == 0xed13f13f, "texture of rows state");
}

/* A frame that times out leaves the scene stuck, and every frame after
 * it is refused; once the GPU was built again the scene starts over. */
static void rebuilt(void)
{
	CHECK(sgx_draw_setup(), "setup");
	uint32_t vgpu, gpu = sgx_draw_map(0x8a000000u, 0x100000);
	float *v = sgx_draw_vertices(3 * 24, &vgpu);
	memset(v, 0, 3 * 24);
	const struct tdraw tri[] = { FLAT(vgpu, 3) };
	CHECK(gpu && frame(tri, 1, 0) && frame(tri, 1, 0), "two frames");
	hang = true;
	printf("  (a timeout is expected here)\n");
	CHECK(!frame(tri, 1, 0) && !sgx_draw_refused(), "a frame that does not complete");
	hang = false;
	unsigned k = kick_no;
	CHECK(!frame(tri, 1, 0) && sgx_draw_refused() && kick_no == k, "a frame after it");
	CHECK(!sgx_draw_setup() || !frame(tri, 1, 0), "setup again before the rebuild");
	if (!fresh())
		CHECK(0, "image");
	generation++;
	CHECK(sgx_draw_setup() && sgx_draw_get_target() == SGX_SCENE_PBUFFER, "setup after the rebuild");
	CHECK(sgx_draw_map(0x8a000000u, 0x100000) == gpu, "the same mapping again");
	v = sgx_draw_vertices(3 * 24, &vgpu);
	memset(v, 0, 3 * 24);
	const struct tdraw tri2[] = { FLAT(vgpu, 3) };
	k = kick_no;
	CHECK(frame(tri2, 1, 0) && kick_no == k + 2, "a frame after the rebuild");
	CHECK(*P(SGX_SCENE_CCB_CTL) == 2 * CMD_SIZE && P(SGX_SCENE_CCB)[1] == SGX_SCENE_A_FIRST_EVER &&
	      P(SGX_SCENE_SURFACE_SYNC)[0] == 1 && P(SGX_SCENE_TA3D_SYNC)[0] == 1 && *P(SGX_SCENE_RT_PENDING) == 2,
	      "the scene from scratch: queue %x, flags %x", *P(SGX_SCENE_CCB_CTL), P(SGX_SCENE_CCB)[1]);
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
	run("rebuilt after a failure", rebuilt);
	char flat[512];
	snprintf(flat, sizeof flat, "%s/mix/flat.txt", recordings);
	FILE *f = fopen(flat, "r");
	if (f)
		fclose(f);
	if (!lib || !f) {
		printf("sgx_draw_test: SKIP: recordings not compared, %s\n",
		       !lib ? "no vendor library (SGX_LIBSRV_INIT)" : "no recordings (SGX_RECORDINGS)");
		if (getenv("SGX_REQUIRE_REFERENCE"))
			failures++;
	} else {
		CHECK(sgx_crc32(0, lib + SGX_FILE_FIRST, SGX_FILE_END - SGX_FILE_FIRST) == SGX_FILE_CRC, "library");
		run("tri_tq", tri_tq);
		run("tri_read_tq", tri_read);
		run("tri_moved_tq", tri_moved);
		run("tri_color_tq", tri_color);
		run("tri_blend_tq", tri_blend);
		run("tri_two_tq", tri_two);
		run("tri_read2_tq", tri_read2);
		run("tri_read_tq_gap", retarget);
		run("mesh", mesh);
		run("mix", mix);
		run("mix_minus", mix_minus);
		run("mix_plus", mix_plus);
		run("mix_tex", mix_tex);
		run("mix_tex3", mix_tex3);
		run("mix_swap", mix_swap);
		run("mix_alpha", mix_alpha);
		run("mix_back", mix_back);
		run("mix2", mix2);
		run("mix2 with the GPU off between the frames", mix2_power);
	}
	if (failures)
		return 1;
	printf("sgx_draw_test: ok\n");
	return 0;
}

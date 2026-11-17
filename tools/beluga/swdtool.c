/* swdtool: the Apollo3 coprocessor of the OPPO Watch from its application
 * processor, as root on the watch. The kernel's SWD driver bit-bangs the
 * link on two of the application processor's GPIOs and gives /dev/swd:
 * a connect, a reset, word reads and writes of the MCU's memory, and
 * reads and writes of whole blocks at the file offset; everything else
 * is built on those (swd.c). Closing the device ends the
 * debug session and lets the core run, so every path that has taken the
 * core resets the MCU before it ends, but for a flash that did not
 * finish: that one leaves the core in a loop in SRAM, as a reset would
 * start an image with its vector page erased.
 *
 *   swdtool probe                 what is there, without writing anything
 *   swdtool backup <dir>          flash and info space, read twice while halted
 *   swdtool flash <image>         write a firmware image at 0xc000, then reset
 *   swdtool restore <file.flash>  write a backup's image area back, then reset
 *   swdtool ramrun <image> <addr> load an image linked to run from SRAM and start it
 *   swdtool log                   the firmware's log
 *   swdtool shell                 the firmware's console
 *   swdtool reset                 reset the MCU and let it run
 *   swdtool probe-reset           reset into a halt before the image, show what the
 *                                 boot loader leaves, then reset and let it run
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include "swd.h"

#define SWD_DEV		"/dev/swd"
#define CONNECT		0x4400u
#define RESET_HALT	0x4402u
#define RESET_RUN	0x4405u
#define MEM_WRITE	0x40084408u
#define MEM_READ	0xc0084409u
#define RESET_NODE	"/sys/devices/platform/soc/soc:oppo,sensor-hub/reset"

#define CPUID		0xe000ed00u
#define CPUID_M4	0x410fc240u	/* without the revision */
#define BOX_MAGIC	0x58425753u
#define BOX_VERSION	1u
#define BOX_IN_SIZE	256u
#define BOX_OUT_SIZE	1728u
#define BOX_HEAD	64u
#define BOX_IN		64u
#define BOX_OUT		(BOX_IN + BOX_IN_SIZE)
#define LOG_BASE	0x1005b000u
#define LOG_SIZE	0x4800u
#define LOG_SIG		0x43474244u
#define LOG_CONSOLE	0x4000u
#define LOG_RECORD	0x400u
/* Our settings and then the stock data pages: only a restore writes them. */
#define IMAGE_END	0xe8000u

struct mem_arg {
	uint32_t addr, val;
};

enum { FREE, TAKEN, FLASHING };

static int dev = -1;
static volatile sig_atomic_t taken, stop;
static struct termios tty_saved;
static bool tty_raw, box_attached;

static int link_read(void *ctx, uint32_t addr, uint32_t *val)
{
	struct mem_arg a = { addr, 0 };
	if (ioctl(dev, MEM_READ, &a) < 0)
		return -1;
	*val = a.val;
	return 0;
}

static int link_write(void *ctx, uint32_t addr, uint32_t val)
{
	struct mem_arg a = { addr, val };
	return ioctl(dev, MEM_WRITE, &a) < 0 ? -1 : 0;
}

static void link_sleep(void *ctx, uint32_t us) { usleep(us); }

/* The driver takes the file offset as the address, which a 32-bit
 * off_t only holds below 2 GiB: flash and SRAM. */
#define BLOCK_END	0x80000000u

static int link_read_block(void *ctx, uint32_t addr, void *buf, uint32_t n)
{
	if (addr >= BLOCK_END || n > BLOCK_END - addr)
		return -1;
	for (uint32_t done = 0; done < n;) {
		ssize_t r = pread(dev, (uint8_t *)buf + done, n - done, (off_t)(addr + done));
		if (r <= 0)
			return -1;
		done += (uint32_t)r;
	}
	return 0;
}

static int link_write_block(void *ctx, uint32_t addr, const void *buf, uint32_t n)
{
	if (addr >= BLOCK_END || n > BLOCK_END - addr)
		return -1;
	for (uint32_t done = 0; done < n;) {
		ssize_t r = pwrite(dev, (const uint8_t *)buf + done, n - done, (off_t)(addr + done));
		if (r <= 0)
			return -1;
		done += (uint32_t)r;
	}
	return 0;
}

static struct swd_link swd = { .read = link_read, .write = link_write, .sleep_us = link_sleep };

/* Blocks only from a driver whose read gives what a word read does. */
static void blocks_probe(void)
{
	uint32_t a = 0, b = 1;
	if (!link_read(NULL, SWD_FLASH_FIRST, &a) && !link_read_block(NULL, SWD_FLASH_FIRST, &b, 4) && a == b) {
		swd.read_block = link_read_block;
		swd.write_block = link_write_block;
	} else
		fprintf(stderr, "swdtool: no block transfers, going word by word\n");
}

/* The reset line, through the driver or else the sensor hub's node: it
 * works whatever state the core is in. */
static int hard_reset(void)
{
	if (dev >= 0 && ioctl(dev, RESET_RUN, 0) >= 0)
		return 0;
	int fd = open(RESET_NODE, O_WRONLY);
	int r = fd >= 0 && write(fd, "1", 1) == 1 ? 0 : -1;
	if (fd >= 0)
		close(fd);
	return r;
}

static void cleanup(void)
{
	if (box_attached)
		swd_write(&swd, SWD_BOX_BASE + 40, 0);
	if (tty_raw)
		tcsetattr(0, TCSANOW, &tty_saved);
	if (taken == FLASHING)
		fprintf(stderr, swd_park(&swd) ? "swdtool: could not park the core: do not reset the MCU, flash again or restore\n"
					       : "swdtool: the core is parked in SRAM: flash again or restore before any reset\n");
	else if (taken && hard_reset())
		fprintf(stderr, "swdtool: the MCU is left halted: reset it by hand\n");
	taken = FREE;
}

/* A flash is let finish its page and stopped in order instead. */
static void on_signal(int sig)
{
	if (taken == FLASHING) {
		stop = 1;
		return;
	}
	cleanup();
	_exit(128 + sig);
}

static int fail(const char *what)
{
	fprintf(stderr, "swdtool: %s%s%s\n", what, errno ? ": " : "", errno ? strerror(errno) : "");
	return 1;
}

static int refuse(const char *why)
{
	fprintf(stderr, "swdtool: refused: %s\n", why);
	return 1;
}

static int take(void)
{
	taken = TAKEN;
	int r = swd_take(&swd);
	if (r == -3)
		return refuse("the watchdog is locked on and would reset the chip midway");
	if (r == -4)
		return refuse("the core keeps halting inside the boot ROM, maybe in a flash write of its own");
	return r ? fail("could not halt the core") : 0;
}

static uint8_t *load(const char *path, uint32_t *size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	uint8_t *p = NULL;
	long n = fseek(f, 0, SEEK_END) ? -1 : ftell(f);
	if (n > 0 && n <= (long)SWD_FLASH_SIZE && !fseek(f, 0, SEEK_SET)) {
		p = malloc((size_t)n);
		if (p && fread(p, 1, (size_t)n, f) != (size_t)n) {
			free(p);
			p = NULL;
		}
	}
	fclose(f);
	*size = p ? (uint32_t)n : 0;
	return p;
}

/* A new file only, written under a temporary name and renamed once it
 * is complete. */
static int save_new(const char *path, const void *p, size_t n)
{
	char tmp[600];
	if ((size_t)snprintf(tmp, sizeof tmp, "%s.part", path) >= sizeof tmp)
		return -1;
	if (!access(path, F_OK)) {
		errno = EEXIST;
		return -1;
	}
	int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0)
		return -1;
	bool ok = write(fd, p, n) == (ssize_t)n && !fsync(fd);
	ok = !close(fd) && ok;
	if (!ok || link(tmp, path)) {
		unlink(tmp);
		return -1;
	}
	return unlink(tmp);
}

static int read_all(uint32_t addr, uint32_t *buf, uint32_t words)
{
	for (uint32_t i = 0; i < words; i += 1024) {
		uint32_t n = words - i < 1024 ? words - i : 1024;
		if (swd_read_block(&swd, addr + 4 * i, buf + i, n))
			return -1;
		fprintf(stderr, "\r%08x", addr + 4 * (i + n));
	}
	fprintf(stderr, "\n");
	return 0;
}

static int check_chip(void)
{
	uint32_t id;
	if (swd_read(&swd, CPUID, &id))
		return fail("no link to the MCU");
	if ((id & 0xfffffff0u) != CPUID_M4) {
		fprintf(stderr, "swdtool: refused: CPUID %08x is not a Cortex-M4\n", id);
		return 1;
	}
	return 0;
}

static int cmd_probe(void)
{
	static const struct { const char *name; uint32_t addr; } regs[] = {
		{ "CPUID", CPUID }, { "DHCSR", 0xe000edf0u }, { "DEMCR", 0xe000edfcu },
		{ "CHIPPN", 0x40020000u }, { "CHIPREV", 0x4002000cu }, { "DEBUGGER", 0x40020020u },
		{ "BOOTLOADER", 0x400201a0u }, { "SHADOWVALID", 0x400201a4u },
		{ "FLASHWPROT0", 0x400203a0u }, { "FLASHWPROT1", 0x400203a4u },
		{ "FLASHRPROT0", 0x400203b0u }, { "FLASHRPROT1", 0x400203b4u },
		{ "RSTGEN STAT", 0x4ffff000u }, { "WDT CFG", 0x40024000u },
		{ "INFO0 SIG0", SWD_INFO0_BASE }, { "INFO0 SIG1", SWD_INFO0_BASE + 4 },
		{ "INFO0 SIG2", SWD_INFO0_BASE + 8 }, { "INFO0 SIG3", SWD_INFO0_BASE + 12 },
		{ "INFO0 SECURITY", SWD_INFO0_BASE + 0x10 },
		{ "INFO0 WPROT0", SWD_INFO0_BASE + 0x1f8 }, { "INFO0 WPROT1", SWD_INFO0_BASE + 0x1fc },
		{ "INFO0 RPROT0", SWD_INFO0_BASE + 0x200 }, { "INFO0 RPROT1", SWD_INFO0_BASE + 0x204 },
		{ "INFO0 MAINPTR", SWD_INFO0_BASE + 0xc00 },
		{ "flash 0xc000", 0xc000 }, { "flash 0xc004", 0xc004 },
		{ "SWD box", SWD_BOX_BASE },
	};
	int bad = 0;
	for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++) {
		uint32_t v;
		if (swd_read(&swd, regs[i].addr, &v)) {
			printf("%-15s %08x  read failed: %s\n", regs[i].name, regs[i].addr, strerror(errno));
			bad++;
		} else
			printf("%-15s %08x  %08x\n", regs[i].name, regs[i].addr, v);
	}
	return bad ? 1 : 0;
}

static uint32_t show(const char *name, uint32_t addr)
{
	uint32_t v = 0;
	if (swd_read(&swd, addr, &v))
		printf("%-22s %08x  read failed: %s\n", name, addr, strerror(errno));
	else
		printf("%-22s %08x  %08x\n", name, addr, v);
	return v;
}

/* The halt before the image that the vendor's flasher starts from. The
 * 0x4004 aliases are read last, as a fault on one can fail the reads
 * after it. */
static int cmd_probe_reset(void)
{
	if (check_chip())
		return 1;
	taken = TAKEN;
	if (ioctl(dev, RESET_HALT, 0) < 0)
		return fail("reset into a halt failed");
	static const struct { const char *name; unsigned reg; } regs[] = { { "PC", 15 }, { "MSP", 17 }, { "xPSR", 16 } };
	for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++) {
		uint32_t v;
		if (swd_reg_read(&swd, regs[i].reg, &v))
			printf("%-22s read failed\n", regs[i].name);
		else
			printf("%-22s %08x\n", regs[i].name, v);
	}
	show("DHCSR", 0xe000edf0u);
	show("VTOR", 0xe000ed08u);
	show("CLKGEN CCTRL", 0x40004018u);
	show("FLASHWPROT0", 0x400203a0u);
	show("BOOTLOADER", 0x400201a0u);
	show("SCRATCH0", 0x400201b0u);
	show("BOOTLOADER at 0x4004", 0x400401a0u);
	show("SCRATCH0 at 0x4004", 0x400401b0u);
	if (hard_reset())
		return fail("reset failed: the MCU is left halted");
	taken = FREE;
	return 0;
}

/* Both parts are read twice with the core halted and the watchdog off,
 * so the vendor's firmware writes nothing meanwhile, and saved only if
 * the core stayed halted, the reads match and the image area starts
 * with a vector table. */
static int cmd_backup(const char *dir)
{
	/* The boot loader's pages are read-protected, and a read there
	 * fails every access after it, so they are saved erased. */
	struct { const char *name; uint32_t addr, size, skip; uint32_t *data; } parts[] = {
		{ "mcu.flash", 0, SWD_FLASH_SIZE, SWD_FLASH_FIRST, NULL }, { "mcu.info0", SWD_INFO0_BASE, SWD_INFO0_SIZE, 0, NULL },
	};
	if (check_chip())
		return 1;
	if (mkdir(dir, 0755) && errno != EEXIST)
		return fail(dir);
	if (take())
		return 1;
	for (unsigned i = 0; i < 2; i++) {
		uint32_t skip = parts[i].skip / 4, words = parts[i].size / 4 - skip, *b = malloc(parts[i].size);
		parts[i].data = malloc(parts[i].size);
		if (!parts[i].data || !b)
			return fail("out of memory");
		memset(parts[i].data, 0xff, parts[i].size);
		memset(b, 0xff, parts[i].size);
		if (read_all(parts[i].addr + parts[i].skip, parts[i].data + skip, words) ||
		    read_all(parts[i].addr + parts[i].skip, b + skip, words))
			return fail("read failed");
		if (!swd_halted(&swd))
			return refuse("the core ran during the reads, nothing saved");
		if (memcmp(parts[i].data, b, parts[i].size))
			return refuse("two reads differ: the link is not reliable, nothing saved");
		free(b);
	}
	const uint8_t *flash = (const uint8_t *)parts[0].data;
	const char *why = swd_image_check(flash + SWD_FLASH_FIRST, SWD_FLASH_SIZE - SWD_FLASH_FIRST, SWD_FLASH_FIRST);
	if (why) {
		fprintf(stderr, "swdtool: refused: no plausible image at 0xc000 (%s): read-protected or not what it seems\n", why);
		return 1;
	}
	char path[512], manifest[1024];
	size_t mlen = 0;
	for (unsigned i = 0; i < 2; i++) {
		if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, parts[i].name) >= sizeof path)
			return refuse("directory name too long");
		if (save_new(path, parts[i].data, parts[i].size))
			return fail(path);
		uint32_t crc = swd_crc32(0, parts[i].data, parts[i].size);
		mlen += (size_t)snprintf(manifest + mlen, sizeof manifest - mlen, "%s %08x %u %08x\n", parts[i].name,
					 parts[i].addr, parts[i].size, crc);
		printf("%s: %u bytes from %08x, crc32 %08x\n", path, parts[i].size, parts[i].addr, crc);
	}
	if ((size_t)snprintf(path, sizeof path, "%s/manifest", dir) >= sizeof path || save_new(path, manifest, mlen))
		return fail("manifest");
	if (swd_reset(&swd))
		return fail("reset failed");
	taken = FREE;
	return 0;
}

static bool progress(uint32_t done, uint32_t total)
{
	fprintf(stderr, "\r%u / %u", done, total);
	return !stop;
}

static int flash_image(const uint8_t *img, uint32_t size)
{
	const char *why = swd_flash_check(SWD_FLASH_FIRST, size);
	if (!why)
		why = swd_image_check(img, size, SWD_FLASH_FIRST);
	if (why)
		return refuse(why);
	if (check_chip() || take())
		return 1;
	taken = FLASHING;
	int r = swd_flash(&swd, SWD_FLASH_FIRST, img, size, progress);
	fprintf(stderr, "\n");
	if (r == -2)
		taken = TAKEN;
	if (r) {
		fprintf(stderr, "swdtool: flash %s (%d); the boot loader is untouched\n", r == -7 ? "stopped" : "failed", r);
		return 1;
	}
	taken = TAKEN;
	printf("flashed %u bytes at %08x, crc32 %08x\n", size, SWD_FLASH_FIRST, swd_crc32(0, img, size));
	if (swd_reset(&swd))
		return fail("reset failed");
	taken = FREE;
	return 0;
}

static int cmd_flash(const char *path)
{
	uint32_t size;
	uint8_t *img = load(path, &size);
	if (!img)
		return fail(path);
	if (size > IMAGE_END - SWD_FLASH_FIRST)
		return refuse("image runs into the settings at 0xe8000");
	return flash_image(img, size);
}

/* A backup is the whole flash; only what is above the boot loader goes
 * back. */
static int cmd_restore(const char *path)
{
	uint32_t size;
	uint8_t *img = load(path, &size);
	if (!img || size != SWD_FLASH_SIZE)
		return refuse("not a flash backup");
	return flash_image(img + SWD_FLASH_FIRST, SWD_FLASH_SIZE - SWD_FLASH_FIRST);
}

static int cmd_ramrun(const char *path, const char *where)
{
	char *end;
	errno = 0;
	unsigned long addr = strtoul(where, &end, 0);
	if (errno || *end || addr > 0xffffffffu)
		return refuse("not an address");
	uint32_t size;
	uint8_t *img = load(path, &size);
	if (!img)
		return fail(path);
	const char *why = swd_ram_check((uint32_t)addr, size);
	if (!why)
		why = swd_image_check(img, size, (uint32_t)addr);
	if (why)
		return refuse(why);
	if (check_chip() || take())
		return 1;
	int r = swd_ram_run(&swd, img, size, (uint32_t)addr);
	if (r) {
		fprintf(stderr, "swdtool: ramrun failed (%d)\n", r);
		return 1;
	}
	/* Running now: closing the device only ends the debug session. */
	taken = FREE;
	return 0;
}

/* The box is the firmware's, so nothing in it is taken as an address:
 * its sizes must be the ones this tool knows and its indices inside
 * them. */
struct box {
	uint32_t in_head, in_tail, out_head, out_tail;
};

static int box_read(struct box *b)
{
	uint32_t w[12];
	if (swd_read_block(&swd, SWD_BOX_BASE, w, 12))
		return fail("no link to the MCU");
	if (w[0] != BOX_MAGIC) {
		fprintf(stderr, "swdtool: no firmware console (magic %08x): the stock firmware, or not started\n", w[0]);
		return 1;
	}
	if (w[1] != BOX_VERSION || w[2] != LOG_BASE || w[3] != LOG_SIZE || w[8] != BOX_IN_SIZE || w[9] != BOX_OUT_SIZE)
		return refuse("the firmware's console box is of another version");
	if (w[4] >= BOX_IN_SIZE || w[5] >= BOX_IN_SIZE || w[6] >= BOX_OUT_SIZE || w[7] >= BOX_OUT_SIZE)
		return refuse("the firmware's console box is corrupt");
	*b = (struct box){ w[4], w[5], w[6], w[7] };
	return 0;
}

/* A log zone, {signature, start, size} and the ring after it, read until
 * two reads of its header around the data agree, as the firmware keeps
 * writing. */
static int read_zone(uint32_t addr, uint32_t bytes, bool ring, FILE *out)
{
	uint32_t cap = bytes - 12, *w = malloc(bytes);
	if (!w)
		return fail("out of memory");
	for (int tries = 0; tries < 5; tries++) {
		uint32_t a[3], b[3];
		if (swd_read_block(&swd, addr, a, 3) || swd_read_block(&swd, addr + 12, w, cap / 4) ||
		    swd_read_block(&swd, addr, b, 3)) {
			free(w);
			return fail("read failed");
		}
		if (memcmp(a, b, sizeof a))
			continue;
		if (a[0] == LOG_SIG && a[1] < cap && a[2] <= cap) {
			const uint8_t *d = (const uint8_t *)w;
			uint32_t from = ring ? (a[1] + cap - a[2]) % cap : 0;
			for (uint32_t i = 0; i < a[2]; i++)
				fputc(d[(from + i) % cap], out);
		}
		free(w);
		return 0;
	}
	free(w);
	return refuse("the log kept changing while it was read");
}

static int cmd_log(void)
{
	struct box b;
	if (box_read(&b))
		return 1;
	if (read_zone(LOG_BASE, LOG_CONSOLE, true, stdout))
		return 1;
	printf("\n--- fault record:\n");
	return read_zone(LOG_BASE + LOG_CONSOLE, LOG_RECORD, false, stdout);
}

/* 1 while the ring is full; the byte is kept for the next try. */
static int put_in(uint8_t c)
{
	struct box b;
	if (box_read(&b))
		return -1;
	if ((b.in_head + 1) % BOX_IN_SIZE == b.in_tail)
		return 1;
	uint32_t at = SWD_BOX_BASE + BOX_IN + b.in_head, w;
	if (swd_read(&swd, at & ~3u, &w))
		return -1;
	w = (w & ~(0xffu << 8 * (at & 3))) | (uint32_t)c << 8 * (at & 3);
	if (swd_write(&swd, at & ~3u, w))
		return -1;
	return swd_write(&swd, SWD_BOX_BASE + 16, (b.in_head + 1) % BOX_IN_SIZE) ? -1 : 0;
}

static int drain_out(void)
{
	struct box b;
	if (box_read(&b))
		return -1;
	uint32_t tail = b.out_tail;
	while (tail != b.out_head) {
		uint32_t at = SWD_BOX_BASE + BOX_OUT + tail, w;
		if (swd_read(&swd, at & ~3u, &w))
			return -1;
		putchar((int)(w >> 8 * (at & 3) & 0xff));
		tail = (tail + 1) % BOX_OUT_SIZE;
	}
	fflush(stdout);
	return swd_write(&swd, SWD_BOX_BASE + 28, tail) ? -1 : 0;
}

static int cmd_shell(void)
{
	struct box b;
	if (box_read(&b))
		return 1;
	if (!tcgetattr(0, &tty_saved)) {
		struct termios raw = tty_saved;
		cfmakeraw(&raw);
		tty_raw = !tcsetattr(0, TCSANOW, &raw);
	}
	if (swd_write(&swd, SWD_BOX_BASE + 40, 1))
		return fail("no link to the MCU");
	box_attached = true;
	fprintf(stderr, "swdtool: console, ctrl-] to leave\r\n");
	int pending = -1, r = 0;
	for (;;) {
		struct pollfd p = { 0, POLLIN, 0 };
		if (pending < 0 && poll(&p, 1, 20) > 0) {
			uint8_t c;
			if (read(0, &c, 1) != 1 || c == 0x1d)
				break;
			pending = c;
		} else if (pending >= 0)
			usleep(5000);
		if (pending >= 0) {
			int e = put_in((uint8_t)pending);
			if (e < 0) {
				r = 1;
				break;
			}
			if (!e)
				pending = -1;
		}
		if (drain_out()) {
			r = 1;
			break;
		}
	}
	return r;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: swdtool probe | backup <dir> | flash <image> | restore <dir>/mcu.flash |\n"
				"               ramrun <image> <addr> | log | shell | reset | probe-reset\n");
		return 2;
	}
	const char *c = argv[1];
	dev = open(SWD_DEV, O_RDWR);
	if (dev < 0)
		return !strcmp(c, "reset") ? (hard_reset() ? fail("reset failed") : 0) : fail("open " SWD_DEV);
	if (ioctl(dev, CONNECT, 0) < 0) {
		if (!strcmp(c, "reset"))
			return hard_reset() ? fail("reset failed") : 0;
		return fail("no SWD connection to the MCU");
	}
	atexit(cleanup);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);
	blocks_probe();
	if (!strcmp(c, "probe"))
		return cmd_probe();
	if (!strcmp(c, "probe-reset"))
		return cmd_probe_reset();
	if (!strcmp(c, "backup") && argc == 3)
		return cmd_backup(argv[2]);
	if (!strcmp(c, "flash") && argc == 3)
		return cmd_flash(argv[2]);
	if (!strcmp(c, "restore") && argc == 3)
		return cmd_restore(argv[2]);
	if (!strcmp(c, "ramrun") && argc == 4)
		return cmd_ramrun(argv[2], argv[3]);
	if (!strcmp(c, "log"))
		return cmd_log();
	if (!strcmp(c, "shell"))
		return cmd_shell();
	if (!strcmp(c, "reset"))
		return hard_reset() ? fail("reset failed") : 0;
	fprintf(stderr, "swdtool: unknown command %s\n", c);
	return 2;
}

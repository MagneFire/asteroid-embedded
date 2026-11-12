/* swdtool: the Apollo3 coprocessor of the OPPO Watch from its application
 * processor, as root on the watch. The kernel's SWD driver bit-bangs the
 * link on two of the application processor's GPIOs and gives /dev/swd,
 * whose ioctls read and write one word of the MCU's memory at a time;
 * everything else is built on that (swd.c).
 *
 *   swdtool probe                 what is there, without writing anything
 *   swdtool backup <file>         flash and info space, read twice: <file>.flash, <file>.info0
 *   swdtool flash <image> [addr]  write a firmware image, 0xc000 by default, then reset
 *   swdtool restore <file.flash>  write a backup's image area back, then reset
 *   swdtool ramrun <image> <addr> load an image linked to run from SRAM and start it
 *   swdtool log                   the firmware's log and its last fault record
 *   swdtool shell                 the firmware's console
 *   swdtool reset                 reset the MCU and let it run
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "swd.h"

#define SWD_DEV		"/dev/swd"
#define MEM_WRITE	0x40084408u
#define MEM_READ	0xc0084409u
#define RESET_NODE	"/sys/devices/platform/soc/soc:oppo,sensor-hub/reset"

#define BOX_MAGIC	0x58425753u
#define BOX_LOG_BASE	8
#define BOX_IN_HEAD	16
#define BOX_IN_TAIL	20
#define BOX_OUT_HEAD	24
#define BOX_OUT_TAIL	28
#define BOX_IN_SIZE	32
#define BOX_OUT_SIZE	36
#define BOX_ATTACHED	40
#define BOX_IN		64
#define LOG_SIG		0x43474244u
#define LOG_CONSOLE	0x4000u
#define LOG_RECORD	0x400u

struct mem_arg {
	uint32_t addr, val;
};

static int dev = -1;

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

static const struct swd_link swd = { link_read, link_write, link_sleep, NULL };

static int die(const char *what)
{
	fprintf(stderr, "swdtool: %s%s%s\n", what, errno ? ": " : "", errno ? strerror(errno) : "");
	return 1;
}

static uint8_t *load(const char *path, uint32_t *size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *p = n > 0 && n <= (long)SWD_FLASH_SIZE ? malloc((size_t)n) : NULL;
	if (p && fread(p, 1, (size_t)n, f) != (size_t)n) {
		free(p);
		p = NULL;
	}
	fclose(f);
	*size = p ? (uint32_t)n : 0;
	return p;
}

static int save(const char *path, const void *p, size_t n)
{
	FILE *f = fopen(path, "wb");
	if (!f || fwrite(p, 1, n, f) != n) {
		if (f)
			fclose(f);
		return -1;
	}
	return fclose(f);
}

static int cmd_probe(void)
{
	static const struct { const char *name; uint32_t addr; } regs[] = {
		{ "CPUID", 0xe000ed00u }, { "DHCSR", 0xe000edf0u }, { "DEMCR", 0xe000edfcu },
		{ "CHIPPN", 0x40020000u }, { "CHIPREV", 0x4002000cu }, { "BOOTLOADER", 0x400201a0u },
		{ "SHADOWVALID", 0x400201a4u }, { "DEBUGGER", 0x400201b0u }, { "RSTGEN STAT", 0x4ffff000u },
		{ "INFO0 +0", SWD_INFO0_BASE }, { "INFO0 +4", SWD_INFO0_BASE + 4 }, { "INFO0 +8", SWD_INFO0_BASE + 8 },
		{ "flash 0x0", 0 }, { "flash 0x4", 4 }, { "flash 0xc000", 0xc000 }, { "flash 0xc004", 0xc004 },
		{ "SWD box", SWD_BOX_BASE },
	};
	int bad = 0;
	for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++) {
		uint32_t v;
		if (swd_read(&swd, regs[i].addr, &v)) {
			printf("%-12s %08x  read failed: %s\n", regs[i].name, regs[i].addr, strerror(errno));
			bad++;
		} else
			printf("%-12s %08x  %08x\n", regs[i].name, regs[i].addr, v);
	}
	return bad ? 1 : 0;
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

static int cmd_backup(const char *base)
{
	struct { const char *ext; uint32_t addr, size; } parts[] = {
		{ ".flash", 0, SWD_FLASH_SIZE }, { ".info0", SWD_INFO0_BASE, SWD_INFO0_SIZE },
	};
	for (unsigned i = 0; i < 2; i++) {
		uint32_t words = parts[i].size / 4, *a = malloc(parts[i].size), *b = malloc(parts[i].size);
		if (!a || !b)
			return die("out of memory");
		if (read_all(parts[i].addr, a, words) || read_all(parts[i].addr, b, words))
			return die("read failed");
		if (memcmp(a, b, parts[i].size))
			return die("two reads differ: the link is not reliable, nothing saved");
		char path[512];
		snprintf(path, sizeof path, "%s%s", base, parts[i].ext);
		if (save(path, a, parts[i].size))
			return die(path);
		printf("%s: %u bytes from %08x, crc32 %08x\n", path, parts[i].size, parts[i].addr,
		       swd_crc32(0, a, parts[i].size));
		free(a);
		free(b);
	}
	return 0;
}

static void progress(uint32_t done, uint32_t total) { fprintf(stderr, "\r%u / %u", done, total); }

static int flash_image(const uint8_t *img, uint32_t size, uint32_t addr)
{
	const char *why = swd_flash_check(addr, size);
	if (!why && addr == SWD_FLASH_FIRST)
		why = swd_image_check(img, size, addr);
	if (why) {
		fprintf(stderr, "swdtool: refused: %s\n", why);
		return 1;
	}
	int r = swd_flash(&swd, addr, img, size, progress);
	fprintf(stderr, "\n");
	if (r) {
		fprintf(stderr, "swdtool: flash failed (%d); the boot loader is untouched, try again or restore\n", r);
		return 1;
	}
	printf("flashed %u bytes at %08x, crc32 %08x\n", size, addr, swd_crc32(0, img, size));
	return swd_reset(&swd) ? die("reset failed") : 0;
}

static int cmd_flash(const char *path, uint32_t addr)
{
	uint32_t size;
	uint8_t *img = load(path, &size);
	if (!img)
		return die(path);
	return flash_image(img, size, addr);
}

/* A backup is the whole flash; only what is above the boot loader goes
 * back. */
static int cmd_restore(const char *path)
{
	uint32_t size;
	uint8_t *img = load(path, &size);
	if (!img || size != SWD_FLASH_SIZE)
		return die("not a flash backup");
	return flash_image(img + SWD_FLASH_FIRST, SWD_FLASH_SIZE - SWD_FLASH_FIRST, SWD_FLASH_FIRST);
}

static int cmd_ramrun(const char *path, uint32_t addr)
{
	uint32_t size;
	uint8_t *img = load(path, &size);
	if (!img)
		return die(path);
	const char *why = swd_image_check(img, size, addr);
	if (why) {
		fprintf(stderr, "swdtool: refused: %s\n", why);
		return 1;
	}
	int r = swd_ram_run(&swd, img, size, addr);
	if (r)
		fprintf(stderr, "swdtool: ramrun failed (%d)\n", r);
	return r ? 1 : 0;
}

static int box_word(uint32_t off, uint32_t *v) { return swd_read(&swd, SWD_BOX_BASE + off, v); }

static int box_check(void)
{
	uint32_t magic;
	if (box_word(0, &magic))
		return die("no link to the MCU");
	if (magic != BOX_MAGIC) {
		fprintf(stderr, "swdtool: no firmware console (magic %08x): the stock firmware, or not started\n", magic);
		return 1;
	}
	return 0;
}

/* A log zone: {signature, start, size} and the ring after it. */
static int print_zone(uint32_t addr, uint32_t cap_bytes, bool ring)
{
	uint32_t hdr[3];
	if (swd_read_block(&swd, addr, hdr, 3))
		return die("read failed");
	uint32_t cap = cap_bytes - 12;
	if (hdr[0] != LOG_SIG || hdr[1] >= cap + !ring || hdr[2] > cap)
		return 0;
	uint32_t *w = malloc(cap + 4);
	if (!w || swd_read_block(&swd, addr + 12, w, (cap + 3) / 4))
		return die("read failed");
	const uint8_t *d = (const uint8_t *)w;
	uint32_t from = ring ? (hdr[1] + cap - hdr[2]) % cap : 0;
	for (uint32_t i = 0; i < hdr[2]; i++)
		putchar(d[(from + i) % cap]);
	free(w);
	return 0;
}

static int cmd_log(void)
{
	if (box_check())
		return 1;
	uint32_t base;
	if (box_word(BOX_LOG_BASE, &base))
		return die("read failed");
	print_zone(base, LOG_CONSOLE, true);
	printf("\n--- fault record:\n");
	return print_zone(base + LOG_CONSOLE, LOG_RECORD, false);
}

/* A byte into the input ring: the word it is in is read, changed and
 * written back, which is safe as only this side writes the ring. */
static int put_in(uint32_t size, uint8_t c)
{
	uint32_t head, tail, w;
	if (box_word(BOX_IN_HEAD, &head) || box_word(BOX_IN_TAIL, &tail))
		return -1;
	if ((head + 1) % size == tail % size)
		return 1;
	uint32_t at = SWD_BOX_BASE + BOX_IN + head % size;
	if (swd_read(&swd, at & ~3u, &w))
		return -1;
	w = (w & ~(0xffu << 8 * (at & 3))) | (uint32_t)c << 8 * (at & 3);
	if (swd_write(&swd, at & ~3u, w))
		return -1;
	return swd_write(&swd, SWD_BOX_BASE + BOX_IN_HEAD, (head + 1) % size);
}

static int drain_out(uint32_t in_size, uint32_t out_size)
{
	uint32_t head, tail;
	if (box_word(BOX_OUT_HEAD, &head) || box_word(BOX_OUT_TAIL, &tail))
		return -1;
	uint32_t base = SWD_BOX_BASE + BOX_IN + in_size;
	while (tail != head) {
		uint32_t at = base + tail, w;
		if (swd_read(&swd, at & ~3u, &w))
			return -1;
		putchar((int)(w >> 8 * (at & 3) & 0xff));
		tail = (tail + 1) % out_size;
	}
	fflush(stdout);
	return swd_write(&swd, SWD_BOX_BASE + BOX_OUT_TAIL, tail);
}

static int cmd_shell(void)
{
	if (box_check())
		return 1;
	uint32_t in_size, out_size;
	if (box_word(BOX_IN_SIZE, &in_size) || box_word(BOX_OUT_SIZE, &out_size) || !in_size || !out_size)
		return die("read failed");
	struct termios old, raw;
	bool tty = !tcgetattr(0, &old);
	if (tty) {
		raw = old;
		cfmakeraw(&raw);
		tcsetattr(0, TCSANOW, &raw);
	}
	swd_write(&swd, SWD_BOX_BASE + BOX_ATTACHED, 1);
	fprintf(stderr, "swdtool: console, ctrl-] to leave\r\n");
	for (;;) {
		struct pollfd p = { 0, POLLIN, 0 };
		if (poll(&p, 1, 20) > 0) {
			uint8_t c;
			if (read(0, &c, 1) != 1 || c == 0x1d)
				break;
			if (put_in(in_size, c) < 0)
				break;
		}
		if (drain_out(in_size, out_size))
			break;
	}
	swd_write(&swd, SWD_BOX_BASE + BOX_ATTACHED, 0);
	if (tty)
		tcsetattr(0, TCSANOW, &old);
	return 0;
}

static int cmd_reset(void)
{
	if (!swd_reset(&swd))
		return 0;
	int fd = open(RESET_NODE, O_WRONLY);
	if (fd < 0 || write(fd, "1", 1) != 1)
		return die("reset failed over SWD and through " RESET_NODE);
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: swdtool probe | backup <file> | flash <image> [addr] | restore <file.flash> |\n"
				"               ramrun <image> <addr> | log | shell | reset\n");
		return 2;
	}
	dev = open(SWD_DEV, O_RDWR);
	if (dev < 0)
		return die("open " SWD_DEV);
	const char *c = argv[1];
	if (!strcmp(c, "probe"))
		return cmd_probe();
	if (!strcmp(c, "backup") && argc == 3)
		return cmd_backup(argv[2]);
	if (!strcmp(c, "flash") && (argc == 3 || argc == 4))
		return cmd_flash(argv[2], argc == 4 ? (uint32_t)strtoul(argv[3], NULL, 0) : SWD_FLASH_FIRST);
	if (!strcmp(c, "restore") && argc == 3)
		return cmd_restore(argv[2]);
	if (!strcmp(c, "ramrun") && argc == 4)
		return cmd_ramrun(argv[2], (uint32_t)strtoul(argv[3], NULL, 0));
	if (!strcmp(c, "log"))
		return cmd_log();
	if (!strcmp(c, "shell"))
		return cmd_shell();
	if (!strcmp(c, "reset"))
		return cmd_reset();
	fprintf(stderr, "swdtool: unknown command %s\n", c);
	return 2;
}

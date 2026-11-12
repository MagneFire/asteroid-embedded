/* The console over SWD: the application processor reads and writes this
 * SRAM block through the debug port while the core runs. It finds the
 * block by its magic at SWD_BOX_BASE, with the address and size of the
 * log region in it. Input is a ring the other side fills and this side
 * empties; output is the reverse. Each side writes only its own index,
 * after the data, so neither needs a lock. Output that finds the ring
 * full is dropped: nobody may be reading. */
#include <string.h>
#include "asteroid.h"
#include "console/console.h"

#define MAGIC		0x58425753u	/* "SWBX" */
#define VERSION		1u
#define IN_SIZE		256u
#define OUT_SIZE	(SWD_BOX_SIZE - IN_SIZE - 64u)

struct box {
	uint32_t magic, version, log_base, log_size;
	uint32_t in_head, in_tail;	/* head written over SWD */
	uint32_t out_head, out_tail;	/* tail written over SWD */
	uint32_t in_size, out_size, attached, dropped;
	uint32_t reserved[4];
	uint8_t in[IN_SIZE];
	uint8_t out[OUT_SIZE];
};
_Static_assert(sizeof(struct box) <= SWD_BOX_SIZE, "SWD box too big");

#define BOX ((volatile struct box *)(uintptr_t)SWD_BOX_BASE)

static size_t out_room(void *ctx)
{
	volatile struct box *b = BOX;
	return OUT_SIZE - 1 - (b->out_head + OUT_SIZE - b->out_tail) % OUT_SIZE;
}

static void out(const char *s, size_t n, void *ctx)
{
	volatile struct box *b = BOX;
	uint32_t head = b->out_head;
	for (size_t i = 0; i < n; i++) {
		if (out_room(ctx) == 0) {
			b->dropped += (uint32_t)(n - i);
			break;
		}
		b->out[head] = (uint8_t)s[i];
		head = (head + 1) % OUT_SIZE;
		barrier();
		b->out_head = head;
	}
}

void swdbox_init(void)
{
	volatile struct box *b = BOX;
	memset((void *)(uintptr_t)SWD_BOX_BASE, 0, sizeof(struct box));
	b->version = VERSION;
	b->log_base = RAMOOPS_BASE;
	b->log_size = RAMOOPS_SIZE;
	b->in_size = IN_SIZE;
	b->out_size = OUT_SIZE;
	barrier();
	b->magic = MAGIC;
}

/* The other side sets attached while it has a terminal open. */
void swdbox_poll(void)
{
	static bool attached;
	volatile struct box *b = BOX;
	if (b->magic != MAGIC)
		return;
	bool now = b->attached != 0;
	if (now != attached) {
		attached = now;
		if (now)
			console_attach(out, out_room, NULL, true);
		else
			console_detach();
	}
	uint32_t head = b->in_head % IN_SIZE, tail = b->in_tail % IN_SIZE;
	barrier();
	while (tail != head) {
		char buf[32];
		size_t n = 0;
		while (tail != head && n < sizeof buf) {
			buf[n++] = (char)b->in[tail];
			tail = (tail + 1) % IN_SIZE;
		}
		barrier();
		b->in_tail = tail;
		console_input(buf, n);
	}
}

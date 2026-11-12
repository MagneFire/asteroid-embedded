/* The SGX530's address space and what its microkernel needs in it
 * before it can start. Nothing here touches the hardware, so the host
 * tests build it too. */
#ifndef SGX_IMAGE_H
#define SGX_IMAGE_H
#include <stdint.h>
#include <stdbool.h>

/* The heaps the microkernel and its clients were built for. Shared 3D
 * starts where the parameter buffer does and is never allocated from. */
enum {
	SGX_HEAP_GENERAL,
	SGX_HEAP_TA_DATA,
	SGX_HEAP_KERNEL_CODE,
	SGX_HEAP_KERNEL_DATA,
	SGX_HEAP_PIXEL_USSE,
	SGX_HEAP_VERTEX_USSE,
	SGX_HEAP_PDS_PIXEL,
	SGX_HEAP_PDS_VERTEX,
	SGX_HEAP_SYNC,
	SGX_HEAP_SHARED_3D,
	SGX_HEAP_3D_PARAMETERS,
	SGX_HEAPS
};
uint32_t sgx_heap_base(unsigned heap);

#define SGX_MEM_READ		(1u << 0)
#define SGX_MEM_WRITE		(1u << 1)
#define SGX_MEM_CACHE_CONSISTENT (1u << 2)
#define SGX_MEM_NO_SYNC		(1u << 3)
#define SGX_MEM_DUMMY		(1u << 5)	/* every page is the same one */
#define SGX_MEM_EDM_PROTECT	(1u << 6)

#define SGX_PAGE		4096u

/* A pool of pages with the GPU's page tables in it: the directory is
 * the pool's first page. */
struct sgx_mem {
	uint8_t *base;
	uint32_t phys, size, used;
	uint32_t next[SGX_HEAPS];
	uint32_t dummy;
};

void sgx_mem_init(struct sgx_mem *m, void *base, uint32_t phys, uint32_t size);
/* The physical address of new, zeroed pages outside the GPU's address
 * space, or 0. */
uint32_t sgx_pages(struct sgx_mem *m, uint32_t pages);
/* The GPU address of new, zeroed memory in a heap, or 0. */
uint32_t sgx_alloc(struct sgx_mem *m, unsigned heap, uint32_t size, uint32_t align, uint32_t flags);
/* Memory from elsewhere, page aligned, at the next free address of a heap. */
uint32_t sgx_map(struct sgx_mem *m, unsigned heap, uint32_t phys, uint32_t size, uint32_t flags);
/* Where the CPU finds a GPU address of the pool, or NULL. */
void *sgx_ptr(const struct sgx_mem *m, uint32_t addr);

struct sgx_image {
	uint32_t ccb, ccb_ctl, kicker, ta3d_ctl, host_ctl, misc, trace, hwperf, ta_sig, sig_3d;
	uint32_t code[4];	/* microkernel, TA, SPM, 3D */
	uint32_t pds[5];
	uint32_t stencil[2];
};

/* The file's bytes the image is made of, and their CRC-32. */
#define SGX_FILE_FIRST		0x2898u
#define SGX_FILE_END		0x1c598u
#define SGX_FILE_CRC		0xde27f927u
uint32_t sgx_crc32(uint32_t crc, const void *data, uint32_t len);

typedef bool (*sgx_read_fn)(void *ctx, uint32_t off, void *buf, uint32_t len);
/* Builds the image from the vendor's libsrv_init.so, which read returns
 * the bytes of. */
bool sgx_image_build(struct sgx_mem *m, struct sgx_image *img, sgx_read_fn read, void *ctx);

/* Commands to the microkernel, and the eight words of one in its queue. */
enum { SGX_CMD_TA, SGX_CMD_TRANSFER, SGX_CMD_2D, SGX_CMD_POWER, SGX_CMD_SUSPEND, SGX_CMD_CLEANUP, SGX_CMD_GETMISCINFO,
       SGX_CMD_PROCESS_QUEUES, SGX_CMDS };
#define SGX_CACHE_INVAL_PT	1u
#define SGX_CACHE_INVAL_PD	2u
#define SGX_CACHE_INVAL_DATA	8u
void sgx_command_words(uint32_t c[8], unsigned type, uint32_t cache_control, uint32_t arg);

/* Puts v into the 32-bit constant of the GPU instruction at off. */
void sgx_set_constant(uint32_t *program, uint32_t off, uint32_t v);

struct sgx_reg {
	uint32_t off, value;
};
#define SGX_SCRIPT_MAX		40
enum { SGX_SCRIPT_BEFORE_RESET, SGX_SCRIPT_AFTER_RESET, SGX_SCRIPT_OFF };
/* The register writes of a script, and how many. */
unsigned sgx_script(const struct sgx_image *img, unsigned which, struct sgx_reg *out);
#endif

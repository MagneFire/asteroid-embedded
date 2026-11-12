#ifndef SGX_H
#define SGX_H
#include <stdint.h>
#include <stdbool.h>

/* What the GPU and its microkernel say about themselves. */
struct sgx_info {
	uint32_t core_id, core_revision;
	uint32_t ddk_version, ddk_build, software_core, software_revision, build_options;
	bool structs_match;	/* the sizes of what is shared with the microkernel */
	uint32_t start_us;
};

/* Powers the GPU and starts its microkernel, which is read from the
 * watch's system partition the first time. */
bool sgx_start(void);
void sgx_stop(void);
bool sgx_running(void);
bool sgx_query(struct sgx_info *info);
/* The microkernel's trace of its last steps, for the log. */
void sgx_log_state(void);

/* For the drawing on top (sgx_draw.c): the GPU's memory, and a TA
 * command for a render context, with the cache-control bits of
 * SGX_CACHE_INVAL_* the context's page tables need. */
struct sgx_mem;
struct sgx_mem *sgx_memory(void);
bool sgx_submit_ta(uint32_t context, uint32_t cache);
#endif

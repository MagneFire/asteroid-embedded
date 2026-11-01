#ifndef FDT_H
#define FDT_H
#include <stdbool.h>
#include <stdint.h>

/* Whether p points at a flattened device tree. */
bool fdt_valid(const void *fdt);
/* The property name of the node at path ("/" or "/chosen", one level
 * of nodes at most per component), its length in *len; NULL if absent. */
const void *fdt_prop(const void *fdt, const char *path, const char *name, uint32_t *len);
/* Whether a string-list property (compatible) holds s. */
bool fdt_strlist_has(const char *list, uint32_t len, const char *s);
#endif

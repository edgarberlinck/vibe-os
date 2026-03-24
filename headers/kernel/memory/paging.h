#ifndef KERNEL_MEMORY_PAGING_H
#define KERNEL_MEMORY_PAGING_H

#include <stdint.h>

void paging_init(void);
int paging_is_enabled(void);
uintptr_t paging_page_directory_phys(void);

#endif /* KERNEL_MEMORY_PAGING_H */

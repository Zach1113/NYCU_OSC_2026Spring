#ifndef KERNEL_MM_H
#define KERNEL_MM_H

void mm_init(const void *fdt);
void *alloc(unsigned long size);
void free(void *ptr);
void mm_page_get(void *page);
void mm_page_put(void *page);
unsigned long mm_page_ref_count(void *page);
void mm_dump_free_areas(void);
void mm_self_test(void);
void mm_set_log_enabled(int enabled);

#endif

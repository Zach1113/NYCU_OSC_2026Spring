#ifndef CPIO_H
#define CPIO_H

void cpio_set_archive(const void *start, const void *end);
void cpio_init_from_dtb(const void *fdt);
int cpio_ready(void);
void cpio_ls(void);
void cpio_cat(const char *filename);

#endif

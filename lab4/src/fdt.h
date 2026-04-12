#ifndef FDT_H
#define FDT_H

#define FDT_BEGIN_NODE 0x00000001U
#define FDT_END_NODE   0x00000002U
#define FDT_PROP       0x00000003U
#define FDT_NOP        0x00000004U
#define FDT_END        0x00000009U

struct fdt_header {
    unsigned int magic;
    unsigned int totalsize;
    unsigned int off_dt_struct;
    unsigned int off_dt_strings;
    unsigned int off_mem_rsvmap;
    unsigned int version;
    unsigned int last_comp_version;
    unsigned int boot_cpuid_phys;
    unsigned int size_dt_strings;
    unsigned int size_dt_struct;
};

unsigned int fdt_be32(const void *p);
unsigned long fdt_be64(const void *p);
int fdt_path_offset(const void *fdt, const char *path);
const void *fdt_getprop(const void *fdt, int nodeoffset, const char *name, int *lenp);
int fdt_get_memory_region(const void *fdt, int entry,
                          unsigned long *base, unsigned long *size);
int fdt_get_reserved_memory_region(const void *fdt, int entry,
                                   unsigned long *base, unsigned long *size);
int fdt_get_initrd_region(const void *fdt,
                          unsigned long *start, unsigned long *end);

#endif

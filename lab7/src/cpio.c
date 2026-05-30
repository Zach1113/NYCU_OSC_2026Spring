#include "cpio.h"
#include "fdt.h"
#include "uart.h"
#include "vm.h"

struct cpio_newc_header {
    char c_magic[6];
    char c_ino[8];
    char c_mode[8];
    char c_uid[8];
    char c_gid[8];
    char c_nlink[8];
    char c_mtime[8];
    char c_filesize[8];
    char c_devmajor[8];
    char c_devminor[8];
    char c_rdevmajor[8];
    char c_rdevminor[8];
    char c_namesize[8];
    char c_check[8];
};

static const char *g_rd_start;
static const char *g_rd_end;

static unsigned long align4(unsigned long x) {
    return (x + 3UL) & ~3UL;
}

static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int filename_match(const char *archive_name, const char *target) {
    if (streq(archive_name, target))
        return 1;
    if (archive_name[0] == '.' && archive_name[1] == '/')
        return streq(archive_name + 2, target);
    return 0;
}

static unsigned long hex_to_u32(const char *s, int n) {
    unsigned long v = 0;
    while (n-- > 0) {
        char c = *s++;
        v <<= 4;
        if (c >= '0' && c <= '9')
            v += (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v += (unsigned long)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v += (unsigned long)(c - 'A' + 10);
        else
            return 0;
    }
    return v;
}

static unsigned long dec_width(unsigned long v) {
    unsigned long width = 1;
    while (v >= 10) {
        v /= 10;
        width++;
    }
    return width;
}

static int cpio_walk_next(const char **cursor,
                          const struct cpio_newc_header **out_hdr,
                          const char **out_name,
                          const char **out_data,
                          unsigned long *out_namesz,
                          unsigned long *out_filesz) {
    const char *p = *cursor;
    const struct cpio_newc_header *h;
    const char *name;
    const char *data;
    unsigned long namesz;
    unsigned long filesz;
    unsigned long next;

    if (!g_rd_start || !g_rd_end || p < g_rd_start || p >= g_rd_end)
        return 0;

    if ((unsigned long)(g_rd_end - p) < sizeof(struct cpio_newc_header))
        return 0;

    h = (const struct cpio_newc_header *)p;
    if (!(h->c_magic[0] == '0' && h->c_magic[1] == '7' && h->c_magic[2] == '0' &&
          h->c_magic[3] == '7' && h->c_magic[4] == '0' && h->c_magic[5] == '1'))
        return 0;

    namesz = hex_to_u32(h->c_namesize, 8);
    filesz = hex_to_u32(h->c_filesize, 8);
    if (namesz == 0)
        return 0;

    name = p + sizeof(struct cpio_newc_header);
    if (name + namesz > g_rd_end)
        return 0;

    data = (const char *)align4((unsigned long)(name + namesz));
    if (data > g_rd_end || data + filesz > g_rd_end)
        return 0;

    next = align4((unsigned long)(data + filesz));
    if ((const char *)next < p || (const char *)next > g_rd_end)
        return 0;

    *cursor = (const char *)next;
    if (out_hdr)
        *out_hdr = h;
    if (out_name)
        *out_name = name;
    if (out_data)
        *out_data = data;
    if (out_namesz)
        *out_namesz = namesz;
    if (out_filesz)
        *out_filesz = filesz;
    return 1;
}

void cpio_set_archive(const void *start, const void *end) {
    g_rd_start = (const char *)start;
    g_rd_end = (const char *)end;
}

void cpio_init_from_dtb(const void *fdt) {
    unsigned long start;
    unsigned long end;

    g_rd_start = 0;
    g_rd_end = 0;

    if (!fdt_get_initrd_region(fdt, &start, &end))
        return;

    cpio_set_archive((const void *)phys_to_virt(start),
                     (const void *)phys_to_virt(end));
}

int cpio_ready(void) {
    return g_rd_start && g_rd_end && g_rd_end > g_rd_start;
}

void cpio_ls(void) {
    const char *cursor;
    unsigned long max_width = 1;
    unsigned long file_count = 0;

    if (!cpio_ready()) {
        uart_puts("ls: initrd not set\n");
        return;
    }

    cursor = g_rd_start;
    while (cursor < g_rd_end) {
        const char *name;
        unsigned long namesz;
        unsigned long filesz;

        if (!cpio_walk_next(&cursor, 0, &name, 0, &namesz, &filesz)) {
            uart_puts("ls: invalid cpio archive\n");
            return;
        }

        if (namesz >= 11 &&
            name[0] == 'T' && name[1] == 'R' && name[2] == 'A' && name[3] == 'I' &&
            name[4] == 'L' && name[5] == 'E' && name[6] == 'R' && name[7] == '!' &&
            name[8] == '!' && name[9] == '!')
            break;

        if (dec_width(filesz) > max_width)
            max_width = dec_width(filesz);
        file_count++;
    }
    uart_puts("Total ");
    uart_dec(file_count);
    uart_puts(" files.\n");

    cursor = g_rd_start;
    while (cursor < g_rd_end) {
        const char *name;
        unsigned long namesz;
        unsigned long filesz;

        if (!cpio_walk_next(&cursor, 0, &name, 0, &namesz, &filesz)) {
            uart_puts("ls: invalid cpio archive\n");
            return;
        }

        if (namesz >= 11 &&
            name[0] == 'T' && name[1] == 'R' && name[2] == 'A' && name[3] == 'I' &&
            name[4] == 'L' && name[5] == 'E' && name[6] == 'R' && name[7] == '!' &&
            name[8] == '!' && name[9] == '!')
            break;

        uart_dec(filesz);
        for (unsigned long i = dec_width(filesz); i < max_width; i++)
            uart_putc(' ');
        uart_puts("  ");
        uart_puts(name);
        uart_putc('\n');
    }
}

void cpio_cat(const char *filename) {
    const char *cursor;

    if (!cpio_ready()) {
        uart_puts("cat: initrd not set\n");
        return;
    }

    if (!filename || filename[0] == '\0') {
        uart_puts("cat: missing filename\n");
        return;
    }

    cursor = g_rd_start;
    while (cursor < g_rd_end) {
        const char *name;
        const char *data;
        unsigned long namesz;
        unsigned long filesz;
        unsigned long i;

        if (!cpio_walk_next(&cursor, 0, &name, &data, &namesz, &filesz)) {
            uart_puts("cat: invalid cpio archive\n");
            return;
        }

        if (namesz >= 11 &&
            name[0] == 'T' && name[1] == 'R' && name[2] == 'A' && name[3] == 'I' &&
            name[4] == 'L' && name[5] == 'E' && name[6] == 'R' && name[7] == '!' &&
            name[8] == '!' && name[9] == '!')
            break;

        if (!filename_match(name, filename))
            continue;

        for (i = 0; i < filesz; i++)
            uart_putc(data[i]);
        if (filesz == 0 || data[filesz - 1] != '\n')
            uart_putc('\n');
        return;
    }

    uart_puts("cat: file not found: ");
    uart_puts(filename);
    uart_putc('\n');
}

int cpio_find(const char *filename, const void **data, unsigned long *size) {
    const char *cursor;

    if (data)
        *data = 0;
    if (size)
        *size = 0;

    if (!cpio_ready() || !filename || filename[0] == '\0')
        return 0;

    cursor = g_rd_start;
    while (cursor < g_rd_end) {
        const char *name;
        const char *file_data;
        unsigned long namesz;
        unsigned long filesz;

        if (!cpio_walk_next(&cursor, 0, &name, &file_data, &namesz, &filesz))
            return 0;

        if (namesz >= 11 &&
            name[0] == 'T' && name[1] == 'R' && name[2] == 'A' && name[3] == 'I' &&
            name[4] == 'L' && name[5] == 'E' && name[6] == 'R' && name[7] == '!' &&
            name[8] == '!' && name[9] == '!')
            break;

        if (!filename_match(name, filename))
            continue;

        if (data)
            *data = file_data;
        if (size)
            *size = filesz;
        return 1;
    }

    return 0;
}

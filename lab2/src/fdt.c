#include "fdt.h"

#define FDT_MAGIC 0xd00dfeedU

static const char *fdt_struct_base(const void *fdt) {
    const struct fdt_header *h = (const struct fdt_header *)fdt;
    return (const char *)fdt + fdt_be32(&h->off_dt_struct);
}

static const char *fdt_strings_base(const void *fdt) {
    const struct fdt_header *h = (const struct fdt_header *)fdt;
    return (const char *)fdt + fdt_be32(&h->off_dt_strings);
}

static const char *align4(const char *p) {
    unsigned long x = (unsigned long)p;
    x = (x + 3UL) & ~3UL;
    return (const char *)x;
}

static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int cpy_node_name(char *dst, int dst_sz, const char *name) {
    int i = 0;
    while (name[i] && name[i] != '@' && i < dst_sz - 1) {
        dst[i] = name[i];
        i++;
    }
    dst[i] = '\0';
    return i;
}

unsigned int fdt_be32(const void *p) {
    const unsigned char *b = (const unsigned char *)p;
    return ((unsigned int)b[0] << 24) |
           ((unsigned int)b[1] << 16) |
           ((unsigned int)b[2] << 8) |
           (unsigned int)b[3];
}

unsigned long fdt_be64(const void *p) {
    const unsigned char *b = (const unsigned char *)p;
    unsigned long hi = ((unsigned long)b[0] << 24) |
                       ((unsigned long)b[1] << 16) |
                       ((unsigned long)b[2] << 8) |
                       (unsigned long)b[3];
    unsigned long lo = ((unsigned long)b[4] << 24) |
                       ((unsigned long)b[5] << 16) |
                       ((unsigned long)b[6] << 8) |
                       (unsigned long)b[7];
    return (hi << 32) | lo;
}

int fdt_path_offset(const void *fdt, const char *path) {
    const struct fdt_header *h = (const struct fdt_header *)fdt;
    const char *sb;
    const char *p;
    const char *end;
    char cur[256];
    int depth = 0;
    int saved_len[64];

    if (!fdt || !path)
        return -1;
    if (fdt_be32(&h->magic) != FDT_MAGIC)
        return -1;

    sb = fdt_struct_base(fdt);
    p = sb;
    end = sb + fdt_be32(&h->size_dt_struct);
    cur[0] = '\0';

    while (p + 4 <= end) {
        unsigned int token = fdt_be32(p);
        if (token == FDT_BEGIN_NODE) {
            const char *name = p + 4;
            int old_len = 0;
            int node_off = (int)(p - sb);

            if (depth >= 64)
                return -1;

            while (*(cur + old_len))
                old_len++;
            saved_len[depth] = old_len;

            if (depth == 0 && *name == '\0') {
                cur[0] = '/';
                cur[1] = '\0';
            } else {
                char nm[64];
                int i;
                int nm_len = cpy_node_name(nm, sizeof(nm), name);
                if (!(old_len == 1 && cur[0] == '/')) { // not root(/)
                    if (old_len < (int)sizeof(cur) - 1) {
                        cur[old_len++] = '/';
                        cur[old_len] = '\0';
                    }
                }
                i = 0;
                while (i < nm_len && old_len < (int)sizeof(cur) - 1) {
                    cur[old_len++] = nm[i++];
                }
                cur[old_len] = '\0';
            }

            if (streq(cur, path))
                return node_off;

            depth++;
            while (*name)
                name++;
            p = align4(name + 1);
            continue;
        }

        if (token == FDT_END_NODE) {
            if (depth > 0) {
                depth--;
                cur[saved_len[depth]] = '\0';
            }
            p += 4;
            continue;
        }

        if (token == FDT_PROP) {
            unsigned int len;
            p += 4;
            if (p + 8 > end)
                return -1;
            len = fdt_be32(p);
            p += 8;
            p = align4(p + len);
            continue;
        }

        if (token == FDT_NOP) {
            p += 4;
            continue;
        }

        if (token == FDT_END)
            break;

        return -1;
    }

    return -1;
}

const void *fdt_getprop(const void *fdt, int nodeoffset, const char *name, int *lenp) {
    const struct fdt_header *h = (const struct fdt_header *)fdt;
    const char *sb;
    const char *strs;
    const char *p;
    const char *end;
    int depth = 0;

    if (!fdt || !name || nodeoffset < 0)
        return 0;
    if (fdt_be32(&h->magic) != FDT_MAGIC)
        return 0;

    sb = fdt_struct_base(fdt);
    strs = fdt_strings_base(fdt);
    p = sb + nodeoffset;
    end = sb + fdt_be32(&h->size_dt_struct);

    if (p + 4 > end || fdt_be32(p) != FDT_BEGIN_NODE)
        return 0;

    p += 4;
    while (p < end && *p)
        p++;
    p = align4(p + 1);

    while (p + 4 <= end) {
        unsigned int token = fdt_be32(p);

        if (token == FDT_PROP) {
            unsigned int len;
            unsigned int nameoff;
            const char *propname;
            const char *data;

            p += 4;
            if (p + 8 > end)
                return 0;
            len = fdt_be32(p);
            nameoff = fdt_be32(p + 4);
            data = p + 8;
            propname = strs + nameoff;

            if (depth == 0 && streq(propname, name)) {
                if (lenp)
                    *lenp = (int)len;
                return data;
            }

            p = align4(data + len);
            continue;
        }

        if (token == FDT_BEGIN_NODE) {
            const char *node = p + 4;
            depth++;
            while (node < end && *node)
                node++;
            p = align4(node + 1);
            continue;
        }

        if (token == FDT_END_NODE) {
            if (depth == 0)
                break;
            depth--;
            p += 4;
            continue;
        }

        if (token == FDT_NOP) {
            p += 4;
            continue;
        }

        if (token == FDT_END)
            break;

        return 0;
    }

    return 0;
}

#include "video.h"

#include "vfs.h"
#include "vm.h"

#define FB_WIDTH  1920U
#define FB_HEIGHT 1080U
#define FB_BPP    4U

#ifdef QEMU
#define FB_BASE 0xfe000000UL
#else
#define FB_BASE 0x7f700000UL
#endif

#define CACHE_BLOCK_SIZE 64UL

#ifndef QEMU
static void cbo_flush(unsigned long start) {
    asm volatile("mv a0, %0\n\t"
                 ".word 0x0025200F"
                 :
                 : "r"(start)
                 : "memory", "a0");
}
#endif

static void copy_bytes(void *dst, const void *src, unsigned long len) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    while (len--)
        *d++ = *s++;
}

static void flush_dcache(void *addr, unsigned long len) {
#ifndef QEMU
    unsigned long start = (unsigned long)addr & ~(CACHE_BLOCK_SIZE - 1UL);
    unsigned long end = (unsigned long)addr + len;
    unsigned long line;

    asm volatile("" ::: "memory");
    for (line = start; line < end; line += CACHE_BLOCK_SIZE)
        cbo_flush(line);
    asm volatile("" ::: "memory");
#else
    (void)addr;
    (void)len;
#endif
}

static unsigned char *framebuffer_base(void) {
    return (unsigned char *)phys_to_virt(FB_BASE);
}

void video_init(void) {
}

void video_framebuffer_info(struct framebuffer_info *info) {
    if (!info)
        return;
    info->width = FB_WIDTH;
    info->height = FB_HEIGHT;
    info->bpp = FB_BPP;
}

unsigned long video_framebuffer_size(void) {
    return (unsigned long)FB_WIDTH * (unsigned long)FB_HEIGHT *
           (unsigned long)FB_BPP;
}

int video_framebuffer_write(unsigned long offset, const void *buf,
                            unsigned long len) {
    unsigned long size = video_framebuffer_size();
    unsigned long writable;
    unsigned char *dst;

    if (len && !buf)
        return VFS_EINVAL;
    if (len == 0)
        return 0;
    if (offset >= size)
        return VFS_ENOSPC;

    writable = size - offset;
    if (writable > len)
        writable = len;

    dst = framebuffer_base() + offset;
    copy_bytes(dst, buf, writable);
    flush_dcache(dst, writable);
    return (int)writable;
}

void video_display(unsigned int *bmp_image, unsigned int width,
                   unsigned int height) {
    unsigned int copy_w;
    unsigned int copy_h;
    unsigned int dst_x;
    unsigned int dst_y;
    unsigned int src_x;
    unsigned int src_y;
    unsigned int y;

    if (!bmp_image || width == 0 || height == 0)
        return;

    copy_w = width > FB_WIDTH ? FB_WIDTH : width;
    copy_h = height > FB_HEIGHT ? FB_HEIGHT : height;
    dst_x = (FB_WIDTH - copy_w) / 2U;
    dst_y = (FB_HEIGHT - copy_h) / 2U;
    src_x = (width - copy_w) / 2U;
    src_y = (height - copy_h) / 2U;

    for (y = 0; y < copy_h; y++) {
        unsigned int *src = bmp_image + (src_y + y) * width + src_x;
        unsigned long offset =
            ((unsigned long)(dst_y + y) * FB_WIDTH + dst_x) * FB_BPP;

        video_framebuffer_write(offset, src,
                                (unsigned long)copy_w * FB_BPP);
    }
}

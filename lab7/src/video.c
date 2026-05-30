#include "video.h"

#include "vm.h"

#define FB_WIDTH  1920U
#define FB_HEIGHT 1080U

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

void video_init(void) {
}

void video_display(unsigned int *bmp_image, unsigned int width, unsigned int height) {
    unsigned int *fb = (unsigned int *)phys_to_virt(FB_BASE);
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
        unsigned int x;
        unsigned int *dst = fb + (dst_y + y) * FB_WIDTH + dst_x;
        unsigned int *src = bmp_image + (src_y + y) * width + src_x;

        for (x = 0; x < copy_w; x++)
            dst[x] = src[x];
        flush_dcache(dst, (unsigned long)copy_w * sizeof(unsigned int));
    }
}

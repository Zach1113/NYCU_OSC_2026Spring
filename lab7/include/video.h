#ifndef VIDEO_H
#define VIDEO_H

struct framebuffer_info {
    unsigned int width;
    unsigned int height;
    unsigned int bpp;
};

void video_init(void);
void video_display(unsigned int *bmp_image, unsigned int width, unsigned int height);
void video_framebuffer_info(struct framebuffer_info *info);
unsigned long video_framebuffer_size(void);
int video_framebuffer_write(unsigned long offset, const void *buf, unsigned long len);

#endif

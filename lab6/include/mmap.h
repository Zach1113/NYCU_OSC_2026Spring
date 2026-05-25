#ifndef KERNEL_MMAP_H
#define KERNEL_MMAP_H

#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

#define MAP_ANONYMOUS 0x20
#define MAP_POPULATE  0x8000

struct thread;
struct mmap_region;

unsigned long user_mmap_anonymous(struct thread *t, unsigned long addr,
                                  unsigned long length, int prot, int flags);
int user_mmap_clone(struct thread *dst, const struct thread *src);
int user_mmap_replace_frame(struct thread *t, unsigned long addr,
                            unsigned long old_frame,
                            unsigned long new_frame);
int user_mmap_handle_page_fault(struct thread *t, unsigned long addr,
                                unsigned long cause);
void user_mmap_destroy(struct thread *t);

#endif

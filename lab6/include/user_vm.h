#ifndef USER_VM_H
#define USER_VM_H

struct thread;

int user_address_space_init(struct thread *t, const void *prog,
                            unsigned long size);
int user_stack_handle_page_fault(struct thread *t, unsigned long addr,
                                 unsigned long cause);
int user_image_handle_page_fault(struct thread *t, unsigned long addr,
                                 unsigned long cause);
int user_cow_handle_page_fault(struct thread *t, unsigned long addr,
                               unsigned long cause);
int user_address_space_clone_cow(struct thread *dst, struct thread *src);
void user_address_space_destroy(struct thread *t);

#endif

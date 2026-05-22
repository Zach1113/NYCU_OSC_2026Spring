#ifndef SCHED_H
#define SCHED_H

#include "trap.h"

#define KERNEL_STACK_SIZE 0x8000UL
#define USER_STACK_SIZE   0x8000UL
#define MAX_SIGNALS       32

struct mmap_region;

enum thread_status {
    THREAD_RUNNING,
    THREAD_READY,
    THREAD_WAITING,
    THREAD_SLEEPING,
    THREAD_TERMINATED
};

struct thread_context {
    unsigned long ra;
    unsigned long sp;
    unsigned long s[12];
};

struct thread {
    struct thread_context context;
    int pid;
    int is_user;
    enum thread_status status;
    unsigned long pgd;
    unsigned long pgd_pa;
    unsigned long kernel_stack;
    unsigned long user_stack;
    unsigned long user_image;
    unsigned long user_image_size;
    unsigned long user_image_alloc_size;
    struct thread *next;
    struct thread *parent;
    int waiting_pid;
    unsigned long long wake_time;
    unsigned long signal_handlers[MAX_SIGNALS];
    unsigned long pending_signals;
    int signal_active;
    struct trapframe signal_saved_tf;
    unsigned long signal_stack;
    struct mmap_region *mmap_regions;
};

void scheduler_init(void);
struct thread *thread_create(void (*fn)(void));
struct thread *user_process_create(unsigned long entry);
struct thread *user_process_create_from_file(const char *path);
int user_address_space_init(struct thread *t, const void *prog,
                            unsigned long size);
int user_stack_handle_page_fault(struct thread *t, unsigned long addr,
                                 unsigned long cause);
void user_address_space_destroy(struct thread *t);
struct thread *get_current(void);
void schedule(void);
void thread_exit(void);
void kill_zombies(void);
int scheduler_has_user_processes(void);
int scheduler_foreground_active(void);
void scheduler_thread_test(void);
struct thread *scheduler_find(int pid);
int scheduler_next_pid(void);
void scheduler_enqueue_existing(struct thread *t);
int scheduler_is_idle(struct thread *t);
void scheduler_wake_parent_of(struct thread *child);
void scheduler_copy_bytes(void *dst, const void *src, unsigned long len);
void scheduler_wake_sleepers(unsigned long long now);

#endif

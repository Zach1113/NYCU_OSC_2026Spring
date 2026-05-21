#include "sched.h"

#include "cpio.h"
#include "mm.h"
#include "mmap.h"
#include "uart.h"
#include "vm.h"

#define SSTATUS_SPP  (1UL << 8)
#define SSTATUS_SPIE (1UL << 5)
#define SSTATUS_SUM  (1UL << 18)

static struct thread *g_run_queue;
static struct thread *g_idle_thread;
static int g_next_pid;
static int g_foreground_pid;

extern void switch_to(struct thread *prev, struct thread *next);
extern void ret_from_exception(void);

static unsigned long irq_save(void) {
    unsigned long sstatus;

    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    asm volatile("csrci sstatus, 2");
    return sstatus;
}

static void irq_restore(unsigned long sstatus) {
    asm volatile("csrw sstatus, %0" : : "r"(sstatus));
}

static void irq_enable(void) {
    asm volatile("csrsi sstatus, 2");
}

static void zero_bytes(void *ptr, unsigned long len) {
    unsigned char *p = (unsigned char *)ptr;

    while (len--)
        *p++ = 0;
}

static void copy_bytes(void *dst, const void *src, unsigned long len) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    while (len--)
        *d++ = *s++;
}

static unsigned long align_up(unsigned long value, unsigned long align) {
    return (value + align - 1UL) & ~(align - 1UL);
}


int user_address_space_init(struct thread *t, const void *prog,
                            unsigned long size) {
    unsigned long image_size;

    if (!t || !prog || size == 0)
        return 0;

    image_size = align_up(size, PAGE_SIZE);
    if (image_size == 0)
        image_size = PAGE_SIZE;

    t->pgd = (unsigned long)vm_create_user_pgd();
    if (!t->pgd)
        return 0;
    t->pgd_pa = virt_to_phys(t->pgd);

    t->user_image = (unsigned long)alloc(image_size);
    t->user_stack = (unsigned long)alloc(USER_STACK_SIZE);
    if (!t->user_image || !t->user_stack) {
        user_address_space_destroy(t);
        return 0;
    }

    zero_bytes((void *)t->user_image, image_size);
    zero_bytes((void *)t->user_stack, USER_STACK_SIZE);
    copy_bytes((void *)t->user_image, prog, size);

    if (!map_pages((unsigned long *)t->pgd, 0, image_size,
                   virt_to_phys(t->user_image), PROT_USER_RWX) ||
        !map_pages((unsigned long *)t->pgd, USER_STACK_BASE, USER_STACK_SIZE,
                   virt_to_phys(t->user_stack), PROT_USER_RW)) {
        user_address_space_destroy(t);
        return 0;
    }

    t->user_image_size = size;
    t->user_image_alloc_size = image_size;
    return 1;
}

void user_address_space_destroy(struct thread *t) {
    if (!t)
        return;

    user_mmap_destroy(t);

    if (t->pgd) {
        if (t->signal_stack)
            unmap_pages((unsigned long *)t->pgd, USER_SIGNAL_STACK_BASE,
                        USER_STACK_SIZE);
        if (t->user_stack)
            unmap_pages((unsigned long *)t->pgd, USER_STACK_BASE,
                        USER_STACK_SIZE);
        if (t->user_image)
            unmap_pages((unsigned long *)t->pgd, 0,
                        t->user_image_alloc_size);
    }

    if (t->signal_stack) {
        free((void *)t->signal_stack);
        t->signal_stack = 0;
    }
    if (t->user_stack) {
        free((void *)t->user_stack);
        t->user_stack = 0;
    }
    if (t->user_image) {
        free((void *)t->user_image);
        t->user_image = 0;
    }
    if (t->pgd) {
        vm_free_user_pgd((unsigned long *)t->pgd);
        t->pgd = 0;
        t->pgd_pa = 0;
    }
    t->user_image_size = 0;
    t->user_image_alloc_size = 0;
}

static void enqueue_thread(struct thread *t) {
    if (!g_run_queue) {
        g_run_queue = t;
        t->next = t;
        return;
    }

    t->next = g_run_queue->next;
    g_run_queue->next = t;
}

static int is_live_user_thread(struct thread *t) {
    return t && t->is_user && t->status != THREAD_TERMINATED;
}

struct thread *get_current(void) {
    register struct thread *current asm("tp");
    return current;
}

void scheduler_init(void) {
    static struct thread idle;

    zero_bytes(&idle, sizeof(idle));
    idle.pid = 0;
    idle.status = THREAD_RUNNING;
    idle.waiting_pid = -1;

    g_run_queue = 0;
    g_idle_thread = &idle;
    g_next_pid = 1;
    g_foreground_pid = -1;
    enqueue_thread(&idle);

    asm volatile("mv tp, %0" : : "r"(&idle));
}

static void thread_bootstrap(void) {
    void (*fn)(void) = (void (*)(void))get_current()->context.s[0];

    irq_enable();
    if (fn)
        fn();
    thread_exit();
}

struct thread *thread_create(void (*fn)(void)) {
    unsigned long flags;
    struct thread *t;

    if (!fn)
        return 0;

    t = (struct thread *)alloc(sizeof(*t));
    if (!t)
        return 0;
    zero_bytes(t, sizeof(*t));

    t->kernel_stack = (unsigned long)alloc(KERNEL_STACK_SIZE);
    if (!t->kernel_stack) {
        free(t);
        return 0;
    }

    t->pid = g_next_pid++;
    t->status = THREAD_READY;
    t->waiting_pid = -1;
    t->context.ra = (unsigned long)thread_bootstrap;
    t->context.sp = t->kernel_stack + KERNEL_STACK_SIZE;
    t->context.s[0] = (unsigned long)fn;

    flags = irq_save();
    enqueue_thread(t);
    irq_restore(flags);
    return t;
}

static struct thread *user_process_create_from_image(const void *prog,
                                                     unsigned long size) {
    unsigned long flags;
    struct thread *t;
    struct trapframe *tf;

    if (!prog || size == 0)
        return 0;

    t = (struct thread *)alloc(sizeof(*t));
    if (!t)
        return 0;
    zero_bytes(t, sizeof(*t));

    t->kernel_stack = (unsigned long)alloc(KERNEL_STACK_SIZE);
    if (!t->kernel_stack || !user_address_space_init(t, prog, size)) {
        if (t->kernel_stack)
            free((void *)t->kernel_stack);
        user_address_space_destroy(t);
        free(t);
        return 0;
    }

    t->pid = g_next_pid++;
    t->is_user = 1;
    t->status = THREAD_READY;
    t->parent = get_current();
    t->waiting_pid = -1;

    tf = (struct trapframe *)(t->kernel_stack + KERNEL_STACK_SIZE -
                              sizeof(struct trapframe));
    zero_bytes(tf, sizeof(*tf));
    tf->tp = (unsigned long)t;
    tf->sp = USER_STACK_TOP;
    tf->sepc = 0;
    tf->sstatus = (tf->sstatus & ~SSTATUS_SPP) | SSTATUS_SPIE | SSTATUS_SUM;

    t->context.ra = (unsigned long)ret_from_exception;
    t->context.sp = (unsigned long)tf;

    flags = irq_save();
    enqueue_thread(t);
    irq_restore(flags);
    return t;
}

struct thread *user_process_create(unsigned long entry) {
    return user_process_create_from_image((const void *)entry, PAGE_SIZE);
}

struct thread *user_process_create_from_file(const char *path) {
    const void *prog = 0;
    unsigned long size = 0;
    struct thread *t;

    if (!cpio_find(path, &prog, &size))
        return 0;

    t = user_process_create_from_image(prog, size);
    if (t)
        g_foreground_pid = t->pid;
    return t;
}

static int runnable(struct thread *t) {
    return t && (t->status == THREAD_READY || t->status == THREAD_RUNNING);
}

void schedule(void) {
    unsigned long flags;
    struct thread *prev;
    struct thread *next;
    struct thread *head;

    flags = irq_save();
    prev = get_current();
    if (!prev || !g_run_queue) {
        irq_restore(flags);
        return;
    }

    next = prev->next ? prev->next : g_run_queue;
    head = next;
    while (!runnable(next)) {
        next = next->next;
        if (next == head) {
            next = g_idle_thread;
            break;
        }
    }

    if (!runnable(next))
        next = g_idle_thread;

    if (prev->status == THREAD_RUNNING)
        prev->status = THREAD_READY;
    next->status = THREAD_RUNNING;

    if (prev != next) {
        switch_vm(next->pgd_pa);
        switch_to(prev, next);
    }

    irq_restore(flags);
}

static void wake_waiting_parent(struct thread *child) {
    struct thread *parent = child ? child->parent : 0;

    if (parent && parent->status == THREAD_WAITING &&
        parent->waiting_pid == child->pid) {
        parent->status = THREAD_READY;
        parent->waiting_pid = -1;
    }
}

void thread_exit(void) {
    unsigned long flags;
    struct thread *current = get_current();

    flags = irq_save();
    if (current && current != g_idle_thread) {
        current->status = THREAD_TERMINATED;
        if (current->pid == g_foreground_pid)
            g_foreground_pid = -1;
        wake_waiting_parent(current);
    }
    irq_restore(flags);

    schedule();
    while (1)
        ;
}

void kill_zombies(void) {
    unsigned long flags;
    struct thread *prev;
    struct thread *cur;

    flags = irq_save();
    if (!g_run_queue || !g_idle_thread) {
        irq_restore(flags);
        return;
    }

    prev = g_idle_thread;
    cur = prev->next;
    while (cur != g_idle_thread) {
        if (cur->status == THREAD_TERMINATED &&
            (!cur->parent || cur->parent == g_idle_thread ||
             cur->parent->status == THREAD_TERMINATED)) {
            struct thread *dead = cur;

            prev->next = cur->next;
            if (g_run_queue == dead)
                g_run_queue = prev;
            cur = cur->next;

            if (dead->kernel_stack)
                free((void *)dead->kernel_stack);
            user_address_space_destroy(dead);
            free(dead);
            continue;
        }

        prev = cur;
        cur = cur->next;
    }
    irq_restore(flags);
}

int scheduler_has_user_processes(void) {
    unsigned long flags;
    struct thread *t;

    flags = irq_save();
    if (!g_run_queue) {
        irq_restore(flags);
        return 0;
    }

    t = g_run_queue;
    do {
        if (is_live_user_thread(t)) {
            irq_restore(flags);
            return 1;
        }
        t = t->next;
    } while (t != g_run_queue);

    irq_restore(flags);
    return 0;
}

int scheduler_foreground_active(void) {
    unsigned long flags;
    struct thread *t;
    int pid;

    flags = irq_save();
    pid = g_foreground_pid;
    if (pid < 0) {
        irq_restore(flags);
        return 0;
    }

    t = scheduler_find(pid);
    if (!t || t->status == THREAD_TERMINATED) {
        g_foreground_pid = -1;
        irq_restore(flags);
        return 0;
    }

    irq_restore(flags);
    return 1;
}

static void thread_test_fn(void) {
    int i;

    for (i = 0; i < 5; i++) {
        char line[32];
        unsigned long pid = (unsigned long)get_current()->pid;
        char digits[21];
        int pos = 0;
        int dpos = 0;

        line[pos++] = 'T';
        line[pos++] = 'h';
        line[pos++] = 'r';
        line[pos++] = 'e';
        line[pos++] = 'a';
        line[pos++] = 'd';
        line[pos++] = ' ';
        line[pos++] = 'i';
        line[pos++] = 'd';
        line[pos++] = ':';
        line[pos++] = ' ';
        if (pid == 0)
            digits[dpos++] = '0';
        while (pid > 0) {
            digits[dpos++] = (char)('0' + pid % 10UL);
            pid /= 10UL;
        }
        while (dpos > 0)
            line[pos++] = digits[--dpos];
        line[pos++] = ' ';
        line[pos++] = (char)('0' + i);
        line[pos++] = '\n';
        uart_write_atomic(line, (unsigned long)pos);
        for (volatile int j = 0; j < 100000000; j++)
            ;
        schedule();
    }
}

void scheduler_thread_test(void) {
    int i;
    int pid[3];

    for (i = 0; i < 3; i++) {
        struct thread *t = thread_create(thread_test_fn);

        if (!t) {
            uart_puts("threadtest: thread_create failed\n");
            pid[i] = -1;
            continue;
        }
        pid[i] = t->pid;
    }

    while (1) {
        int live = 0;

        for (i = 0; i < 3; i++) {
            struct thread *t;

            if (pid[i] < 0)
                continue;
            t = scheduler_find(pid[i]);
            if (t && t->status != THREAD_TERMINATED) {
                live = 1;
                break;
            }
        }

        if (!live)
            break;
        schedule();
    }
}

struct thread *scheduler_find(int pid) {
    struct thread *t;

    if (!g_run_queue)
        return 0;

    t = g_run_queue;
    do {
        if (t->pid == pid)
            return t;
        t = t->next;
    } while (t != g_run_queue);

    return 0;
}

int scheduler_next_pid(void) {
    return g_next_pid++;
}

void scheduler_enqueue_existing(struct thread *t) {
    unsigned long flags;

    flags = irq_save();
    enqueue_thread(t);
    irq_restore(flags);
}

int scheduler_is_idle(struct thread *t) {
    return t == g_idle_thread;
}

void scheduler_wake_parent_of(struct thread *child) {
    wake_waiting_parent(child);
}

void scheduler_copy_bytes(void *dst, const void *src, unsigned long len) {
    copy_bytes(dst, src, len);
}

void scheduler_wake_sleepers(unsigned long long now) {
    unsigned long flags;
    struct thread *t;

    flags = irq_save();
    if (!g_run_queue) {
        irq_restore(flags);
        return;
    }

    t = g_run_queue;
    do {
        if (t->status == THREAD_SLEEPING && now >= t->wake_time) {
            t->status = THREAD_READY;
            t->wake_time = 0;
        }
        t = t->next;
    } while (t != g_run_queue);

    irq_restore(flags);
}

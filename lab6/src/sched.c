#include "sched.h"

#include "cpio.h"
#include "mm.h"
#include "mmap.h"
#include "uart.h"
#include "vm.h"

#define SSTATUS_SPP  (1UL << 8)
#define SSTATUS_SPIE (1UL << 5)
#define SSTATUS_SUM  (1UL << 18)
#define SCAUSE_LOAD_PAGE_FAULT  13UL
#define SCAUSE_STORE_PAGE_FAULT 15UL

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


static unsigned long align_down(unsigned long value, unsigned long align) {
    return value & ~(align - 1UL);
}

static int stack_fault_allowed(unsigned long cause) {
    return cause == SCAUSE_LOAD_PAGE_FAULT ||
           cause == SCAUSE_STORE_PAGE_FAULT;
}

static void log_translation_fault(unsigned long addr) {
    uart_puts("[Translation fault]: ");
    uart_hex(addr);
    uart_putc('\n');
}

static void log_permission_fault(unsigned long addr) {
    uart_puts("[Permission fault]: ");
    uart_hex(addr);
    uart_putc('\n');
}

static unsigned long min_ulong(unsigned long a, unsigned long b) {
    return a < b ? a : b;
}

static unsigned long alloc_zero_page(void) {
    unsigned long frame = (unsigned long)alloc(PAGE_SIZE);

    if (frame)
        zero_bytes((void *)frame, PAGE_SIZE);
    return frame;
}

static int map_tracked_page(struct thread *t, unsigned long va,
                            unsigned long frame, unsigned long prot) {
    return map_pages((unsigned long *)t->pgd, va, PAGE_SIZE,
                     virt_to_phys(frame), prot);
}

static int pte_make_cow(unsigned long *pte) {
    if (!pte || !(*pte & PTE_V) || !(*pte & PTE_U))
        return 0;
    if (*pte & PTE_W)
        *pte = (*pte & ~PTE_W) | PTE_COW;
    return 1;
}

static int share_user_page(struct thread *dst, struct thread *src,
                           unsigned long va, unsigned long frame) {
    unsigned long *src_pte;
    unsigned long flags;

    if (!frame)
        return 1;

    src_pte = vm_get_pte((unsigned long *)src->pgd, va, 0);
    if (!src_pte || !(*src_pte & PTE_V))
        return 0;

    pte_make_cow(src_pte);
    flags = *src_pte & PTE_FLAGS_MASK;
    mm_page_get((void *)frame);
    if (!map_pages((unsigned long *)dst->pgd, va, PAGE_SIZE,
                   virt_to_phys(frame), flags)) {
        mm_page_put((void *)frame);
        return 0;
    }
    return 1;
}

static int user_replace_owned_frame(struct thread *t, unsigned long va,
                                    unsigned long old_frame,
                                    unsigned long new_frame) {
    unsigned long idx;

    if (va < t->user_image_alloc_size) {
        idx = va / PAGE_SIZE;
        if (idx < t->user_image_page_count &&
            t->user_image_pages[idx] == old_frame) {
            t->user_image_pages[idx] = new_frame;
            return 1;
        }
        return 0;
    }

    if (va >= USER_STACK_BASE && va < USER_STACK_TOP) {
        idx = (va - USER_STACK_BASE) / PAGE_SIZE;
        if (idx < t->user_stack_page_count &&
            t->user_stack_pages[idx] == old_frame) {
            t->user_stack_pages[idx] = new_frame;
            return 1;
        }
        return 0;
    }

    return user_mmap_replace_frame(t, va, old_frame, new_frame);
}

int user_stack_handle_page_fault(struct thread *t, unsigned long addr,
                                 unsigned long cause) {
    unsigned long va;
    unsigned long page_index;
    unsigned long frame;

    if (!t || !t->pgd || !t->user_stack_pages || !stack_fault_allowed(cause))
        return 0;
    if (addr < USER_STACK_BASE || addr >= USER_STACK_TOP)
        return 0;

    va = align_down(addr, PAGE_SIZE);
    if (vm_translate((unsigned long *)t->pgd, va))
        return 0;

    page_index = (va - USER_STACK_BASE) / PAGE_SIZE;
    if (page_index >= t->user_stack_page_count)
        return 0;

    frame = t->user_stack_pages[page_index];
    if (!frame) {
        frame = alloc_zero_page();
        if (!frame)
            return 0;
        t->user_stack_pages[page_index] = frame;
    }

    if (!map_tracked_page(t, va, frame, PROT_USER_RW)) {
        if (t->user_stack_pages[page_index] == frame) {
            t->user_stack_pages[page_index] = 0;
            mm_page_put((void *)frame);
        }
        return 0;
    }

    log_translation_fault(addr);
    return 1;
}

int user_cow_handle_page_fault(struct thread *t, unsigned long addr,
                               unsigned long cause) {
    unsigned long va;
    unsigned long *pte;
    unsigned long old_frame;
    unsigned long new_frame;
    unsigned long flags;

    if (!t || !t->pgd || cause != SCAUSE_STORE_PAGE_FAULT)
        return 0;

    va = align_down(addr, PAGE_SIZE);
    pte = vm_get_pte((unsigned long *)t->pgd, va, 0);
    if (!pte || !(*pte & PTE_V) || !(*pte & PTE_COW))
        return 0;

    old_frame = phys_to_virt(vm_pte_pa(*pte));
    log_permission_fault(addr);

    if (mm_page_ref_count((void *)old_frame) <= 1) {
        *pte = (*pte | PTE_W) & ~PTE_COW;
        vm_flush_tlb();
        return 1;
    }

    new_frame = (unsigned long)alloc(PAGE_SIZE);
    if (!new_frame)
        return 0;
    copy_bytes((void *)new_frame, (const void *)old_frame, PAGE_SIZE);

    if (!user_replace_owned_frame(t, va, old_frame, new_frame)) {
        free((void *)new_frame);
        return 0;
    }

    flags = ((*pte & PTE_FLAGS_MASK) | PTE_W) & ~PTE_COW;
    *pte = vm_make_pte(virt_to_phys(new_frame), flags);
    mm_page_put((void *)old_frame);
    vm_flush_tlb();
    return 1;
}

int user_address_space_init(struct thread *t, const void *prog,
                            unsigned long size) {
    unsigned long image_size;
    unsigned long i;
    unsigned long stack_top_index;

    if (!t || !prog || size == 0)
        return 0;

    image_size = align_up(size, PAGE_SIZE);
    if (image_size == 0)
        image_size = PAGE_SIZE;

    t->pgd = (unsigned long)vm_create_user_pgd();
    if (!t->pgd)
        return 0;
    t->pgd_pa = virt_to_phys(t->pgd);

    t->user_image_size = size;
    t->user_image_alloc_size = image_size;
    t->user_image_page_count = image_size / PAGE_SIZE;
    t->user_stack_page_count = USER_STACK_SIZE / PAGE_SIZE;

    t->user_image_pages = (unsigned long *)alloc(t->user_image_page_count *
                                                 sizeof(unsigned long));
    t->user_stack_pages = (unsigned long *)alloc(t->user_stack_page_count *
                                                 sizeof(unsigned long));
    if (!t->user_image_pages || !t->user_stack_pages) {
        user_address_space_destroy(t);
        return 0;
    }
    zero_bytes(t->user_image_pages,
               t->user_image_page_count * sizeof(unsigned long));
    zero_bytes(t->user_stack_pages,
               t->user_stack_page_count * sizeof(unsigned long));

    for (i = 0; i < t->user_image_page_count; i++) {
        unsigned long frame = alloc_zero_page();
        unsigned long copied = i * PAGE_SIZE;
        unsigned long copy_len = 0;

        if (!frame) {
            user_address_space_destroy(t);
            return 0;
        }
        if (copied < size)
            copy_len = min_ulong(PAGE_SIZE, size - copied);
        if (copy_len)
            copy_bytes((void *)frame, (const char *)prog + copied, copy_len);
        t->user_image_pages[i] = frame;
        if (!map_tracked_page(t, i * PAGE_SIZE, frame, PROT_USER_RWX)) {
            user_address_space_destroy(t);
            return 0;
        }
    }

    stack_top_index = t->user_stack_page_count - 1;
    t->user_stack_pages[stack_top_index] = alloc_zero_page();
    if (!t->user_stack_pages[stack_top_index] ||
        !map_tracked_page(t, USER_STACK_TOP - PAGE_SIZE,
                          t->user_stack_pages[stack_top_index],
                          PROT_USER_RW)) {
        user_address_space_destroy(t);
        return 0;
    }

    t->user_image = t->user_image_pages[0];
    t->user_stack = t->user_stack_pages[0];
    return 1;
}

int user_address_space_clone_cow(struct thread *dst, struct thread *src) {
    unsigned long i;

    if (!dst || !src || !src->pgd)
        return 0;

    dst->pgd = (unsigned long)vm_create_user_pgd();
    if (!dst->pgd)
        return 0;
    dst->pgd_pa = virt_to_phys(dst->pgd);

    dst->user_image_size = src->user_image_size;
    dst->user_image_alloc_size = src->user_image_alloc_size;
    dst->user_image_page_count = src->user_image_page_count;
    dst->user_stack_page_count = src->user_stack_page_count;

    dst->user_image_pages = (unsigned long *)alloc(dst->user_image_page_count *
                                                   sizeof(unsigned long));
    dst->user_stack_pages = (unsigned long *)alloc(dst->user_stack_page_count *
                                                   sizeof(unsigned long));
    if (!dst->user_image_pages || !dst->user_stack_pages) {
        user_address_space_destroy(dst);
        return 0;
    }
    zero_bytes(dst->user_image_pages,
               dst->user_image_page_count * sizeof(unsigned long));
    zero_bytes(dst->user_stack_pages,
               dst->user_stack_page_count * sizeof(unsigned long));

    for (i = 0; i < src->user_image_page_count; i++) {
        unsigned long frame = src->user_image_pages[i];

        if (!frame)
            continue;
        if (!share_user_page(dst, src, i * PAGE_SIZE, frame)) {
            user_address_space_destroy(dst);
            return 0;
        }
        dst->user_image_pages[i] = frame;
    }

    for (i = 0; i < src->user_stack_page_count; i++) {
        unsigned long frame = src->user_stack_pages[i];
        unsigned long va = USER_STACK_BASE + i * PAGE_SIZE;

        if (!frame)
            continue;
        if (!share_user_page(dst, src, va, frame)) {
            user_address_space_destroy(dst);
            return 0;
        }
        dst->user_stack_pages[i] = frame;
    }

    if (!user_mmap_clone(dst, src)) {
        user_address_space_destroy(dst);
        return 0;
    }

    dst->user_image = dst->user_image_page_count ? dst->user_image_pages[0] : 0;
    dst->user_stack = dst->user_stack_page_count ? dst->user_stack_pages[0] : 0;
    vm_flush_tlb();
    return 1;
}

void user_address_space_destroy(struct thread *t) {
    unsigned long i;

    if (!t)
        return;

    user_mmap_destroy(t);

    if (t->pgd) {
        if (t->signal_stack)
            unmap_pages((unsigned long *)t->pgd, USER_SIGNAL_STACK_BASE,
                        USER_STACK_SIZE);
        if (t->user_stack_pages)
            unmap_pages((unsigned long *)t->pgd, USER_STACK_BASE,
                        USER_STACK_SIZE);
        if (t->user_image_pages)
            unmap_pages((unsigned long *)t->pgd, 0,
                        t->user_image_alloc_size);
    }

    if (t->signal_stack) {
        free((void *)t->signal_stack);
        t->signal_stack = 0;
    }

    if (t->user_stack_pages) {
        for (i = 0; i < t->user_stack_page_count; i++) {
            if (t->user_stack_pages[i])
                mm_page_put((void *)t->user_stack_pages[i]);
        }
        free(t->user_stack_pages);
        t->user_stack_pages = 0;
    }

    if (t->user_image_pages) {
        for (i = 0; i < t->user_image_page_count; i++) {
            if (t->user_image_pages[i])
                mm_page_put((void *)t->user_image_pages[i]);
        }
        free(t->user_image_pages);
        t->user_image_pages = 0;
    }

    if (t->pgd) {
        vm_free_user_pgd((unsigned long *)t->pgd);
        t->pgd = 0;
        t->pgd_pa = 0;
    }
    t->user_stack = 0;
    t->user_image = 0;
    t->user_image_size = 0;
    t->user_image_alloc_size = 0;
    t->user_image_page_count = 0;
    t->user_stack_page_count = 0;
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

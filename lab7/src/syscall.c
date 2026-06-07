#include "syscall.h"

#include "cpio.h"
#include "mm.h"
#include "mmap.h"
#include "sched.h"
#include "timer.h"
#include "uart.h"
#include "user_vm.h"
#include "video.h"
#include "vfs.h"
#include "vm.h"

#define SYS_GETPID     0UL
#define SYS_UART_READ  1UL
#define SYS_UART_WRITE 2UL
#define SYS_EXEC       3UL
#define SYS_FORK       4UL
#define SYS_WAITPID    5UL
#define SYS_EXIT       6UL
#define SYS_STOP       7UL
#define SYS_DISPLAY    8UL
#define SYS_USLEEP     9UL
#define SYS_SIGNAL     10UL
#define SYS_SIGRETURN  11UL
#define SYS_KILL       12UL
#define SYS_MMAP       13UL
#define SYS_OPEN       14UL
#define SYS_CLOSE      15UL
#define SYS_READ       16UL
#define SYS_WRITE      17UL
#define SYS_MKDIR      18UL
#define SYS_MOUNT      19UL
#define SYS_CHDIR      20UL

#define SSTATUS_SPP    (1UL << 8)
#define SIGRETURN_INST_ADD_A7 0x00b00893U
#define SIGRETURN_INST_ECALL  0x00000073U
#define SIGRETURN_INST_LOOP   0x0000006fU

extern void ret_from_exception(void);

static int g_uart_line_owner = -1;

static void uart_release_owner(int pid);

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

static int valid_signum(int signum) {
    return signum > 0 && signum < MAX_SIGNALS;
}

static void clear_signal_runtime(struct thread *t) {
    if (!t)
        return;

    t->pending_signals = 0;
    t->signal_active = 0;
    zero_bytes(&t->signal_saved_tf, sizeof(t->signal_saved_tf));
    if (t->signal_stack) {
        if (t->pgd)
            unmap_pages((unsigned long *)t->pgd, USER_SIGNAL_STACK_BASE,
                        USER_STACK_SIZE);
        free((void *)t->signal_stack);
        t->signal_stack = 0;
    }
}

static int next_pending_signal(struct thread *t) {
    int signum;

    if (!t)
        return 0;

    for (signum = 1; signum < MAX_SIGNALS; signum++) {
        if (t->pending_signals & (1UL << signum))
            return signum;
    }
    return 0;
}

static void make_signal_target_runnable(struct thread *t) {
    if (!t)
        return;

    if (t->status == THREAD_SLEEPING || t->status == THREAD_WAITING) {
        t->status = THREAD_READY;
        t->wake_time = 0;
        t->waiting_pid = -1;
    }
}

static void terminate_thread_for_signal(struct thread *t) {
    if (!t || !t->is_user || scheduler_is_idle(t))
        return;

    uart_release_owner(t->pid);
    vfs_thread_cleanup(t);
    t->status = THREAD_TERMINATED;
    scheduler_wake_parent_of(t);
    if (t == get_current())
        thread_exit();
}

static void syscall_getpid(struct trapframe *tf) {
    tf->a0 = (unsigned long)get_current()->pid;
    tf->sepc += 4;
}

static int uart_write_completes_line(const char *buf, unsigned long count) {
    unsigned long i;

    if (count == 2 && buf[0] == '$' && buf[1] == ' ')
        return 1;

    for (i = 0; i < count; i++) {
        if (buf[i] == '\n' || buf[i] == '\r')
            return 1;
    }
    return 0;
}

static void uart_release_owner(int pid) {
    unsigned long flags = irq_save();

    if (g_uart_line_owner == pid)
        g_uart_line_owner = -1;
    irq_restore(flags);
}

static void uart_wait_for_line_owner(int pid) {
    while (1) {
        unsigned long flags = irq_save();
        struct thread *owner;

        if (g_uart_line_owner == -1 || g_uart_line_owner == pid) {
            g_uart_line_owner = pid;
            irq_restore(flags);
            return;
        }

        owner = scheduler_find(g_uart_line_owner);
        if (!owner || owner->status == THREAD_TERMINATED) {
            g_uart_line_owner = pid;
            irq_restore(flags);
            return;
        }

        irq_restore(flags);
        irq_enable();
        schedule();
    }
}

static void syscall_uart_read(struct trapframe *tf) {
    char *buf = (char *)tf->a0;
    long count = (long)tf->a1;
    long i;
    int pid = get_current()->pid;

    if (!buf || count < 0) {
        tf->a0 = (unsigned long)-1L;
        tf->sepc += 4;
        return;
    }

    /*
     * User shells often print prompts without a trailing newline before
     * blocking for input. Do not let such prompt fragments keep the
     * line-output lock and starve child process output/video startup.
     */
    uart_release_owner(pid);

    for (i = 0; i < count; i++) {
        char c;

        while (!uart_getc_nonblock(&c)) {
            irq_enable();
            schedule();
        }
        buf[i] = c;
    }

    tf->a0 = (unsigned long)count;
    tf->sepc += 4;
}

static void syscall_uart_write(struct trapframe *tf) {
    const char *buf = (const char *)tf->a0;
    long count = (long)tf->a1;
    int pid = get_current()->pid;

    if (!buf || count < 0) {
        tf->a0 = (unsigned long)-1L;
        tf->sepc += 4;
        return;
    }

    if (count == 0) {
        tf->a0 = 0;
        tf->sepc += 4;
        return;
    }

    uart_wait_for_line_owner(pid);
    uart_write_atomic(buf, (unsigned long)count);
    if (uart_write_completes_line(buf, (unsigned long)count))
        uart_release_owner(pid);

    tf->a0 = (unsigned long)count;
    tf->sepc += 4;
}

static void syscall_exec(struct trapframe *tf) {
    const char *path = (const char *)tf->a0;
    const void *prog = 0;
    unsigned long size = 0;
    struct thread *current = get_current();
    struct thread next_space;

    if (!path || !cpio_find(path, &prog, &size)) {
        tf->a0 = (unsigned long)-1L;
        tf->sepc += 4;
        return;
    }

    zero_bytes(&next_space, sizeof(next_space));
    if (!user_address_space_init(&next_space, prog, size)) {
        tf->a0 = (unsigned long)-1L;
        tf->sepc += 4;
        return;
    }

    switch_vm(next_space.pgd_pa);
    user_address_space_destroy(current);
    clear_signal_runtime(current);
    current->pgd = next_space.pgd;
    current->pgd_pa = next_space.pgd_pa;
    current->user_image = next_space.user_image;
    current->user_image_size = next_space.user_image_size;
    current->user_image_alloc_size = next_space.user_image_alloc_size;
    current->user_image_pages = next_space.user_image_pages;
    current->user_image_page_count = next_space.user_image_page_count;
    current->user_stack = next_space.user_stack;
    current->user_stack_pages = next_space.user_stack_pages;
    current->user_stack_page_count = next_space.user_stack_page_count;
    current->mmap_regions = next_space.mmap_regions;

    zero_bytes(tf, sizeof(*tf));
    tf->tp = (unsigned long)current;
    tf->sp = USER_STACK_TOP;
    tf->sepc = 0;
    tf->sstatus = (1UL << 5) | (1UL << 18);
    tf->a0 = 0;
}

static void syscall_fork(struct trapframe *tf) {
    struct thread *parent = get_current();
    struct thread *child;
    struct trapframe *child_tf;
    unsigned long kernel_offset;

    child = (struct thread *)alloc(sizeof(*child));
    if (!child) {
        tf->a0 = (unsigned long)-1L;
        tf->sepc += 4;
        return;
    }

    scheduler_copy_bytes(child, parent, sizeof(*child));
    child->kernel_stack = (unsigned long)alloc(KERNEL_STACK_SIZE);
    child->pgd = 0;
    child->pgd_pa = 0;
    child->user_image = 0;
    child->user_image_size = 0;
    child->user_image_alloc_size = 0;
    child->user_image_pages = 0;
    child->user_image_page_count = 0;
    child->user_stack = 0;
    child->user_stack_pages = 0;
    child->user_stack_page_count = 0;
    child->signal_stack = 0;
    child->mmap_regions = 0;
    if (!child->kernel_stack || !user_address_space_clone_cow(child, parent)) {
        if (child->kernel_stack)
            free((void *)child->kernel_stack);
        user_address_space_destroy(child);
        free(child);
        tf->a0 = (unsigned long)-1L;
        tf->sepc += 4;
        return;
    }

    scheduler_copy_bytes((void *)child->kernel_stack,
                         (const void *)parent->kernel_stack,
                         KERNEL_STACK_SIZE);

    child->pid = scheduler_next_pid();
    child->status = THREAD_READY;
    child->parent = parent;
    child->waiting_pid = -1;
    child->pending_signals = 0;
    child->signal_active = 0;
    child->signal_stack = 0;
    zero_bytes(&child->signal_saved_tf, sizeof(child->signal_saved_tf));
    vfs_thread_clone(child, parent);

    kernel_offset = child->kernel_stack - parent->kernel_stack;
    child_tf = (struct trapframe *)((unsigned long)tf + kernel_offset);

    child_tf->a0 = 0;
    child_tf->tp = (unsigned long)child;
    child_tf->sepc += 4;

    child->context.ra = (unsigned long)ret_from_exception;
    child->context.sp = (unsigned long)child_tf;
    child->next = 0;

    scheduler_enqueue_existing(child);

    tf->a0 = (unsigned long)child->pid;
    tf->sepc += 4;
}

static void syscall_waitpid(struct trapframe *tf) {
    int pid = (int)tf->a0;
    struct thread *current = get_current();
    struct thread *target = scheduler_find(pid);

    if (!target || scheduler_is_idle(target)) {
        tf->a0 = (unsigned long)-1L;
        tf->sepc += 4;
        return;
    }

    if (target->status != THREAD_TERMINATED) {
        current->status = THREAD_WAITING;
        current->waiting_pid = pid;
        schedule();
    }

    target = scheduler_find(pid);
    if (target && target->status == THREAD_TERMINATED)
        target->parent = 0;

    tf->a0 = (unsigned long)pid;
    tf->sepc += 4;
}

static void syscall_exit(struct trapframe *tf) {
    (void)tf;
    uart_release_owner(get_current()->pid);
    thread_exit();
}

static void syscall_stop(struct trapframe *tf) {
    int pid = (int)tf->a0;
    struct thread *target = scheduler_find(pid);

    if (!target || scheduler_is_idle(target)) {
        tf->a0 = (unsigned long)-1L;
        tf->sepc += 4;
        return;
    }

    uart_release_owner(target->pid);
    vfs_thread_cleanup(target);
    target->status = THREAD_TERMINATED;
    scheduler_wake_parent_of(target);
    if (target == get_current())
        thread_exit();

    tf->a0 = 0;
    tf->sepc += 4;
}

static void syscall_display(struct trapframe *tf) {
    irq_enable();
    video_display((unsigned int *)tf->a0,
                  (unsigned int)tf->a1,
                  (unsigned int)tf->a2);
    tf->sepc += 4;
    schedule();
}

static unsigned long long usec_to_ticks(unsigned int usec) {
    unsigned long long hz = timer_timebase_hz();
    unsigned long long whole = (unsigned long long)(usec / 1000000U) * hz;
    unsigned long long frac = ((unsigned long long)(usec % 1000000U) * hz) /
                              1000000ULL;

    return whole + frac;
}

static void syscall_usleep(struct trapframe *tf) {
    unsigned int usec = (unsigned int)tf->a0;
    unsigned long long wait_ticks = usec_to_ticks(usec);
    unsigned long long start = timer_now();
    struct thread *current = get_current();

    if (usec == 0) {
        tf->a0 = 0;
        tf->sepc += 4;
        return;
    }

    if (wait_ticks == 0)
        wait_ticks = 1;

    while (timer_now() - start < wait_ticks && !current->pending_signals) {
        current->wake_time = start + wait_ticks;
        current->status = THREAD_SLEEPING;
        schedule();
    }

    tf->a0 = 0;
    tf->sepc += 4;
}

static void syscall_signal(struct trapframe *tf) {
    int signum = (int)tf->a0;
    unsigned long handler = tf->a1;
    struct thread *current = get_current();
    unsigned long old_handler;

    if (!valid_signum(signum) || !current || !current->is_user) {
        tf->a0 = (unsigned long)-1L;
        tf->sepc += 4;
        return;
    }

    old_handler = current->signal_handlers[signum];
    current->signal_handlers[signum] = handler;

    tf->a0 = old_handler;
    tf->sepc += 4;
}

static void syscall_sigreturn(struct trapframe *tf) {
    struct thread *current = get_current();
    unsigned long signal_stack;

    if (!current || !current->is_user || !current->signal_active) {
        tf->a0 = (unsigned long)-1L;
        tf->sepc += 4;
        return;
    }

    uart_puts("[Signal] sigreturn\n");

    signal_stack = current->signal_stack;
    scheduler_copy_bytes(tf, &current->signal_saved_tf, sizeof(*tf));
    current->signal_active = 0;
    current->signal_stack = 0;
    zero_bytes(&current->signal_saved_tf, sizeof(current->signal_saved_tf));

    if (signal_stack) {
        if (current->pgd)
            unmap_pages((unsigned long *)current->pgd, USER_SIGNAL_STACK_BASE,
                        USER_STACK_SIZE);
        free((void *)signal_stack);
    }
}

static void syscall_kill(struct trapframe *tf) {
    int pid = (int)tf->a0;
    int signum = (int)tf->a1;
    struct thread *target = scheduler_find(pid);

    if (!valid_signum(signum) || !target || !target->is_user ||
        scheduler_is_idle(target) || target->status == THREAD_TERMINATED) {
        tf->a0 = (unsigned long)-1L;
        tf->sepc += 4;
        return;
    }

    if (target->signal_handlers[signum]) {
        target->pending_signals |= 1UL << signum;
        make_signal_target_runnable(target);
    } else {
        terminate_thread_for_signal(target);
    }

    tf->a0 = 0;
    tf->sepc += 4;
}

static void syscall_mmap(struct trapframe *tf) {
    unsigned long addr = tf->a0;
    unsigned long length = tf->a1;
    int prot = (int)tf->a2;
    int flags = (int)tf->a3;
    unsigned long mapped;

    mapped = user_mmap_anonymous(get_current(), addr, length, prot, flags);
    tf->a0 = mapped ? mapped : (unsigned long)-1L;
    tf->sepc += 4;
}


static int copy_user_path(char *dst, const char *src) {
    unsigned long i;

    if (!dst || !src)
        return VFS_EINVAL;
    for (i = 0; i <= VFS_MAX_PATH; i++) {
        dst[i] = src[i];
        if (src[i] == 0)
            return 0;
    }
    dst[VFS_MAX_PATH] = 0;
    return VFS_ENAMETOOLONG;
}

static int fd_alloc(struct thread *t, struct file *file) {
    int fd;

    if (!t || !file)
        return VFS_EINVAL;
    for (fd = 0; fd < VFS_MAX_FD; fd++) {
        if (!t->fd_table[fd]) {
            t->fd_table[fd] = file;
            return fd;
        }
    }
    return VFS_ENFILE;
}

static struct file *fd_get(struct thread *t, int fd) {
    if (!t || fd < 0 || fd >= VFS_MAX_FD)
        return 0;
    return t->fd_table[fd];
}

static void syscall_open(struct trapframe *tf) {
    char path[VFS_MAX_PATH + 1];
    struct file *file = 0;
    int ret;

    ret = copy_user_path(path, (const char *)tf->a0);
    if (ret == 0)
        ret = vfs_open(path, (int)tf->a1, &file);
    if (ret == 0) {
        ret = fd_alloc(get_current(), file);
        if (ret < 0)
            vfs_close(file);
    }
    tf->a0 = (unsigned long)(long)ret;
    tf->sepc += 4;
}

static void syscall_close_fd(struct trapframe *tf) {
    int fd = (int)tf->a0;
    struct thread *current = get_current();
    struct file *file = fd_get(current, fd);
    int ret;

    if (!file) {
        tf->a0 = (unsigned long)(long)VFS_EBADF;
        tf->sepc += 4;
        return;
    }

    current->fd_table[fd] = 0;
    ret = vfs_close(file);
    tf->a0 = (unsigned long)(long)ret;
    tf->sepc += 4;
}

static void syscall_read_fd(struct trapframe *tf) {
    int fd = (int)tf->a0;
    void *buf = (void *)tf->a1;
    unsigned long count = tf->a2;
    struct file *file = fd_get(get_current(), fd);
    int ret;

    if (!file)
        ret = VFS_EBADF;
    else
        ret = vfs_read(file, buf, count);
    tf->a0 = (unsigned long)(long)ret;
    tf->sepc += 4;
}

static void syscall_write_fd(struct trapframe *tf) {
    int fd = (int)tf->a0;
    const void *buf = (const void *)tf->a1;
    unsigned long count = tf->a2;
    struct file *file = fd_get(get_current(), fd);
    int ret;

    if (!file)
        ret = VFS_EBADF;
    else
        ret = vfs_write(file, buf, count);
    tf->a0 = (unsigned long)(long)ret;
    tf->sepc += 4;
}

static void syscall_mkdir_fd(struct trapframe *tf) {
    char path[VFS_MAX_PATH + 1];
    int ret;

    (void)tf->a1;
    ret = copy_user_path(path, (const char *)tf->a0);
    if (ret == 0)
        ret = vfs_mkdir(path);
    tf->a0 = (unsigned long)(long)ret;
    tf->sepc += 4;
}

static void syscall_mount_fd(struct trapframe *tf) {
    char target[VFS_MAX_PATH + 1];
    char filesystem[VFS_MAX_PATH + 1];
    int ret;

    (void)tf->a0;
    (void)tf->a3;
    (void)tf->a4;
    ret = copy_user_path(target, (const char *)tf->a1);
    if (ret == 0)
        ret = copy_user_path(filesystem, (const char *)tf->a2);
    if (ret == 0)
        ret = vfs_mount(target, filesystem);
    tf->a0 = (unsigned long)(long)ret;
    tf->sepc += 4;
}

static void syscall_chdir_fd(struct trapframe *tf) {
    char path[VFS_MAX_PATH + 1];
    int ret;

    ret = copy_user_path(path, (const char *)tf->a0);
    if (ret == 0)
        ret = vfs_chdir(path);
    tf->a0 = (unsigned long)(long)ret;
    tf->sepc += 4;
}

void signal_deliver(struct trapframe *tf) {
    struct thread *current = get_current();
    unsigned int *trampoline;
    unsigned long stack;
    int signum;

    if (!tf || !current || !current->is_user)
        return;
    if (tf->sstatus & SSTATUS_SPP)
        return;
    if (current->signal_active)
        return;

    signum = next_pending_signal(current);
    if (!signum)
        return;

    if (!current->signal_handlers[signum]) {
        current->pending_signals &= ~(1UL << signum);
        terminate_thread_for_signal(current);
        return;
    }

    stack = (unsigned long)alloc(USER_STACK_SIZE);
    if (!stack)
        return;
    zero_bytes((void *)stack, USER_STACK_SIZE);
    if (!map_pages((unsigned long *)current->pgd, USER_SIGNAL_STACK_BASE,
                   USER_STACK_SIZE, virt_to_phys(stack), PROT_USER_RWX)) {
        free((void *)stack);
        return;
    }

    current->pending_signals &= ~(1UL << signum);
    current->signal_active = 1;
    current->signal_stack = stack;
    scheduler_copy_bytes(&current->signal_saved_tf, tf, sizeof(*tf));

    trampoline = (unsigned int *)stack;
    trampoline[0] = SIGRETURN_INST_ADD_A7; // addi a7, zero, 11
    trampoline[1] = SIGRETURN_INST_ECALL;  // ecall
    trampoline[2] = SIGRETURN_INST_LOOP;   // jal zero, 0
    asm volatile("fence.i" ::: "memory");

    tf->ra = USER_SIGNAL_STACK_BASE;
    tf->sp = USER_SIGNAL_STACK_TOP;
    tf->sepc = current->signal_handlers[signum];
    tf->a0 = (unsigned long)signum;
    tf->tp = (unsigned long)current;
}

void syscall_handle(struct trapframe *tf) {
    switch (tf->a7) {
    case SYS_GETPID:
        syscall_getpid(tf);
        break;
    case SYS_UART_READ:
        syscall_uart_read(tf);
        break;
    case SYS_UART_WRITE:
        syscall_uart_write(tf);
        break;
    case SYS_EXEC:
        syscall_exec(tf);
        break;
    case SYS_FORK:
        syscall_fork(tf);
        break;
    case SYS_WAITPID:
        syscall_waitpid(tf);
        break;
    case SYS_EXIT:
        syscall_exit(tf);
        break;
    case SYS_STOP:
        syscall_stop(tf);
        break;
    case SYS_DISPLAY:
        syscall_display(tf);
        break;
    case SYS_USLEEP:
        syscall_usleep(tf);
        break;
    case SYS_SIGNAL:
        syscall_signal(tf);
        break;
    case SYS_SIGRETURN:
        syscall_sigreturn(tf);
        break;
    case SYS_KILL:
        syscall_kill(tf);
        break;
    case SYS_MMAP:
        syscall_mmap(tf);
        break;
    case SYS_OPEN:
        syscall_open(tf);
        break;
    case SYS_CLOSE:
        syscall_close_fd(tf);
        break;
    case SYS_READ:
        syscall_read_fd(tf);
        break;
    case SYS_WRITE:
        syscall_write_fd(tf);
        break;
    case SYS_MKDIR:
        syscall_mkdir_fd(tf);
        break;
    case SYS_MOUNT:
        syscall_mount_fd(tf);
        break;
    case SYS_CHDIR:
        syscall_chdir_fd(tf);
        break;
    default:
        uart_puts("[syscall] unknown syscall: ");
        uart_dec(tf->a7);
        uart_putc('\n');
        tf->a0 = (unsigned long)-1L;
        tf->sepc += 4;
        break;
    }
}

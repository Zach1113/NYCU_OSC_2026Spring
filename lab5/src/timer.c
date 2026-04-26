#include "timer.h"

#include "fdt.h"
#include "task.h"
#include "uart.h"

#define SBI_EXT_TIME      0x54494d45UL
#define SBI_EXT_TIME_SET  0UL
#define SBI_EXT_LEGACY_SET_TIMER 0UL

#define SIE_STIE          (1UL << 5) // Supervisor Timer Interrupt Enable
#define SSTATUS_SIE       (1UL << 1) // Supervisor Interrupt Enable

#define DEFAULT_TIMEBASE_HZ 24000000ULL
#define PERIOD_SECONDS       2ULL
#define MAX_TIMERS           32
#define TIMER_TASK_PRIORITY  1

struct sbiret {
    long error;
    long value;
};

struct timer_entry {
    int used;
    unsigned long seq;
    unsigned long long deadline;
    timer_callback_t callback;
    void *arg;
    struct timer_entry *next;
};

static unsigned long long g_timebase_hz = DEFAULT_TIMEBASE_HZ;
static unsigned long long g_boot_time;
static unsigned long long g_periodic_next_fire;
static unsigned long long g_programmed_fire;
static int g_timer_warned;
static int g_periodic_timer_enabled = 1;
static unsigned long g_timer_seq;
static struct timer_entry g_timers[MAX_TIMERS];
static struct timer_entry *g_timer_head;

static unsigned long irq_save(void) {
    unsigned long sstatus;

    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    asm volatile("csrci sstatus, 2");
    return sstatus;
}

static void irq_restore(unsigned long sstatus) {
    asm volatile("csrw sstatus, %0" : : "r"(sstatus));
}

static struct sbiret sbi_ecall(unsigned long ext, unsigned long fid,
                               unsigned long arg0, unsigned long arg1,
                               unsigned long arg2, unsigned long arg3,
                               unsigned long arg4, unsigned long arg5) {
    struct sbiret ret;
    register unsigned long a0 asm("a0") = arg0;
    register unsigned long a1 asm("a1") = arg1;
    register unsigned long a2 asm("a2") = arg2;
    register unsigned long a3 asm("a3") = arg3;
    register unsigned long a4 asm("a4") = arg4;
    register unsigned long a5 asm("a5") = arg5;
    register unsigned long a6 asm("a6") = fid;
    register unsigned long a7 asm("a7") = ext;

    asm volatile("ecall"
                 : "+r"(a0), "+r"(a1)
                 : "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
                 : "memory");

    ret.error = (long)a0;
    ret.value = (long)a1;
    return ret;
}

static void sbi_set_timer(unsigned long long stime_value) {
    struct sbiret ret;

    ret = sbi_ecall(SBI_EXT_TIME, SBI_EXT_TIME_SET,
                    (unsigned long)stime_value,
                    (unsigned long)(stime_value >> 32),
                    0, 0, 0, 0);
    if (ret.error == 0)
        return;

    ret = sbi_ecall(SBI_EXT_LEGACY_SET_TIMER, 0,
                    (unsigned long)stime_value,
                    0, 0, 0, 0, 0);
    if (ret.error == 0)
        return;

    if (!g_timer_warned) {
        g_timer_warned = 1;
        uart_puts("[Timer] sbi_set_timer failed, error=");
        uart_dec((unsigned long)ret.error);
        uart_putc('\n');
    }
}

static unsigned long long timer_period_ticks(void) {
    return g_timebase_hz * PERIOD_SECONDS;
}

static void enable_timer_interrupts(void) {
    unsigned long v;

    asm volatile("csrr %0, sie" : "=r"(v));
    v |= SIE_STIE;
    asm volatile("csrw sie, %0" : : "r"(v));

    asm volatile("csrr %0, sstatus" : "=r"(v));
    v |= SSTATUS_SIE;
    asm volatile("csrw sstatus, %0" : : "r"(v));
}

static unsigned long long timer_deadline_from_seconds(int sec) {
    unsigned long long now = timer_now();

    if (sec <= 0)
        return now;
    return now + (unsigned long long)sec * g_timebase_hz;
}

static void program_next_event_locked(void) {
    unsigned long long next = g_periodic_next_fire;
    unsigned long long now = timer_now();

    if (g_timer_head && g_timer_head->deadline < next)
        next = g_timer_head->deadline;
    if (next <= now)
        next = now + 1ULL;

    g_programmed_fire = next;
    sbi_set_timer(next);
}

static void timer_periodic_print_task(void *arg) {
    unsigned long long sec = (unsigned long long)(unsigned long)arg;

    uart_puts("[Timer] ");
    uart_dec((unsigned long)sec);
    uart_puts(" seconds after boot\n");
}

static void timer_task_trampoline(void *arg) {
    struct timer_entry *timer = (struct timer_entry *)arg;
    timer_callback_t callback;
    void *cb_arg;

    if (!timer || !timer->used)
        return;

    callback = timer->callback;
    cb_arg = timer->arg;

    timer->used = 0;
    timer->callback = 0;
    timer->arg = 0;
    timer->next = 0;

    if (callback)
        callback(cb_arg);
}

unsigned long long timer_now(void) {
    unsigned long long t;
    asm volatile("rdtime %0" : "=r"(t));
    return t;
}

unsigned long timer_seconds_since_boot(void) {
    unsigned long long now = timer_now();

    if (g_timebase_hz == 0)
        return 0;
    return (unsigned long)((now - g_boot_time) / g_timebase_hz);
}

void timer_init_from_dtb(const void *fdt) {
    int off;
    int len = 0;
    const void *prop;

    g_timebase_hz = DEFAULT_TIMEBASE_HZ;

    if (!fdt)
        return;

    off = fdt_path_offset(fdt, "/cpus");
    if (off < 0)
        return;

    prop = fdt_getprop(fdt, off, "timebase-frequency", &len);
    if (!prop)
        return;

    if (len >= 8)
        g_timebase_hz = fdt_be64(prop);
    else if (len >= 4)
        g_timebase_hz = fdt_be32(prop);
}

void timer_init(void) {
    int i;

    g_boot_time = timer_now();
    g_periodic_next_fire = g_boot_time + timer_period_ticks();
    g_programmed_fire = g_periodic_next_fire;
    g_timer_warned = 0;
    g_periodic_timer_enabled = 1;
    g_timer_seq = 0;
    g_timer_head = 0;
    for (i = 0; i < MAX_TIMERS; i++) {
        g_timers[i].used = 0;
        g_timers[i].next = 0;
    }

    sbi_set_timer(g_programmed_fire);
    enable_timer_interrupts();
}

int add_timer(timer_callback_t callback, void *arg, int sec) {
    unsigned long flags;
    struct timer_entry *timer = 0;
    struct timer_entry **pp;
    int i;

    if (!callback)
        return 0;

    flags = irq_save();

    for (i = 0; i < MAX_TIMERS; i++) {
        if (!g_timers[i].used) {
            timer = &g_timers[i];
            timer->used = 1;
            timer->seq = g_timer_seq++;
            timer->deadline = timer_deadline_from_seconds(sec);
            timer->callback = callback;
            timer->arg = arg;
            timer->next = 0;
            break;
        }
    }

    if (!timer) {
        irq_restore(flags);
        return 0;
    }

    pp = &g_timer_head;
    while (*pp) {
        if ((*pp)->deadline > timer->deadline)
            break;
        if ((*pp)->deadline == timer->deadline && (*pp)->seq > timer->seq)
            break;
        pp = &(*pp)->next;
    }
    timer->next = *pp;
    *pp = timer;

    program_next_event_locked();
    irq_restore(flags);
    return 1;
}

void timer_handle_interrupt(void) {
    unsigned long long now = timer_now();

    while (g_periodic_next_fire <= now) {
        unsigned long long sec = 0;

        if (g_periodic_timer_enabled) {
            if (g_timebase_hz != 0)
                sec = (g_periodic_next_fire - g_boot_time) / g_timebase_hz;
            if (!add_task(timer_periodic_print_task, (void *)(unsigned long)sec, 0))
                timer_periodic_print_task((void *)(unsigned long)sec);
        }
        g_periodic_next_fire += timer_period_ticks();
    }

    while (g_timer_head && g_timer_head->deadline <= now) {
        struct timer_entry *timer = g_timer_head;

        g_timer_head = timer->next;
        timer->next = 0;
        if (!add_task(timer_task_trampoline, timer, TIMER_TASK_PRIORITY))
            timer_task_trampoline(timer);
    }

    program_next_event_locked();
}

void timer_set_periodic_enabled(int enabled) {
    unsigned long flags = irq_save();

    g_periodic_timer_enabled = enabled ? 1 : 0;
    if (g_periodic_timer_enabled && g_periodic_next_fire <= timer_now())
        g_periodic_next_fire = timer_now() + timer_period_ticks();
    program_next_event_locked();
    irq_restore(flags);
}

int timer_periodic_enabled(void) {
    return g_periodic_timer_enabled;
}

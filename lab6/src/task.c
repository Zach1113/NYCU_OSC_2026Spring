#include "task.h"

#include "uart.h"

#define SSTATUS_SIE (1UL << 1)
#define MAX_TASKS   64
#define MAX_NESTED_TASKS 16

struct task_item {
    int used;
    int priority;
    unsigned long seq;
    task_callback_t callback;
    void *arg;
    struct task_item *next;
};

static struct task_item g_tasks[MAX_TASKS];
static struct task_item *g_ready_head;
static unsigned long g_task_seq;
static int g_priority_stack[MAX_NESTED_TASKS];
static int g_task_depth;
static int priority_set[4];

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

static int current_running_priority(void) {
    if (g_task_depth <= 0)
        return -2147483647 - 1;
    return g_priority_stack[g_task_depth - 1];
}

static int outranks(const struct task_item *a, const struct task_item *b) {
    if (a->priority != b->priority)
        return a->priority > b->priority;
    return a->seq < b->seq;
}

void task_init(void) {
    int i;

    g_ready_head = 0;
    g_task_seq = 0;
    g_task_depth = 0;
    for (i = 0; i < MAX_TASKS; i++) {
        g_tasks[i].used = 0;
        g_tasks[i].next = 0;
    }
}

int add_task(task_callback_t callback, void *arg, int priority) {
    unsigned long flags;
    struct task_item *item = 0;
    struct task_item **pp;
    int i;

    if (!callback)
        return 0;

    flags = irq_save();
    for (i = 0; i < MAX_TASKS; i++) {
        if (!g_tasks[i].used) {
            item = &g_tasks[i];
            item->used = 1;
            item->priority = priority;
            item->seq = g_task_seq++;
            item->callback = callback;
            item->arg = arg;
            item->next = 0;
            break;
        }
    }

    if (!item) {
        irq_restore(flags);
        return 0;
    }

    pp = &g_ready_head;
    while (*pp && !outranks(item, *pp))
        pp = &(*pp)->next;
    item->next = *pp;
    *pp = item;

    irq_restore(flags);
    return 1;
}

void task_run_ready(void) {
    unsigned long outer_flags;

    outer_flags = irq_save();

    while (1) {
        struct task_item *item;
        int threshold;

        threshold = current_running_priority();
        item = g_ready_head;
        if (!item)
            break;
        if (g_task_depth > 0 && item->priority <= threshold)
            break;

        g_ready_head = item->next;
        item->next = 0;

        if (g_task_depth < MAX_NESTED_TASKS)
            g_priority_stack[g_task_depth++] = item->priority;

        irq_enable();
        item->callback(item->arg);
        outer_flags = irq_save();

        if (g_task_depth > 0)
            g_task_depth--;

        item->used = 0;
        item->callback = 0;
        item->arg = 0;
        item->next = 0;
    }

    irq_restore(outer_flags);
}

void p1_callback() {
 
    uart_puts("P1 start\n");
    uart_puts("P1 end\n");
}

void p3_callback() {

    uart_puts("P3 start\n");
    add_task(p1_callback, 0, priority_set[0]);
    task_run_ready();
    uart_puts("P3 end\n");
}

void p2_callback() {

    uart_puts("P2 start\n");
    add_task(p3_callback, 0, priority_set[2]);
    task_run_ready();
    uart_puts("P2 end\n");
}

void p4_callback() {

    uart_puts("P4 start\n");
    add_task(p2_callback, 0, priority_set[1]);
    task_run_ready();
    uart_puts("P4 end\n");
}

void test_func(void) {
    int from_small_to_big = 0;

    if (from_small_to_big) {
        priority_set[0] = 10;
        priority_set[1] = 20;
        priority_set[2] = 30;
        priority_set[3] = 40;
    } else {
        priority_set[0] = 40;
        priority_set[1] = 30;
        priority_set[2] = 20;
        priority_set[3] = 10;
    }

    if (!add_task(p4_callback, 0, priority_set[3]))
        uart_puts("taskbatch: task queue full\n");
}

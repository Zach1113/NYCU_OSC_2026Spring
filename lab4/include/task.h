#ifndef LAB4_TASK_H
#define LAB4_TASK_H

typedef void (*task_callback_t)(void *arg);

void task_init(void);
int add_task(task_callback_t callback, void *arg, int priority);
void task_run_ready(void);
int task_enqueue_demo_batch(void);

#endif

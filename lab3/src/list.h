#ifndef KERNEL_LIST_H
#define KERNEL_LIST_H

struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

static inline void list_init(struct list_head *head) {
    head->next = head;
    head->prev = head;
}

static inline int list_empty(const struct list_head *head) {
    return head->next == head;
}

static inline void list_add(struct list_head *node, struct list_head *head) {
    node->next = head->next;
    node->prev = head;
    head->next->prev = node;
    head->next = node;
}

static inline void list_del(struct list_head *node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->next = node;
    node->prev = node;
}

#define list_entry(ptr, type, member) \
    ((type *)((char *)(ptr) - (unsigned long)(&((type *)0)->member)))

#endif

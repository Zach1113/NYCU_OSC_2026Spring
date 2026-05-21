#include "mmap.h"

#include "mm.h"
#include "sched.h"
#include "vm.h"

struct mmap_region {
    unsigned long start;
    unsigned long end;
    int prot;
    int flags;
    unsigned long page_count;
    unsigned long *frames;
    struct mmap_region *next;
};

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

static int ranges_overlap(unsigned long a_start, unsigned long a_end,
                          unsigned long b_start, unsigned long b_end) {
    return a_start < b_end && b_start < a_end;
}

static int mmap_flags_supported(int flags) {
    return (flags & MAP_ANONYMOUS) &&
           ((flags & ~(MAP_ANONYMOUS | MAP_POPULATE)) == 0);
}

static int mmap_prot_supported(int prot) {
    return (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) == 0;
}

static unsigned long mmap_pte_prot(int prot) {
    unsigned long pte = PTE_V | PTE_U | PTE_A | PTE_D;

    if (prot & (PROT_READ | PROT_WRITE))
        pte |= PTE_R;
    if (prot & PROT_WRITE)
        pte |= PTE_W;
    if (prot & PROT_EXEC)
        pte |= PTE_X;
    return pte;
}

static int user_range_conflict_end(const struct thread *t,
                                   unsigned long start,
                                   unsigned long end,
                                   unsigned long *next_start) {
    const struct mmap_region *r;
    unsigned long next = 0;
    int conflict = 0;

    if (t->user_image_alloc_size &&
        ranges_overlap(start, end, 0, t->user_image_alloc_size)) {
        next = t->user_image_alloc_size;
        conflict = 1;
    }

    if (ranges_overlap(start, end, USER_STACK_BASE, USER_STACK_TOP)) {
        if (!conflict || USER_STACK_TOP > next)
            next = USER_STACK_TOP;
        conflict = 1;
    }

    if (t->signal_stack &&
        ranges_overlap(start, end, USER_SIGNAL_STACK_BASE,
                       USER_SIGNAL_STACK_TOP)) {
        if (!conflict || USER_SIGNAL_STACK_TOP > next)
            next = USER_SIGNAL_STACK_TOP;
        conflict = 1;
    }

    for (r = t->mmap_regions; r; r = r->next) {
        if (!ranges_overlap(start, end, r->start, r->end))
            continue;
        if (!conflict || r->end > next)
            next = r->end;
        conflict = 1;
    }

    if (conflict && next_start)
        *next_start = next;
    return conflict;
}

static int user_range_available(const struct thread *t,
                                unsigned long start,
                                unsigned long length) {
    unsigned long end;

    if (!t || length == 0)
        return 0;
    end = start + length;
    if (end < start || end > USER_SIGNAL_STACK_BASE)
        return 0;
    return !user_range_conflict_end(t, start, end, 0);
}

static unsigned long user_mmap_find_base(const struct thread *t,
                                         unsigned long hint,
                                         unsigned long length) {
    unsigned long candidate;

    if (hint && (hint & (PAGE_SIZE - 1UL)) == 0 &&
        user_range_available(t, hint, length))
        return hint;

    candidate = USER_MMAP_BASE;
    while (candidate < USER_SIGNAL_STACK_BASE) {
        unsigned long end = candidate + length;
        unsigned long next = 0;

        if (end < candidate || end > USER_SIGNAL_STACK_BASE)
            return 0;
        if (!user_range_conflict_end(t, candidate, end, &next))
            return candidate;
        candidate = align_up(next, PAGE_SIZE);
        if (candidate < USER_MMAP_BASE)
            return 0;
    }

    return 0;
}

static void user_mmap_free_region(struct thread *t, struct mmap_region *r,
                                  unsigned long mapped_pages) {
    unsigned long i;

    if (!r)
        return;
    if (t && t->pgd && mapped_pages)
        unmap_pages((unsigned long *)t->pgd, r->start,
                    mapped_pages * PAGE_SIZE);
    if (r->frames) {
        for (i = 0; i < r->page_count; i++) {
            if (r->frames[i])
                free((void *)r->frames[i]);
        }
        free(r->frames);
    }
    free(r);
}

static struct mmap_region *user_mmap_create_region(struct thread *t,
                                                   unsigned long start,
                                                   unsigned long length,
                                                   int prot,
                                                   int flags) {
    struct mmap_region *r;
    unsigned long i;
    unsigned long pte_prot;

    r = (struct mmap_region *)alloc(sizeof(*r));
    if (!r)
        return 0;
    zero_bytes(r, sizeof(*r));

    r->start = start;
    r->end = start + length;
    r->prot = prot;
    r->flags = flags;
    r->page_count = length / PAGE_SIZE;
    r->frames = (unsigned long *)alloc(r->page_count * sizeof(unsigned long));
    if (!r->frames) {
        free(r);
        return 0;
    }
    zero_bytes(r->frames, r->page_count * sizeof(unsigned long));

    pte_prot = mmap_pte_prot(prot);
    for (i = 0; i < r->page_count; i++) {
        unsigned long va = start + i * PAGE_SIZE;
        void *frame = alloc(PAGE_SIZE);

        if (!frame) {
            user_mmap_free_region(t, r, i);
            return 0;
        }
        zero_bytes(frame, PAGE_SIZE);
        r->frames[i] = (unsigned long)frame;

        if (!map_pages((unsigned long *)t->pgd, va, PAGE_SIZE,
                       virt_to_phys((unsigned long)frame), pte_prot)) {
            user_mmap_free_region(t, r, i);
            return 0;
        }
    }

    r->next = t->mmap_regions;
    t->mmap_regions = r;
    return r;
}

unsigned long user_mmap_anonymous(struct thread *t, unsigned long addr,
                                  unsigned long length, int prot, int flags) {
    unsigned long base;
    unsigned long page_count;

    if (!t || !t->pgd || length == 0 || !mmap_flags_supported(flags) ||
        !mmap_prot_supported(prot))
        return 0;
    if (length > ~0UL - (PAGE_SIZE - 1UL))
        return 0;

    length = align_up(length, PAGE_SIZE);
    if (length == 0)
        return 0;
    page_count = length / PAGE_SIZE;
    if (page_count == 0 || page_count > (~0UL / sizeof(unsigned long)))
        return 0;

    base = user_mmap_find_base(t, addr, length);
    if (!base)
        return 0;

    if (!user_mmap_create_region(t, base, length, prot, flags))
        return 0;
    return base;
}

int user_mmap_clone(struct thread *dst, const struct thread *src) {
    const struct mmap_region *r;

    if (!dst || !src)
        return 0;

    for (r = src->mmap_regions; r; r = r->next) {
        struct mmap_region *clone;
        unsigned long i;
        unsigned long length = r->end - r->start;

        if (!user_range_available(dst, r->start, length))
            return 0;
        clone = user_mmap_create_region(dst, r->start, length,
                                        r->prot, r->flags);
        if (!clone)
            return 0;
        for (i = 0; i < r->page_count; i++)
            copy_bytes((void *)clone->frames[i],
                       (const void *)r->frames[i], PAGE_SIZE);
    }

    return 1;
}

void user_mmap_destroy(struct thread *t) {
    struct mmap_region *r;

    if (!t)
        return;

    r = t->mmap_regions;
    t->mmap_regions = 0;
    while (r) {
        struct mmap_region *next = r->next;

        user_mmap_free_region(t, r, r->page_count);
        r = next;
    }
}

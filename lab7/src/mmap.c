#include "mmap.h"

#include "mm.h"
#include "sched.h"
#include "uart.h"
#include "vm.h"

#define SCAUSE_INST_PAGE_FAULT  12UL
#define SCAUSE_LOAD_PAGE_FAULT  13UL
#define SCAUSE_STORE_PAGE_FAULT 15UL

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

static int mmap_prot_allows_fault(int prot, unsigned long cause) {
    if (cause == SCAUSE_INST_PAGE_FAULT) // if the page fault is caused by instruction fetch, check if the region has PROT_EXEC permission
        return (prot & PROT_EXEC) != 0;
    if (cause == SCAUSE_LOAD_PAGE_FAULT) // if the page fault is caused by load, check if the region has PROT_READ or PROT_WRITE permission
        return (prot & (PROT_READ | PROT_WRITE)) != 0;
    if (cause == SCAUSE_STORE_PAGE_FAULT) // if the page fault is caused by store, check if the region has PROT_WRITE permission
        return (prot & PROT_WRITE) != 0;
    return 0;
}

static void log_translation_fault(unsigned long addr) {
    uart_puts("[Translation fault]: ");
    uart_hex(addr);
    uart_putc('\n');
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
                mm_page_put((void *)r->frames[i]);
        }
        free(r->frames);
    }
    free(r);
}

static int user_mmap_map_page(struct thread *t, struct mmap_region *r,
                              unsigned long page_index,
                              unsigned long existing_frame) {
    unsigned long va;       // user virtual address
    unsigned long frame;    // kernel virtual address use to map physical page frame

    if (!t || !t->pgd || !r || page_index >= r->page_count)
        return 0;

    if (existing_frame) {
        frame = existing_frame;
    } else {
        frame = (unsigned long)alloc(PAGE_SIZE);  // frame is a kernel virtual address of a newly allocated page
        if (!frame)
            return 0;
        zero_bytes((void *)frame, PAGE_SIZE);
    }

    va = r->start + page_index * PAGE_SIZE;
    if (!map_pages((unsigned long *)t->pgd, va, PAGE_SIZE,      // map user virtual address to physical page frame
                   virt_to_phys(frame), mmap_pte_prot(r->prot))) {
        if (!existing_frame)
            free((void *)frame);
        return 0;
    }

    r->frames[page_index] = frame;
    return 1;
}

static struct mmap_region *user_mmap_create_region(struct thread *t,
                                                   unsigned long start,
                                                   unsigned long length,
                                                   int prot,
                                                   int flags) {
    struct mmap_region *r;
    unsigned long i;

    // allocate and initialize region structure
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

    // if MAP_POPULATE is set, allocate and map all pages immediately
    if (flags & MAP_POPULATE) {
        for (i = 0; i < r->page_count; i++) {
            if (!user_mmap_map_page(t, r, i, 0)) {
                user_mmap_free_region(t, r, i);
                return 0;
            }
        }
    }

    r->next = t->mmap_regions;
    t->mmap_regions = r;
    return r;
}

static struct mmap_region *user_mmap_find_region(struct thread *t,
                                                 unsigned long addr) {
    struct mmap_region *r;

    if (!t)
        return 0;

    for (r = t->mmap_regions; r; r = r->next) {
        if (addr >= r->start && addr < r->end)
            return r;
    }
    return 0;
}

static int pte_make_cow(unsigned long *pte) {
    if (!pte || !(*pte & PTE_V) || !(*pte & PTE_U))
        return 0;
    if (*pte & PTE_W)
        *pte = (*pte & ~PTE_W) | PTE_COW;
    return 1;
}

static int user_mmap_share_page(struct thread *dst,
                                const struct thread *src,
                                struct mmap_region *clone,
                                const struct mmap_region *src_region,
                                unsigned long page_index) {
    unsigned long src_frame = src_region->frames[page_index];
    unsigned long va = src_region->start + page_index * PAGE_SIZE;
    unsigned long *src_pte;
    unsigned long flags;

    if (!src_frame)
        return 1;

    src_pte = vm_get_pte((unsigned long *)src->pgd, va, 0);
    if (!src_pte || !(*src_pte & PTE_V))
        return 0;

    if (src_region->prot & PROT_WRITE)
        pte_make_cow(src_pte);
    flags = *src_pte & PTE_FLAGS_MASK;

    mm_page_get((void *)src_frame);
    if (!map_pages((unsigned long *)dst->pgd, va, PAGE_SIZE,
                   virt_to_phys(src_frame), flags)) {
        mm_page_put((void *)src_frame);
        return 0;
    }

    clone->frames[page_index] = src_frame;
    return 1;
}

int user_mmap_handle_page_fault(struct thread *t, unsigned long addr,
                                unsigned long cause) {
    struct mmap_region *r;
    unsigned long page_index;
    unsigned long va;

    r = user_mmap_find_region(t, addr); // find the memory region that contains the faulting address
    if (!r || !mmap_prot_allows_fault(r->prot, cause))
        return 0;

    va = addr & ~(PAGE_SIZE - 1UL);
    if (vm_translate((unsigned long *)t->pgd, va))  // check if the faulting address is already mapped, should return 0 for mmap demand paging case
        return 0;

    page_index = (addr - r->start) / PAGE_SIZE;
    if (page_index >= r->page_count)
        return 0;

    if (!r->frames[page_index] &&                   // if frames[page_index] == 0, it means the page is not allocated and mapped yet
        !user_mmap_map_page(t, r, page_index, 0))   // call user_mmap_map_page to allocate a new page and map it to the faulting address
        return 0;

    log_translation_fault(addr);
    return 1;
}

int user_mmap_replace_frame(struct thread *t, unsigned long addr,
                            unsigned long old_frame,
                            unsigned long new_frame) {
    struct mmap_region *r = user_mmap_find_region(t, addr);
    unsigned long page_index;

    if (!r || !(r->prot & PROT_WRITE))
        return 0;

    page_index = (addr - r->start) / PAGE_SIZE;
    if (page_index >= r->page_count || r->frames[page_index] != old_frame)
        return 0;

    r->frames[page_index] = new_frame;
    return 1;
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
                                        r->prot, r->flags & ~MAP_POPULATE);
        if (!clone)
            return 0;
        clone->flags = r->flags;
        for (i = 0; i < r->page_count; i++) {
            if (!user_mmap_share_page(dst, src, clone, r, i))
                return 0;
        }
    }

    vm_flush_tlb();
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

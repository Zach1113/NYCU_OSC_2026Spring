#include "uart.h"
#include "mm.h"
#include "list.h"
#include "fdt.h"

#define PAGE_SIZE   4096UL
#define MAX_ORDER   10
#define MAX_ALLOC_SIZE (PAGE_SIZE * (1UL << MAX_ORDER))

#define FRAME_FREE_HEAD  1U
#define FRAME_FREE_TAIL  2U
#define FRAME_ALLOC_HEAD 3U
#define FRAME_ALLOC_TAIL 4U
#define FRAME_CHUNK_PAGE 5U
#define FRAME_RESERVED_HEAD 6U
#define FRAME_RESERVED_TAIL 7U

#define INVALID_POOL_ID  0xFFFFU
#define POOL_COUNT       8

#define LOGF_VERBOSE         (1U << 0)
#define LOGF_DUMP_ON_CHANGE  (1U << 1)

static const unsigned long g_pool_sizes[POOL_COUNT] = {
    16UL, 32UL, 64UL, 128UL, 256UL, 512UL, 1024UL, 2048UL
};

struct chunk {
    struct chunk *next;
};

struct chunk_pool {
    unsigned long chunk_size;
    struct chunk *free_list;
};

struct frame {
    int order;
    unsigned short state;
    unsigned short pool_id;
    struct list_head node;
};

struct reserve_region {
    unsigned long start;
    unsigned long end;
};

static struct frame *g_frames;
static struct list_head g_free_area[MAX_ORDER + 1];
static struct chunk_pool g_pools[POOL_COUNT];
static int g_mm_ready;
static unsigned int g_log_flags;
static unsigned long g_mm_base;
static unsigned long g_mm_size;
static unsigned long g_num_pages;
static unsigned long g_frame_array_bytes;
static unsigned long g_startup_cur;
static unsigned long g_startup_end;

#define MAX_RESERVED_REGIONS 64
static struct reserve_region g_reserved_regions[MAX_RESERVED_REGIONS];
static int g_reserved_region_count;

static unsigned long idx_to_pa(unsigned long idx);

extern char _start[];
extern char _end[];

static void log_add_block(unsigned long idx, int order) {
    if (!(g_log_flags & LOGF_VERBOSE))
        return;
    unsigned long end = idx + (1UL << order) - 1UL;
    uart_puts("[+] Add page ");
    uart_dec(idx);
    uart_puts(" to order ");
    uart_dec((unsigned long)order);
    uart_puts(". Range of pages: [");
    uart_dec(idx);
    uart_puts(", ");
    uart_dec(end);
    uart_puts("]\n");
}

static void log_remove_block(unsigned long idx, int order) {
    if (!(g_log_flags & LOGF_VERBOSE))
        return;
    unsigned long end = idx + (1UL << order) - 1UL;
    uart_puts("[-] Remove page ");
    uart_dec(idx);
    uart_puts(" from order ");
    uart_dec((unsigned long)order);
    uart_puts(". Range of pages: [");
    uart_dec(idx);
    uart_puts(", ");
    uart_dec(end);
    uart_puts("]\n");
}

static void log_buddy(unsigned long idx, unsigned long buddy, int order, int found) {
    if (!(g_log_flags & LOGF_VERBOSE))
        return;
    if (found)
        uart_puts("[*] Buddy found! buddy idx: ");
    else
        uart_puts("[*] Buddy not mergeable. buddy idx: ");
    uart_dec(buddy);
    uart_puts(" for page ");
    uart_dec(idx);
    uart_puts(" with order ");
    uart_dec((unsigned long)order);
    uart_putc('\n');
}

static void log_split(unsigned long parent, int from_order, unsigned long child) {
    if (!(g_log_flags & LOGF_VERBOSE))
        return;
    uart_puts("[*] Release redundant memory block: split page ");
    uart_dec(parent);
    uart_puts(" order ");
    uart_dec((unsigned long)from_order);
    uart_puts(", return buddy page ");
    uart_dec(child);
    uart_puts(" order ");
    uart_dec((unsigned long)(from_order - 1));
    uart_putc('\n');
}

static void log_chunk_alloc(unsigned long pa, unsigned long chunk_size) {
    if (!(g_log_flags & LOGF_VERBOSE))
        return;
    uart_puts("[Chunk] Allocate ");
    uart_hex(pa);
    uart_puts(" at chunk size ");
    uart_dec(chunk_size);
    uart_putc('\n');
}

static void log_chunk_free(unsigned long pa, unsigned long chunk_size) {
    if (!(g_log_flags & LOGF_VERBOSE))
        return;
    uart_puts("[Chunk] Free ");
    uart_hex(pa);
    uart_puts(" at chunk size ");
    uart_dec(chunk_size);
    uart_putc('\n');
}

static void log_chunk_refill(unsigned long page_pa, unsigned long chunk_size) {
    if (!(g_log_flags & LOGF_VERBOSE))
        return;
    uart_puts("[Chunk] Refill pool size ");
    uart_dec(chunk_size);
    uart_puts(" from page ");
    uart_hex(page_pa);
    uart_putc('\n');
}

static void log_reserve_range(unsigned long start, unsigned long end,
                              unsigned long start_idx, unsigned long end_idx) {
    if (!(g_log_flags & LOGF_VERBOSE))
        return;
    uart_puts("[Reserve] Reserve address [");
    uart_hex(start);
    uart_puts(", ");
    uart_hex(end);
    uart_puts("). Range of pages: [");
    uart_dec(start_idx);
    uart_puts(", ");
    uart_dec(end_idx);
    uart_puts(")\n");
}

static unsigned long next_addr_at_order(int order) {
    struct list_head *n;
    unsigned long idx;

    if (order < 0 || order > MAX_ORDER)
        return 0;
    if (list_empty(&g_free_area[order]))
        return 0;

    n = g_free_area[order].next;
    idx = (unsigned long)(list_entry(n, struct frame, node) - g_frames);
    return idx_to_pa(idx);
}

static unsigned long idx_to_pa(unsigned long idx) {
    return g_mm_base + idx * PAGE_SIZE;
}

static unsigned long pa_to_idx(unsigned long pa) {
    return (pa - g_mm_base) / PAGE_SIZE;
}

static int in_range_pa(unsigned long pa) {
    return g_mm_size != 0 &&
           pa >= g_mm_base &&
           pa < g_mm_base + g_mm_size;
}

static void mark_tail(unsigned long idx, int order, unsigned short state) {
    unsigned long i;
    unsigned long pages = 1UL << order;
    for (i = 1; i < pages; i++) {
        g_frames[idx + i].order = -1;
        g_frames[idx + i].state = state;
        g_frames[idx + i].pool_id = INVALID_POOL_ID;
    }
}

static void block_set_free_head(unsigned long idx, int order) {
    g_frames[idx].order = order;
    g_frames[idx].state = FRAME_FREE_HEAD;
    g_frames[idx].pool_id = INVALID_POOL_ID;
    mark_tail(idx, order, FRAME_FREE_TAIL);
}

static void block_set_alloc_head(unsigned long idx, int order) {
    g_frames[idx].order = order;
    g_frames[idx].state = FRAME_ALLOC_HEAD;
    g_frames[idx].pool_id = INVALID_POOL_ID;
    mark_tail(idx, order, FRAME_ALLOC_TAIL);
}

static void mark_chunk_page(unsigned long idx, unsigned short pool_id) {
    g_frames[idx].order = 0;
    g_frames[idx].state = FRAME_CHUNK_PAGE;
    g_frames[idx].pool_id = pool_id;
}

static void mark_reserved_range(unsigned long start_idx, unsigned long end_idx) {
    unsigned long i;

    if (start_idx >= end_idx || end_idx > g_num_pages)
        return;

    g_frames[start_idx].order = -1;
    g_frames[start_idx].state = FRAME_RESERVED_HEAD;
    g_frames[start_idx].pool_id = INVALID_POOL_ID;
    for (i = start_idx + 1; i < end_idx; i++) {
        g_frames[i].order = -1;
        g_frames[i].state = FRAME_RESERVED_TAIL;
        g_frames[i].pool_id = INVALID_POOL_ID;
    }
}

static void add_free_block(unsigned long idx, int order) {
    block_set_free_head(idx, order);
    list_add(&g_frames[idx].node, &g_free_area[order]);
    log_add_block(idx, order);
    if (g_log_flags & LOGF_DUMP_ON_CHANGE)
        mm_dump_free_areas();
}

static void remove_free_block(unsigned long idx, int order) {
    list_del(&g_frames[idx].node);
    log_remove_block(idx, order);
    if (g_log_flags & LOGF_DUMP_ON_CHANGE)
        mm_dump_free_areas();
}

static int ceil_order_pages(unsigned long pages) {
    int order = 0;
    unsigned long n = 1;
    while (n < pages) {
        n <<= 1;
        order++;
    }
    return order;
}

static int can_merge(unsigned long idx, int order) {
    if (idx >= g_num_pages)
        return 0;
    return g_frames[idx].state == FRAME_FREE_HEAD && g_frames[idx].order == order;
}

// Buddy System Allocator
static unsigned long buddy_alloc_order(int req_order) {
    int cur;
    unsigned long idx;
    struct list_head *n;

    // Find the smallest available block that can satisfy the request
    for (cur = req_order; cur <= MAX_ORDER; cur++) {
        if (!list_empty(&g_free_area[cur]))
            break;
    }
    if (cur > MAX_ORDER)
        return (unsigned long)-1;

    n = g_free_area[cur].next;
    idx = (unsigned long)(list_entry(n, struct frame, node) - g_frames);
    remove_free_block(idx, cur);

    // Split blocks until the required order is reached
    while (cur > req_order) {
        unsigned long buddy;
        cur--;
        buddy = idx + (1UL << cur);
        block_set_free_head(idx, cur);
        add_free_block(buddy, cur);
        log_split(idx, cur + 1, buddy);
    }

    block_set_alloc_head(idx, req_order);
    return idx;
}

// free page block
static void buddy_free_idx(unsigned long *idx_io, int *order_io) {
    unsigned long idx = *idx_io;
    int order = *order_io;

    // merge iteratively
    while (order < MAX_ORDER) {
        unsigned long buddy = idx ^ (1UL << order);
        if (buddy >= g_num_pages) {
            log_buddy(idx, buddy, order, 0);
            break;
        }
        if (!can_merge(buddy, order)) {
            log_buddy(idx, buddy, order, 0);
            break;
        }

        log_buddy(idx, buddy, order, 1);
        remove_free_block(buddy, order);
        if (buddy < idx)
            idx = buddy;
        order++;
        block_set_free_head(idx, order);
    }
    add_free_block(idx, order);
    *idx_io = idx;
    *order_io = order;
}

static int pick_pool_id(unsigned long size) {
    int i;
    for (i = 0; i < POOL_COUNT; i++) {
        if (size <= g_pool_sizes[i])
            return i;
    }
    return -1;
}

static int frame_is_reserved(unsigned long idx) {
    unsigned short state;

    if (idx >= g_num_pages)
        return 1;
    state = g_frames[idx].state;
    return state == FRAME_RESERVED_HEAD || state == FRAME_RESERVED_TAIL;
}

static unsigned long align_up(unsigned long value, unsigned long align) {
    if (align == 0)
        return value;
    return (value + align - 1UL) & ~(align - 1UL);
}

static void clip_range_to_managed(unsigned long start, unsigned long size,
                                  unsigned long *clip_start,
                                  unsigned long *clip_end) {
    unsigned long end = start + size;

    if (end < start)
        end = ~0UL;

    *clip_start = start;
    if (*clip_start < g_mm_base)
        *clip_start = g_mm_base;

    *clip_end = end;
    if (*clip_end > g_mm_base + g_mm_size)
        *clip_end = g_mm_base + g_mm_size;
}

static void record_reserved_range(unsigned long start, unsigned long size) {
    unsigned long clip_start;
    unsigned long clip_end;

    if (g_reserved_region_count >= MAX_RESERVED_REGIONS || size == 0)
        return;

    clip_range_to_managed(start, size, &clip_start, &clip_end);
    if (clip_start >= clip_end)
        return;

    clip_start &= ~(PAGE_SIZE - 1UL);
    clip_end = align_up(clip_end, PAGE_SIZE);
    if (clip_end > g_mm_base + g_mm_size)
        clip_end = g_mm_base + g_mm_size;
    if (clip_start >= clip_end)
        return;

    g_reserved_regions[g_reserved_region_count].start = clip_start;
    g_reserved_regions[g_reserved_region_count].end = clip_end;
    g_reserved_region_count++;
}

static int startup_find_conflict(unsigned long start, unsigned long end,
                                 unsigned long *next_start) {
    int i;
    int found = 0;
    unsigned long candidate = start;

    for (i = 0; i < g_reserved_region_count; i++) {
        unsigned long res_start = g_reserved_regions[i].start;
        unsigned long res_end = g_reserved_regions[i].end;

        // no conflict, pass
        if (end <= res_start || start >= res_end)
            continue;
        
        // conflict, update candidate
        if (!found || res_end > candidate)
            candidate = res_end;
        found = 1;
    }

    if (found)
        *next_start = candidate;
    return found;
}

static unsigned long startup_alloc(unsigned long size, unsigned long align) {
    unsigned long start;
    unsigned long end;
    unsigned long next_start;

    if (size == 0)
        return 0;

    for (;;) {
        start = align_up(g_startup_cur, align);
        end = start + size;
        if (end < start || end > g_startup_end)
            return (unsigned long)-1;
        if (!startup_find_conflict(start, end, &next_start)) {
            g_startup_cur = end; // starting address of next startup allocation
            return start; // g_frame starting address
        }
        g_startup_cur = next_start;
    }
}

static int startup_alloc_frame_array(void) {
    unsigned long frame_array_pa;

    g_frame_array_bytes = align_up(g_num_pages * sizeof(struct frame), PAGE_SIZE);
    g_startup_cur = g_mm_base;
    g_startup_end = g_mm_base + g_mm_size;

    frame_array_pa = startup_alloc(g_frame_array_bytes, PAGE_SIZE);
    if (frame_array_pa == (unsigned long)-1)
        return 0;

    g_frames = (struct frame *)frame_array_pa;
    record_reserved_range(frame_array_pa, g_frame_array_bytes);
    return 1;
}

static void reserve_memory(unsigned long start, unsigned long size) {
    unsigned long clip_start;
    unsigned long clip_end;
    unsigned long start_idx;
    unsigned long end_idx;

    if (!g_num_pages || size == 0)
        return;

    clip_range_to_managed(start, size, &clip_start, &clip_end);

    if (clip_start >= clip_end)
        return;

    clip_start &= ~(PAGE_SIZE - 1UL);
    clip_end = align_up(clip_end, PAGE_SIZE);
    if (clip_end > g_mm_base + g_mm_size)
        clip_end = g_mm_base + g_mm_size;
    if (clip_start >= clip_end)
        return;

    start_idx = pa_to_idx(clip_start);
    end_idx = pa_to_idx(clip_end);
    if (end_idx > g_num_pages)
        end_idx = g_num_pages;
    if (start_idx >= end_idx)
        return;

    mark_reserved_range(start_idx, end_idx);
    log_reserve_range(clip_start, clip_end, start_idx, end_idx);
}

static int init_memory_region_from_fdt(const void *fdt) {
    unsigned long base;
    unsigned long size;

    if (!fdt_get_memory_region(fdt, 0, &base, &size))
        return 0;

    size &= ~(PAGE_SIZE - 1UL);
    if (size == 0)
        return 0;

    g_mm_base = base;
    g_mm_size = size;
    g_num_pages = size / PAGE_SIZE;
    return 1;
}

static void reserve_fdt_blob(const void *fdt) {
    const struct fdt_header *h;

    if (!fdt)
        return;
    h = (const struct fdt_header *)fdt;
    record_reserved_range((unsigned long)fdt, fdt_be32(&h->totalsize));
}

static void reserve_kernel_image(void) {
    unsigned long start = (unsigned long)_start;
    unsigned long end = (unsigned long)_end;

    if (end > start)
        record_reserved_range(start, end - start);
}

static void reserve_initrd_from_fdt(const void *fdt) {
    unsigned long start;
    unsigned long end;

    if (fdt_get_initrd_region(fdt, &start, &end))
        record_reserved_range(start, end - start);
}

static void reserve_reserved_memory_children(const void *fdt) {
    int entry = 0;
    unsigned long base;
    unsigned long size;

    while (fdt_get_reserved_memory_region(fdt, entry, &base, &size)) {
        record_reserved_range(base, size);
        entry++;
    }
}

static void apply_reserved_ranges(void) {
    int i;

    for (i = 0; i < g_reserved_region_count; i++) {
        unsigned long start = g_reserved_regions[i].start;
        unsigned long end = g_reserved_regions[i].end;

        reserve_memory(start, end - start);
    }
}

static void build_initial_free_lists(void) {
    unsigned long i = 0;

    while (i < g_num_pages) {
        unsigned long run_start;
        unsigned long remain;
        int order;

        while (i < g_num_pages && frame_is_reserved(i))
            i++;
        if (i >= g_num_pages)
            break;

        run_start = i;
        while (i < g_num_pages && !frame_is_reserved(i))
            i++;
        remain = i - run_start;

        // break the free range into blocks and add to free lists
        while (remain > 0) {
            order = MAX_ORDER;
            
            // adjust order to ensure block head alignment
            while (order > 0 && ((run_start & ((1UL << order) - 1UL)) != 0))
                order--;

            // adjust order to fit the remaining pages
            while ((1UL << order) > remain)
                order--;
            
            add_free_block(run_start, order);
            run_start += (1UL << order);
            remain -= (1UL << order);
        }
    }
}

static void pool_push_chunk(int pool_id, void *ptr) {
    struct chunk *c = (struct chunk *)ptr;
    c->next = g_pools[pool_id].free_list;
    g_pools[pool_id].free_list = c;
}

static void *pool_pop_chunk(int pool_id) {
    struct chunk *c = g_pools[pool_id].free_list;
    if (!c)
        return 0;
    g_pools[pool_id].free_list = c->next;
    return (void *)c;
}

static int pool_refill(int pool_id) {
    unsigned long idx;
    unsigned long pa;
    unsigned long off;
    unsigned long chunk_size = g_pools[pool_id].chunk_size;

    idx = buddy_alloc_order(0);
    if (idx == (unsigned long)-1)
        return 0;

    mark_chunk_page(idx, (unsigned short)pool_id);
    pa = idx_to_pa(idx);
    log_chunk_refill(pa, chunk_size);

    for (off = 0; off + chunk_size <= PAGE_SIZE; off += chunk_size)
        pool_push_chunk(pool_id, (void *)(pa + off));

    return 1;
}

// Dynamic Memory Allocator - small chunk allocation
static void *chunk_alloc(unsigned long size) {
    int pool_id = pick_pool_id(size);
    void *ptr;
    if (pool_id < 0)
        return 0;

    if (!g_pools[pool_id].free_list) {
        if (!pool_refill(pool_id))
            return 0;
    }

    ptr = pool_pop_chunk(pool_id);
    if (ptr)
        log_chunk_alloc((unsigned long)ptr, g_pools[pool_id].chunk_size);
    return ptr;
}

// free small chunk
static int chunk_free_ptr(unsigned long pa) {
    unsigned long base_pa = pa & ~(PAGE_SIZE - 1UL); // page-aligned base address of the chunk
    unsigned long idx;
    unsigned long chunk_size;
    unsigned long off;
    unsigned short pool_id;

    if (!in_range_pa(base_pa))
        return 0;

    // check if base_pa is a chunk page
    idx = pa_to_idx(base_pa);
    if (idx >= g_num_pages || g_frames[idx].state != FRAME_CHUNK_PAGE)
        return 0;

    pool_id = g_frames[idx].pool_id;
    if (pool_id >= POOL_COUNT)
        return 0;

    // check if pa is chunk-aligned within the page
    chunk_size = g_pools[pool_id].chunk_size;
    off = pa - base_pa;
    if ((off % chunk_size) != 0)
        return 0;

    pool_push_chunk(pool_id, (void *)pa);
    log_chunk_free(pa, chunk_size);
    return 1;
}

void mm_init(const void *fdt) {
    unsigned long i = 0;
    int order;
    unsigned int prev_log_flags;

    g_mm_ready = 0;
    g_frames = 0;
    g_mm_base = 0;
    g_mm_size = 0;
    g_num_pages = 0;
    g_frame_array_bytes = 0;
    g_startup_cur = 0;
    g_startup_end = 0;
    g_reserved_region_count = 0;
    g_log_flags = 0;

    for (order = 0; order <= MAX_ORDER; order++)
        list_init(&g_free_area[order]);

    for (i = 0; i < POOL_COUNT; i++) {
        g_pools[i].chunk_size = g_pool_sizes[i];
        g_pools[i].free_list = 0;
    }

    if (!init_memory_region_from_fdt(fdt)) {
        uart_puts("[MM] Failed to parse /memory from DTB\n");
        return;
    }

    reserve_fdt_blob(fdt);
    reserve_kernel_image();
    reserve_initrd_from_fdt(fdt);
    reserve_reserved_memory_children(fdt);

    if (!startup_alloc_frame_array()) {
        uart_puts("[MM] Failed to allocate frame array during startup\n");
        return;
    }

    for (i = 0; i < g_num_pages; i++) {
        g_frames[i].order = -1;
        g_frames[i].state = FRAME_ALLOC_TAIL;
        g_frames[i].pool_id = INVALID_POOL_ID;
        list_init(&g_frames[i].node);
    }

    apply_reserved_ranges();
    prev_log_flags = g_log_flags;
    g_log_flags &= ~LOGF_VERBOSE;
    build_initial_free_lists();
    g_log_flags = prev_log_flags;

    g_mm_ready = 1;
    uart_puts("[MM] Buddy allocator initialized from DTB. Base=");
    uart_hex(g_mm_base);
    uart_puts(" Size=");
    uart_hex(g_mm_size);
    uart_puts(" FrameArray=");
    uart_hex((unsigned long)g_frames);
    uart_puts(" Bytes=");
    uart_hex(g_frame_array_bytes);
    uart_putc('\n');
}

void *alloc(unsigned long size) {
    unsigned long pages;
    int order;
    unsigned long idx;
    unsigned long pa;
    int pool_id;

    if (!g_mm_ready || size == 0)
        return 0;

    // Call Dynamic Memory Allocator
    if (size < PAGE_SIZE) {
        pool_id = pick_pool_id(size);
        if (pool_id >= 0)
            return chunk_alloc(size);
    }

    // Call Page Frame Allocator
    pages = (size + PAGE_SIZE - 1UL) / PAGE_SIZE;
    order = ceil_order_pages(pages);
    if (order > MAX_ORDER)
        return 0;

    idx = buddy_alloc_order(order);
    if (idx == (unsigned long)-1)
        return 0;

    pa = idx_to_pa(idx);
    if (g_log_flags & LOGF_VERBOSE) {
        uart_puts("[Page] Allocate ");
        uart_hex(pa);
        uart_puts(" at order ");
        uart_dec((unsigned long)order);
        uart_puts(", page ");
        uart_dec(idx);
        uart_puts(". Next address at order ");
        uart_dec((unsigned long)order);
        uart_puts(": ");
        uart_hex(next_addr_at_order(order));
        uart_putc('\n');
    }
    return (void *)pa;
}

void free(void *ptr) {
    unsigned long pa;
    unsigned long idx;
    int order;

    if (!ptr || !g_mm_ready)
        return;

    pa = (unsigned long)ptr;
    if (!in_range_pa(pa)) {
        if (!(g_log_flags & LOGF_VERBOSE))
            return;
        uart_puts("[Page] Free ignore invalid ptr ");
        uart_hex(pa);
        uart_putc('\n');
        return;
    }

    // try free chunk
    if (chunk_free_ptr(pa))
        return;

    // check if pointer is page-aligned
    if ((pa & (PAGE_SIZE - 1UL)) != 0) {
        if (!(g_log_flags & LOGF_VERBOSE))
            return;
        uart_puts("[Page] Free ignore non-page ptr ");
        uart_hex(pa);
        uart_putc('\n');
        return;
    }

    // check if pointer is head of allocated block
    idx = pa_to_idx(pa);
    if (idx >= g_num_pages || g_frames[idx].state != FRAME_ALLOC_HEAD) {
        if (!(g_log_flags & LOGF_VERBOSE))
            return;
        uart_puts("[Page] Free ignore non-head ptr ");
        uart_hex(pa);
        uart_putc('\n');
        return;
    }

    // Free page block
    order = g_frames[idx].order;
    buddy_free_idx(&idx, &order);
    if (g_log_flags & LOGF_VERBOSE) {
        uart_puts("[Page] Free ");
        uart_hex(pa);
        uart_puts(" and add back to order ");
        uart_dec((unsigned long)order);
        uart_puts(", page ");
        uart_dec(idx);
        uart_puts(". Next address at order ");
        uart_dec((unsigned long)order);
        uart_puts(": ");
        uart_hex(next_addr_at_order(order));
        uart_putc('\n');
    }
}

void mm_dump_free_areas(void) {
    int i;
    struct list_head *p;
    unsigned long count;

    uart_puts("[MM] Free area summary\n");
    for (i = MAX_ORDER; i >= 0; i--) {
        count = 0;
        for (p = g_free_area[i].next; p != &g_free_area[i]; p = p->next)
            count++;
        uart_puts("  order ");
        uart_dec((unsigned long)i);
        uart_puts(": ");
        uart_dec(count);
        uart_putc('\n');
    }
}

void mm_self_test(void) {
    char *ptr1;
    char *ptr2;
    char *ptr3;
    char *ptr4;
    char *kmem_ptr1;
    char *kmem_ptr2;
    char *kmem_ptr3;
    char *kmem_ptr4;
    char *kmem_ptr5;
    char *kmem_ptr6;
    char *kmem_ptr7;
    void *kmem_ptr[102];
    int i;
    unsigned int prev_log_flags = g_log_flags;

    g_log_flags |= LOGF_VERBOSE | LOGF_DUMP_ON_CHANGE;

    uart_puts("[MMTEST] start\n");
    uart_puts("[MMTEST] initial free areas\n");
    mm_dump_free_areas();
    uart_puts("----- Testing memory allocation -----\n");
    ptr1 = (char *)alloc(4000);
    ptr2 = (char *)alloc(8000);
    ptr3 = (char *)alloc(4000);
    ptr4 = (char *)alloc(4000);

    uart_puts("----- Deallocation -----\n");
    free(ptr1);
    free(ptr2);
    free(ptr3);
    free(ptr4);

    uart_puts("----- Testing dynamic allocator -----\n");
    kmem_ptr1 = (char *)alloc(16);
    kmem_ptr2 = (char *)alloc(32);
    kmem_ptr3 = (char *)alloc(64);
    kmem_ptr4 = (char *)alloc(128);

    uart_puts("----- Deallocation -----\n");
    free(kmem_ptr1);
    free(kmem_ptr2);
    free(kmem_ptr3);
    free(kmem_ptr4);

    kmem_ptr5 = (char *)alloc(16);
    kmem_ptr6 = (char *)alloc(32);
    free(kmem_ptr5);
    free(kmem_ptr6);

    uart_puts("----- Testing 100 chunk allocations -----\n");
    for (i = 0; i < 100; i++)
        kmem_ptr[i] = alloc(128);
    for (i = 0; i < 100; i++)
        free(kmem_ptr[i]);

    kmem_ptr7 = (char *)alloc(MAX_ALLOC_SIZE + 1UL);
    if (kmem_ptr7 == 0)
        uart_puts("Allocation failed as expected for size > MAX_ALLOC_SIZE\n");
    else {
        uart_puts("Unexpected allocation success for size > MAX_ALLOC_SIZE\n");
        free(kmem_ptr7);
    }

    uart_puts("[MMTEST] done\n");
    g_log_flags = prev_log_flags;
}

void mm_set_log_enabled(int enabled) {
    if (enabled)
        g_log_flags |= LOGF_VERBOSE;
    else
        g_log_flags &= ~LOGF_VERBOSE;
    uart_puts("[MM] verbose log ");
    uart_puts((g_log_flags & LOGF_VERBOSE) ? "on\n" : "off\n");
}

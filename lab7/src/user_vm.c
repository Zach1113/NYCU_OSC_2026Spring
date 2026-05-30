#include "user_vm.h"

#include "mm.h"
#include "mmap.h"
#include "sched.h"
#include "uart.h"
#include "vm.h"

#define SCAUSE_LOAD_PAGE_FAULT  13UL
#define SCAUSE_STORE_PAGE_FAULT 15UL
#define SCAUSE_INST_PAGE_FAULT  12UL

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

static int image_fault_allowed(unsigned long cause) {
    return cause == SCAUSE_INST_PAGE_FAULT ||
           cause == SCAUSE_LOAD_PAGE_FAULT ||
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
        *pte = (*pte & ~PTE_W) | PTE_COW;   // clear write permission and set COW flag
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

    pte_make_cow(src_pte);      // set COW flag on the source PTE and clear write permission if it is set
    flags = *src_pte & PTE_FLAGS_MASK;
    mm_page_get((void *)frame); // frame ref_count++
    if (!map_pages((unsigned long *)dst->pgd, va, PAGE_SIZE,    // map the same frame into child's page table with the same flags 
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

    if (!t || !t->pgd || !t->user_stack_pages || !stack_fault_allowed(cause))   // only handles load/store page fault on user stack access
        return 0;
    if (addr < USER_STACK_BASE || addr >= USER_STACK_TOP)
        return 0;

    va = align_down(addr, PAGE_SIZE);
    if (vm_translate((unsigned long *)t->pgd, va))  // check if the faulting address is already mapped, should return 0 for stack demand paging case
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

int user_image_handle_page_fault(struct thread *t, unsigned long addr,
                                 unsigned long cause) {
    unsigned long va;
    unsigned long page_index;
    unsigned long frame;
    unsigned long copied;
    unsigned long copy_len = 0;

    if (!t || !t->pgd || !t->user_image || !t->user_image_pages ||
        !image_fault_allowed(cause))
        return 0;
    if (addr >= t->user_image_alloc_size)
        return 0;

    va = align_down(addr, PAGE_SIZE);
    if (vm_translate((unsigned long *)t->pgd, va))
        return 0;

    page_index = va / PAGE_SIZE;
    if (page_index >= t->user_image_page_count)
        return 0;

    frame = t->user_image_pages[page_index];
    if (!frame) {
        frame = alloc_zero_page();
        if (!frame)
            return 0;

        copied = page_index * PAGE_SIZE;
        if (copied < t->user_image_size)
            copy_len = min_ulong(PAGE_SIZE, t->user_image_size - copied);
        if (copy_len)
            copy_bytes((void *)frame,
                       (const char *)t->user_image + copied,
                       copy_len);
        t->user_image_pages[page_index] = frame;
    }

    if (!map_tracked_page(t, va, frame, PROT_USER_RWX)) {
        if (t->user_image_pages[page_index] == frame) {
            t->user_image_pages[page_index] = 0;
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

    old_frame = phys_to_virt(vm_pte_pa(*pte));  // get the old frame address from PTE
    log_permission_fault(addr);

    if (mm_page_ref_count((void *)old_frame) <= 1) {    // if ref_count <= 1, restore write permission and clear COW flag
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
    *pte = vm_make_pte(virt_to_phys(new_frame), flags); // point PTE to the new frame with write permission and COW flag cleared
    mm_page_put((void *)old_frame);                     // old frame ref_count--
    vm_flush_tlb();
    return 1;
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

    t->user_image_size = size;
    t->user_image_alloc_size = image_size;
    t->user_image_page_count = image_size / PAGE_SIZE;
    t->user_stack_page_count = USER_STACK_SIZE / PAGE_SIZE;

    t->user_image_pages = (unsigned long *)alloc(t->user_image_page_count *
                                                 sizeof(unsigned long));
    if (!t->user_image_pages) {
        user_address_space_destroy(t);
        return 0;
    }
    zero_bytes(t->user_image_pages,
               t->user_image_page_count * sizeof(unsigned long));

    t->user_stack_pages = (unsigned long *)alloc(t->user_stack_page_count *
                                                 sizeof(unsigned long));
    if (!t->user_stack_pages) {
        user_address_space_destroy(t);
        return 0;
    }
    zero_bytes(t->user_stack_pages,
               t->user_stack_page_count * sizeof(unsigned long));

    t->user_image = (unsigned long)prog;
    t->user_stack = USER_STACK_BASE;
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
    dst->user_image = src->user_image;
    dst->user_image_alloc_size = src->user_image_alloc_size;
    dst->user_image_page_count = src->user_image_page_count;
    dst->user_stack_page_count = src->user_stack_page_count;

    dst->user_image_pages = (unsigned long *)alloc(dst->user_image_page_count *
                                                   sizeof(unsigned long));
    if (!dst->user_image_pages) {
        user_address_space_destroy(dst);
        return 0;
    }
    zero_bytes(dst->user_image_pages,
               dst->user_image_page_count * sizeof(unsigned long));

    dst->user_stack_pages = (unsigned long *)alloc(dst->user_stack_page_count *
                                                   sizeof(unsigned long));
    if (!dst->user_stack_pages) {
        user_address_space_destroy(dst);
        return 0;
    }
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

    dst->user_stack = src->user_stack;
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


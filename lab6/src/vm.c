#include "vm.h"

#include "mm.h"

#define PT_ENTRIES 512UL
#define PGD_KERNEL_BASE 256UL
#define PTE_PPN_SHIFT 10
#define VPN_MASK 0x1ffUL
#define BLOCK_2M_SIZE (2UL * 1024UL * 1024UL)
#define BLOCK_1G_SIZE (1024UL * 1024UL * 1024UL)

static unsigned long g_kernel_pgd[PT_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static unsigned long g_identity_pmd[PT_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static unsigned long g_kernel_pmd[4][PT_ENTRIES] __attribute__((aligned(PAGE_SIZE)));

static unsigned long align_down(unsigned long v, unsigned long align) {
    return v & ~(align - 1UL);
}

static unsigned long align_up(unsigned long v, unsigned long align) {
    return (v + align - 1UL) & ~(align - 1UL);
}

static void zero_page(void *ptr) {
    unsigned long *p = (unsigned long *)ptr;
    unsigned long i;

    for (i = 0; i < PAGE_SIZE / sizeof(unsigned long); i++)
        p[i] = 0;
}

static unsigned long make_pte(unsigned long pa, unsigned long flags) {
    return ((pa >> 12) << PTE_PPN_SHIFT) | flags;
}

static unsigned long pte_pa(unsigned long pte) {
    return (pte >> PTE_PPN_SHIFT) << 12;
}

static int pte_is_leaf(unsigned long pte) {
    return pte & (PTE_R | PTE_W | PTE_X);
}

static int is_mmio_block(unsigned long pa) {
    if (pa >= 0xc0000000UL)
        return 1;
    if (pa >= 0x0c000000UL && pa < 0x10200000UL)
        return 1;
    return 0;
}

static unsigned long vpn_index(unsigned long va, int level) {
    return (va >> (12 + 9 * level)) & VPN_MASK;
}

static void map_2m_range(unsigned long *pgd, unsigned long *pmd,
                         unsigned long va, unsigned long pa,
                         unsigned long size, unsigned long prot) {
    unsigned long end = align_up(va + size, BLOCK_2M_SIZE);
    unsigned long cur_va = align_down(va, BLOCK_2M_SIZE);
    unsigned long cur_pa = align_down(pa, BLOCK_2M_SIZE);
    unsigned long pgd_idx = vpn_index(cur_va, 2);

    pgd[pgd_idx] = make_pte((unsigned long)pmd, PTE_V);
    while (cur_va < end) {
        pmd[vpn_index(cur_va, 1)] = make_pte(cur_pa, prot);
        cur_va += BLOCK_2M_SIZE;
        cur_pa += BLOCK_2M_SIZE;
    }
}

void setup_vm(unsigned long fdt_pa) {
    unsigned long i;
    (void)fdt_pa;

    zero_page(g_kernel_pgd);
    zero_page(g_identity_pmd);
    for (i = 0; i < 4; i++)
        zero_page(g_kernel_pmd[i]);

    map_2m_range(g_kernel_pgd, g_identity_pmd, 0, 0, BLOCK_1G_SIZE,
                 PROT_KERNEL);

    for (i = 0; i < 4; i++) {
        unsigned long va = KERNEL_OFFSET + i * BLOCK_1G_SIZE;
        unsigned long pa = i * BLOCK_1G_SIZE;

        g_kernel_pgd[vpn_index(va, 2)] =
            make_pte((unsigned long)g_kernel_pmd[i], PTE_V);
        for (unsigned long j = 0; j < PT_ENTRIES; j++) {
            unsigned long block_pa = pa + j * BLOCK_2M_SIZE;
            unsigned long flags = is_mmio_block(block_pa) ? PROT_MMIO :
                                  PROT_KERNEL;

            g_kernel_pmd[i][j] = make_pte(block_pa, flags);
        }
    }

    asm volatile(
        "csrw satp, %0\n"
        "sfence.vma zero, zero\n"
        :
        : "r"(MAKE_SATP((unsigned long)g_kernel_pgd))
        : "memory"
    );
}

void drop_identity_map(void) {
    g_kernel_pgd[0] = 0;
    asm volatile("sfence.vma zero, zero" ::: "memory");
}

unsigned long kernel_pgd_pa(void) {
    return virt_to_phys((unsigned long)g_kernel_pgd);
}

void switch_vm(unsigned long pgd_pa) {
    if (!pgd_pa)
        pgd_pa = kernel_pgd_pa();
    asm volatile(
        "csrw satp, %0\n"
        "sfence.vma zero, zero\n"
        :
        : "r"(MAKE_SATP(pgd_pa))
        : "memory"
    );
}

static unsigned long *next_table(unsigned long *table, unsigned long va,
                                 int level, int alloc_table) {
    unsigned long idx = vpn_index(va, level);
    unsigned long pte = table[idx];
    unsigned long *next;

    if (pte & PTE_V)
        return (unsigned long *)phys_to_virt(pte_pa(pte));

    if (!alloc_table)
        return 0;

    next = (unsigned long *)alloc(PAGE_SIZE);
    if (!next)
        return 0;
    zero_page(next);
    table[idx] = make_pte(virt_to_phys((unsigned long)next), PTE_V);
    return next;
}

int map_pages(unsigned long *pgd, unsigned long va, unsigned long size,
              unsigned long pa, unsigned long prot) {
    unsigned long start;
    unsigned long end;

    if (!pgd || size == 0)
        return 0;

    start = align_down(va, PAGE_SIZE);
    end = align_up(va + size, PAGE_SIZE);
    pa = align_down(pa, PAGE_SIZE);

    while (start < end) {
        unsigned long *pmd = next_table(pgd, start, 2, 1);
        unsigned long *pte = pmd ? next_table(pmd, start, 1, 1) : 0;

        if (!pte)
            return 0;
        pte[vpn_index(start, 0)] = make_pte(pa, prot);
        start += PAGE_SIZE;
        pa += PAGE_SIZE;
    }

    asm volatile("sfence.vma zero, zero" ::: "memory");
    return 1;
}

void unmap_pages(unsigned long *pgd, unsigned long va, unsigned long size) {
    unsigned long start;
    unsigned long end;

    if (!pgd || size == 0)
        return;

    start = align_down(va, PAGE_SIZE);
    end = align_up(va + size, PAGE_SIZE);
    while (start < end) {
        unsigned long *pmd = next_table(pgd, start, 2, 0);
        unsigned long *pte = pmd ? next_table(pmd, start, 1, 0) : 0;

        if (pte)
            pte[vpn_index(start, 0)] = 0;
        start += PAGE_SIZE;
    }
    asm volatile("sfence.vma zero, zero" ::: "memory");
}

unsigned long vm_translate(unsigned long *pgd, unsigned long va) {
    unsigned long *table = pgd;
    int level;

    if (!table)
        return 0;

    for (level = 2; level >= 0; level--) {
        unsigned long pte = table[vpn_index(va, level)];

        if (!(pte & PTE_V))
            return 0;
        if (pte_is_leaf(pte)) {
            unsigned long off_mask = (1UL << (12 + 9 * level)) - 1UL;
            return pte_pa(pte) + (va & off_mask);
        }
        table = (unsigned long *)phys_to_virt(pte_pa(pte));
    }

    return 0;
}

unsigned long *vm_create_user_pgd(void) {
    unsigned long *pgd = (unsigned long *)alloc(PAGE_SIZE);
    unsigned long i;

    if (!pgd)
        return 0;

    zero_page(pgd);
    for (i = PGD_KERNEL_BASE; i < PT_ENTRIES; i++)
        pgd[i] = g_kernel_pgd[i];
    return pgd;
}

static void free_low_half_tables(unsigned long *table, int level) {
    unsigned long i;
    unsigned long limit = (level == 2) ? PGD_KERNEL_BASE : PT_ENTRIES;

    for (i = 0; i < limit; i++) {
        unsigned long pte = table[i];

        if (!(pte & PTE_V) || pte_is_leaf(pte))
            continue;
        free_low_half_tables((unsigned long *)phys_to_virt(pte_pa(pte)),
                             level - 1);
        free((void *)phys_to_virt(pte_pa(pte)));
        table[i] = 0;
    }
}

void vm_free_user_pgd(unsigned long *pgd) {
    if (!pgd)
        return;
    free_low_half_tables(pgd, 2);
    free(pgd);
}

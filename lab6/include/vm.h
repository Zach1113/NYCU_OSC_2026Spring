#ifndef KERNEL_VM_H
#define KERNEL_VM_H

#define PAGE_SIZE       4096UL
#ifndef USER_STACK_SIZE
#define USER_STACK_SIZE 0x8000UL
#endif
#define KERNEL_OFFSET   0xffffffc000000000UL
#define KERNEL_LOAD_PA  0x00200000UL
#define USER_STACK_TOP  0x0000004000000000UL
#define USER_STACK_BASE (USER_STACK_TOP - USER_STACK_SIZE)
#define USER_SIGNAL_STACK_TOP  USER_STACK_BASE
#define USER_SIGNAL_STACK_BASE (USER_SIGNAL_STACK_TOP - USER_STACK_SIZE)
#define USER_MMAP_BASE  0x0000002000000000UL

#define PTE_V  (1UL << 0)
#define PTE_R  (1UL << 1)
#define PTE_W  (1UL << 2)
#define PTE_X  (1UL << 3)
#define PTE_U  (1UL << 4)
#define PTE_G  (1UL << 5)
#define PTE_A  (1UL << 6)
#define PTE_D  (1UL << 7)

#define PROT_KERNEL (PTE_V | PTE_R | PTE_W | PTE_X | PTE_G | PTE_A | PTE_D)
#define PROT_MMIO   (PTE_V | PTE_R | PTE_W | PTE_G | PTE_A | PTE_D)
#define PROT_USER_RWX (PTE_V | PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D)
#define PROT_USER_RW  (PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D)

#define SATP_SV39       (8UL << 60)
#define MAKE_SATP(pa)   (SATP_SV39 | ((unsigned long)(pa) >> 12))

static inline unsigned long phys_to_virt(unsigned long pa) {
    return pa + KERNEL_OFFSET;
}

static inline unsigned long virt_to_phys(unsigned long va) {
    if (va >= KERNEL_OFFSET)
        return va - KERNEL_OFFSET;
    return va;
}

void setup_vm(unsigned long fdt_pa);
void drop_identity_map(void);
unsigned long kernel_pgd_pa(void);
void switch_vm(unsigned long pgd_pa);

unsigned long *vm_create_user_pgd(void);
void vm_free_user_pgd(unsigned long *pgd);
int map_pages(unsigned long *pgd, unsigned long va, unsigned long size,
              unsigned long pa, unsigned long prot);
void unmap_pages(unsigned long *pgd, unsigned long va, unsigned long size);
unsigned long vm_translate(unsigned long *pgd, unsigned long va);

#endif

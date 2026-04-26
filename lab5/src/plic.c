#include "plic.h"

#include "fdt.h"

#define PLIC_BASE_DEFAULT 0xE0000000UL

#define PLIC_PRIORITY(irq)      (g_plic_base + ((unsigned long)(irq) * 4UL))
#define PLIC_ENABLE_BASE        (g_plic_base + 0x2000UL)
#define PLIC_ENABLE_STRIDE      0x80UL
#define PLIC_ENABLE_WORD(ctx, i) (PLIC_ENABLE_BASE + (unsigned long)(ctx) * PLIC_ENABLE_STRIDE + (unsigned long)(i) * 4UL)
#define PLIC_CONTEXT_BASE       (g_plic_base + 0x200000UL)
#define PLIC_CONTEXT_STRIDE     0x1000UL
#define PLIC_THRESHOLD(ctx)     (PLIC_CONTEXT_BASE + (unsigned long)(ctx) * PLIC_CONTEXT_STRIDE + 0x0UL)
#define PLIC_CLAIM(ctx)         (PLIC_CONTEXT_BASE + (unsigned long)(ctx) * PLIC_CONTEXT_STRIDE + 0x4UL)

/* Context 1 corresponds to S-mode external interrupt context on hart 0. */
#define PLIC_SMODE_CONTEXT 1

static unsigned long g_plic_base = PLIC_BASE_DEFAULT;

static unsigned int mmio_read32(unsigned long addr) {
    return *(volatile unsigned int *)addr;
}

static void mmio_write32(unsigned long addr, unsigned int v) {
    *(volatile unsigned int *)addr = v;
}

void plic_init_from_dtb(const void *fdt) {
    int off;
    int len = 0;
    const void *prop;

    g_plic_base = PLIC_BASE_DEFAULT;

    if (!fdt)
        return;

    off = fdt_path_offset(fdt, "/soc/interrupt-controller");
    if (off < 0)
        off = fdt_path_offset(fdt, "/soc/interrupt-controller@e0000000");
    if (off < 0)
        return;

    prop = fdt_getprop(fdt, off, "reg", &len);
    if (!prop)
        return;

    if (len >= 8)
        g_plic_base = fdt_be64(prop);
    else if (len >= 4)
        g_plic_base = fdt_be32(prop);
}

void plic_set_priority(int irq, int priority) {
    if (irq <= 0)
        return;
    mmio_write32(PLIC_PRIORITY(irq), (unsigned int)priority);
}

void plic_enable_irq(int irq) {
    unsigned long word;
    unsigned int mask;
    unsigned int val;

    if (irq <= 0)
        return;

    word = (unsigned long)irq / 32UL;
    mask = 1U << (irq % 32);
    val = mmio_read32(PLIC_ENABLE_WORD(PLIC_SMODE_CONTEXT, word));
    val |= mask;
    mmio_write32(PLIC_ENABLE_WORD(PLIC_SMODE_CONTEXT, word), val);
}

void plic_set_threshold(int threshold) {
    mmio_write32(PLIC_THRESHOLD(PLIC_SMODE_CONTEXT), (unsigned int)threshold);
}

int plic_claim(void) {
    return (int)mmio_read32(PLIC_CLAIM(PLIC_SMODE_CONTEXT));
}

void plic_complete(int irq) {
    if (irq <= 0)
        return;
    mmio_write32(PLIC_CLAIM(PLIC_SMODE_CONTEXT), (unsigned int)irq);
}

void plic_init(void) {
    plic_set_threshold(0);
}

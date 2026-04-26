#ifndef PLIC_H
#define PLIC_H

void plic_init_from_dtb(const void *fdt);
void plic_init(void);
void plic_enable_irq(int irq);
void plic_set_priority(int irq, int priority);
void plic_set_threshold(int threshold);
int plic_claim(void);
void plic_complete(int irq);

#endif
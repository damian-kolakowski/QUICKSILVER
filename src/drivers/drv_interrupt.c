#include "drv_interrupt.h"

void interrupt_enable(IRQn_Type irq, uint32_t prio) {
  NVIC_SetPriorityGrouping(3);
  NVIC_SetPriority(irq, prio);
  NVIC_EnableIRQ(irq);
}

void interrupt_disable(IRQn_Type irq) {
  NVIC_DisableIRQ(irq);
}
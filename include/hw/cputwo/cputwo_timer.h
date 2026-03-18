/*
 *  CPUTwo Timer
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CPUTWO_TIMER_H
#define HW_CPUTWO_TIMER_H

#include "hw/sysbus.h"

/* Register offsets */
#define CPUTWO_TIMER_PERIOD   0x00  /* Write: set period & start; Read: remaining */
#define CPUTWO_TIMER_CONTROL  0x04  /* bit 0 = enable, bit 1 = IRQ enable */

#define CPUTWO_TIMER_CTRL_EN      (1 << 0)
#define CPUTWO_TIMER_CTRL_IRQ_EN  (1 << 1)

#define TYPE_CPUTWO_TIMER "cputwo-timer"
OBJECT_DECLARE_SIMPLE_TYPE(CPUTwoTimerState, CPUTWO_TIMER)

struct CPUTwoTimerState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    QEMUTimer *timer;
    qemu_irq irq;

    uint32_t period;
    uint32_t count;
    uint32_t control;
    int64_t  last_tick_ns;  /* host time of last reload */
};

#endif

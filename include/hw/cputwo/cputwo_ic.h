/*
 *  CPUTwo Interrupt Controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CPUTWO_IC_H
#define HW_CPUTWO_IC_H

#include "hw/sysbus.h"

/* Register offsets */
#define CPUTWO_IC_PENDING    0x00   /* Read-only: latched pending bits */
#define CPUTWO_IC_ENABLE     0x04   /* R/W: enable mask */
#define CPUTWO_IC_ACK        0x08   /* Write: clear pending bits */

/* Interrupt source bits */
#define CPUTWO_IC_TIMER      (1 << 0)
#define CPUTWO_IC_UART_RX    (1 << 1)
#define CPUTWO_IC_UART_TX    (1 << 2)
#define CPUTWO_IC_BLK        (1 << 3)

#define CPUTWO_IC_NUM_INPUTS 4

#define TYPE_CPUTWO_IC "cputwo-ic"
OBJECT_DECLARE_SIMPLE_TYPE(CPUTwoICState, CPUTWO_IC)

struct CPUTwoICState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq output_irq;   /* single output to CPU */

    uint32_t pending;
    uint32_t enable;
};

#endif

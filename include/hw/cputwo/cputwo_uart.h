/*
 *  CPUTwo UART
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CPUTWO_UART_H
#define HW_CPUTWO_UART_H

#include "hw/sysbus.h"
#include "chardev/char-fe.h"

/* Register offsets */
#define CPUTWO_UART_STATUS   0x00
#define CPUTWO_UART_TX       0x04
#define CPUTWO_UART_RX       0x08
#define CPUTWO_UART_CONTROL  0x0C

/* Status register bits */
#define CPUTWO_UART_STATUS_TX_READY   (1 << 0)
#define CPUTWO_UART_STATUS_RX_AVAIL   (1 << 1)

/* Control register bits */
#define CPUTWO_UART_CTRL_RX_IRQ_EN    (1 << 0)
#define CPUTWO_UART_CTRL_TX_IRQ_EN    (1 << 1)

#define TYPE_CPUTWO_UART "cputwo-uart"
OBJECT_DECLARE_SIMPLE_TYPE(CPUTwoUartState, CPUTWO_UART)

struct CPUTwoUartState {
    /* <private> */
    SysBusDevice parent_obj;

    /* <public> */
    MemoryRegion mmio;
    qemu_irq irq_rx;
    qemu_irq irq_tx;

    uint32_t status;
    uint32_t control;
    uint8_t rx_byte;
    int rx_ready;

    CharFrontend chr;
};

#endif /* HW_CPUTWO_UART_H */

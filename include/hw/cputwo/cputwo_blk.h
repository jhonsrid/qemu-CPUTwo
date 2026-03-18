/*
 *  CPUTwo Block Device
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CPUTWO_BLK_H
#define HW_CPUTWO_BLK_H

#include "hw/sysbus.h"

/* Register offsets */
#define CPUTWO_BLK_SECTOR   0x00
#define CPUTWO_BLK_BUFFER   0x04
#define CPUTWO_BLK_COMMAND  0x08
#define CPUTWO_BLK_STATUS   0x0C
#define CPUTWO_BLK_CONTROL  0x10

/* Command values */
#define CPUTWO_BLK_CMD_READ   1
#define CPUTWO_BLK_CMD_WRITE  2

/* Status values */
#define CPUTWO_BLK_STATUS_IDLE  0
#define CPUTWO_BLK_STATUS_BUSY  1
#define CPUTWO_BLK_STATUS_ERROR 2

/* Control bits */
#define CPUTWO_BLK_CTRL_IRQ_EN  (1 << 0)

#define CPUTWO_BLK_SECTOR_SIZE  512

#define TYPE_CPUTWO_BLK "cputwo-blk"
OBJECT_DECLARE_SIMPLE_TYPE(CPUTwoBlkState, CPUTWO_BLK)

struct CPUTwoBlkState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;
    BlockBackend *blk;

    uint32_t sector;
    uint32_t buffer;
    uint32_t status;
    uint32_t control;
};

#endif

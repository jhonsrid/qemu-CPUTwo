/*
 *  CPUTwo Block Device
 *
 *  512-byte sector read/write device at 0x03F03000.
 *  Registers:
 *    +0x00 Sector  (r/w): sector number
 *    +0x04 Buffer  (r/w): guest physical address, must be 512-byte aligned
 *    +0x08 Command (write): 1 = read sector to buffer, 2 = write buffer to sector
 *    +0x0C Status  (read): 0 = idle, 1 = busy, 2 = error
 *    +0x10 Control (r/w): bit 0 = interrupt enable on completion
 *
 *  Commands complete synchronously (instantly) from the guest's perspective.
 *  Data is DMA'd between the block backend and guest memory.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/cputwo/cputwo_blk.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "system/block-backend.h"
#include "system/dma.h"
#include "qemu/log.h"

static void cputwo_blk_do_command(CPUTwoBlkState *s, uint32_t cmd)
{
    uint8_t buf[CPUTWO_BLK_SECTOR_SIZE];
    int ret;

    if (!s->blk) {
        qemu_log_mask(LOG_GUEST_ERROR, "cputwo_blk: no drive attached\n");
        s->status = CPUTWO_BLK_STATUS_ERROR;
        goto done;
    }

    if (s->buffer & (CPUTWO_BLK_SECTOR_SIZE - 1)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cputwo_blk: buffer 0x%08x not 512-byte aligned\n",
                      s->buffer);
        s->status = CPUTWO_BLK_STATUS_ERROR;
        goto done;
    }

    s->status = CPUTWO_BLK_STATUS_BUSY;

    int64_t offset = (int64_t)s->sector * CPUTWO_BLK_SECTOR_SIZE;

    if (cmd == CPUTWO_BLK_CMD_READ) {
        ret = blk_pread(s->blk, offset, CPUTWO_BLK_SECTOR_SIZE, buf, 0);
        if (ret < 0) {
            s->status = CPUTWO_BLK_STATUS_ERROR;
            goto done;
        }
        /* DMA write to guest memory */
        if (dma_memory_write(&address_space_memory, s->buffer, buf,
                             CPUTWO_BLK_SECTOR_SIZE,
                             MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            s->status = CPUTWO_BLK_STATUS_ERROR;
            goto done;
        }
        s->status = CPUTWO_BLK_STATUS_IDLE;
    } else if (cmd == CPUTWO_BLK_CMD_WRITE) {
        /* DMA read from guest memory */
        if (dma_memory_read(&address_space_memory, s->buffer, buf,
                            CPUTWO_BLK_SECTOR_SIZE,
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            s->status = CPUTWO_BLK_STATUS_ERROR;
            goto done;
        }
        ret = blk_pwrite(s->blk, offset, CPUTWO_BLK_SECTOR_SIZE, buf, 0);
        if (ret < 0) {
            s->status = CPUTWO_BLK_STATUS_ERROR;
            goto done;
        }
        s->status = CPUTWO_BLK_STATUS_IDLE;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cputwo_blk: unknown command %u\n", cmd);
        s->status = CPUTWO_BLK_STATUS_ERROR;
    }

done:
    /* Fire IRQ on completion if enabled */
    if (s->control & CPUTWO_BLK_CTRL_IRQ_EN) {
        qemu_irq_pulse(s->irq);
    }
}

static uint64_t cputwo_blk_read(void *opaque, hwaddr addr, unsigned size)
{
    CPUTwoBlkState *s = opaque;

    switch (addr) {
    case CPUTWO_BLK_SECTOR:
        return s->sector;
    case CPUTWO_BLK_BUFFER:
        return s->buffer;
    case CPUTWO_BLK_COMMAND:
        return 0; /* write-only */
    case CPUTWO_BLK_STATUS:
        return s->status;
    case CPUTWO_BLK_CONTROL:
        return s->control;
    case CPUTWO_BLK_SIZE:
        if (s->blk) {
            int64_t len = blk_getlength(s->blk);
            if (len > 0)
                return (uint32_t)(len / CPUTWO_BLK_SECTOR_SIZE);
        }
        return 0;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cputwo_blk: bad read 0x%" HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void cputwo_blk_write(void *opaque, hwaddr addr,
                              uint64_t data, unsigned size)
{
    CPUTwoBlkState *s = opaque;

    switch (addr) {
    case CPUTWO_BLK_SECTOR:
        s->sector = data;
        break;
    case CPUTWO_BLK_BUFFER:
        s->buffer = data;
        break;
    case CPUTWO_BLK_COMMAND:
        cputwo_blk_do_command(s, data);
        break;
    case CPUTWO_BLK_STATUS:
        /* read-only */
        break;
    case CPUTWO_BLK_CONTROL:
        s->control = data & 0x1;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cputwo_blk: bad write 0x%" HWADDR_PRIx "\n", addr);
        break;
    }
}

static const MemoryRegionOps cputwo_blk_ops = {
    .read = cputwo_blk_read,
    .write = cputwo_blk_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void cputwo_blk_reset(DeviceState *dev)
{
    CPUTwoBlkState *s = CPUTWO_BLK(dev);
    s->sector = 0;
    s->buffer = 0;
    s->status = CPUTWO_BLK_STATUS_IDLE;
    s->control = 0;
}

static void cputwo_blk_instance_init(Object *obj)
{
    CPUTwoBlkState *s = CPUTWO_BLK(obj);

    memory_region_init_io(&s->mmio, obj, &cputwo_blk_ops, s,
                          TYPE_CPUTWO_BLK, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void cputwo_blk_realize(DeviceState *dev, Error **errp)
{
    CPUTwoBlkState *s = CPUTWO_BLK(dev);

    if (s->blk) {
        /* Request read+write permissions */
        int ret = blk_set_perm(s->blk,
                               BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                               BLK_PERM_ALL, errp);
        if (ret < 0) {
            return;
        }
    }
}

static const Property cputwo_blk_properties[] = {
    DEFINE_PROP_DRIVE("drive", CPUTwoBlkState, blk),
};

static void cputwo_blk_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = cputwo_blk_realize;
    device_class_set_legacy_reset(dc, cputwo_blk_reset);
    device_class_set_props(dc, cputwo_blk_properties);
}

static const TypeInfo cputwo_blk_info = {
    .name = TYPE_CPUTWO_BLK,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CPUTwoBlkState),
    .class_init = cputwo_blk_class_init,
    .instance_init = cputwo_blk_instance_init,
};

static void cputwo_blk_register_types(void)
{
    type_register_static(&cputwo_blk_info);
}

type_init(cputwo_blk_register_types)

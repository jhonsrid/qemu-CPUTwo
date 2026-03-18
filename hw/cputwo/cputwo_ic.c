/*
 *  CPUTwo Interrupt Controller
 *
 *  4-source interrupt controller at 0x03F02000.
 *  Registers:
 *    +0x00 Pending  (read-only): latched interrupt bits
 *    +0x04 Enable   (r/w):      1 = source enabled
 *    +0x08 Ack      (write):    write bit(s) to clear pending
 *
 *  Input lines (directly set pending bits):
 *    0 = Timer, 1 = UART RX, 2 = UART TX, 3 = Block device
 *  Output: single line to CPU, asserted when (pending & enable) != 0.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/cputwo/cputwo_ic.h"
#include "hw/irq.h"
#include "qemu/log.h"

static void cputwo_ic_update(CPUTwoICState *s)
{
    int level = (s->pending & s->enable) != 0;
    qemu_set_irq(s->output_irq, level);
}

static void cputwo_ic_set_irq(void *opaque, int n, int level)
{
    CPUTwoICState *s = opaque;

    if (level) {
        s->pending |= (1u << n);
    }
    /* Pending bits are latched — only cleared by ACK write, not by
     * the source deasserting. */
    cputwo_ic_update(s);
}

static uint64_t cputwo_ic_read(void *opaque, hwaddr addr, unsigned size)
{
    CPUTwoICState *s = opaque;

    switch (addr) {
    case CPUTWO_IC_PENDING:
        return s->pending;
    case CPUTWO_IC_ENABLE:
        return s->enable;
    case CPUTWO_IC_ACK:
        /* ACK is write-only */
        return 0;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cputwo_ic: bad read offset 0x%" HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void cputwo_ic_write(void *opaque, hwaddr addr,
                             uint64_t data, unsigned size)
{
    CPUTwoICState *s = opaque;

    switch (addr) {
    case CPUTWO_IC_PENDING:
        /* Read-only */
        break;
    case CPUTWO_IC_ENABLE:
        s->enable = data & 0xF;
        cputwo_ic_update(s);
        break;
    case CPUTWO_IC_ACK:
        s->pending &= ~(data & 0xF);
        cputwo_ic_update(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cputwo_ic: bad write offset 0x%" HWADDR_PRIx "\n", addr);
        break;
    }
}

static const MemoryRegionOps cputwo_ic_ops = {
    .read = cputwo_ic_read,
    .write = cputwo_ic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void cputwo_ic_reset(DeviceState *dev)
{
    CPUTwoICState *s = CPUTWO_IC(dev);
    s->pending = 0;
    s->enable = 0;
}

static void cputwo_ic_instance_init(Object *obj)
{
    CPUTwoICState *s = CPUTWO_IC(obj);

    memory_region_init_io(&s->mmio, obj, &cputwo_ic_ops, s,
                          TYPE_CPUTWO_IC, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);

    /* 4 input IRQ lines (timer, uart_rx, uart_tx, blk) */
    qdev_init_gpio_in(DEVICE(obj), cputwo_ic_set_irq, CPUTWO_IC_NUM_INPUTS);

    /* 1 output IRQ line to CPU */
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->output_irq);
}

static void cputwo_ic_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, cputwo_ic_reset);
}

static const TypeInfo cputwo_ic_info = {
    .name = TYPE_CPUTWO_IC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CPUTwoICState),
    .class_init = cputwo_ic_class_init,
    .instance_init = cputwo_ic_instance_init,
};

static void cputwo_ic_register_types(void)
{
    type_register_static(&cputwo_ic_info);
}

type_init(cputwo_ic_register_types)

/*
 *  CPUTwo UART device
 *
 *  Memory-mapped serial console at 0x03F00000.
 *  Registers:
 *    +0x00 Status  (read):  bit 0 = TX ready, bit 1 = RX available
 *    +0x04 TX      (write): send low 8 bits as a byte
 *    +0x08 RX      (read):  consume buffered byte
 *    +0x0C Control  (r/w):  bit 0 = RX IRQ enable, bit 1 = TX IRQ enable
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/cputwo/cputwo_uart.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "qemu/log.h"

static void cputwo_uart_update_irq(CPUTwoUartState *s)
{
    int rx_irq = s->rx_ready && (s->control & CPUTWO_UART_CTRL_RX_IRQ_EN);
    qemu_set_irq(s->irq_rx, rx_irq);
}

static uint64_t cputwo_uart_read(void *opaque, hwaddr addr, unsigned size)
{
    CPUTwoUartState *s = opaque;

    switch (addr) {
    case CPUTWO_UART_STATUS:
        return CPUTWO_UART_STATUS_TX_READY |
               (s->rx_ready ? CPUTWO_UART_STATUS_RX_AVAIL : 0);

    case CPUTWO_UART_TX:
        /* TX is write-only */
        return 0;

    case CPUTWO_UART_RX:
        if (s->rx_ready) {
            uint8_t b = s->rx_byte;
            s->rx_ready = 0;
            cputwo_uart_update_irq(s);
            qemu_chr_fe_accept_input(&s->chr);
            return b;
        }
        return 0;

    case CPUTWO_UART_CONTROL:
        return s->control;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cputwo_uart: bad read offset 0x%" HWADDR_PRIx "\n",
                      addr);
        return 0;
    }
}

static void cputwo_uart_write(void *opaque, hwaddr addr,
                               uint64_t data, unsigned size)
{
    CPUTwoUartState *s = opaque;

    switch (addr) {
    case CPUTWO_UART_STATUS:
        /* Status is read-only */
        break;

    case CPUTWO_UART_TX: {
        uint8_t ch = data & 0xFF;
        qemu_chr_fe_write_all(&s->chr, &ch, 1);
        /* TX completes instantly; fire TX IRQ if enabled */
        if (s->control & CPUTWO_UART_CTRL_TX_IRQ_EN) {
            qemu_irq_pulse(s->irq_tx);
        }
        break;
    }

    case CPUTWO_UART_RX:
        /* RX is read-only */
        break;

    case CPUTWO_UART_CONTROL:
        s->control = data & 0x3;
        cputwo_uart_update_irq(s);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cputwo_uart: bad write offset 0x%" HWADDR_PRIx "\n",
                      addr);
        break;
    }
}

static const MemoryRegionOps cputwo_uart_ops = {
    .read = cputwo_uart_read,
    .write = cputwo_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static int cputwo_uart_can_receive(void *opaque)
{
    CPUTwoUartState *s = opaque;
    return s->rx_ready ? 0 : 1;
}

static void cputwo_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    CPUTwoUartState *s = opaque;

    s->rx_byte = buf[0];
    s->rx_ready = 1;
    cputwo_uart_update_irq(s);
}

static void cputwo_uart_reset(DeviceState *dev)
{
    CPUTwoUartState *s = CPUTWO_UART(dev);

    s->status = 0;
    s->control = 0;
    s->rx_byte = 0;
    s->rx_ready = 0;
}

static void cputwo_uart_realize(DeviceState *dev, Error **errp)
{
    CPUTwoUartState *s = CPUTWO_UART(dev);

    qemu_chr_fe_set_handlers(&s->chr, cputwo_uart_can_receive,
                             cputwo_uart_receive, NULL, NULL,
                             s, NULL, true);
}

static void cputwo_uart_instance_init(Object *obj)
{
    CPUTwoUartState *s = CPUTWO_UART(obj);

    memory_region_init_io(&s->mmio, obj, &cputwo_uart_ops, s,
                          TYPE_CPUTWO_UART, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);

    /* Two IRQ outputs: RX and TX */
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq_rx);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq_tx);
}

static const Property cputwo_uart_properties[] = {
    DEFINE_PROP_CHR("chardev", CPUTwoUartState, chr),
};

static void cputwo_uart_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, cputwo_uart_reset);
    dc->realize = cputwo_uart_realize;
    device_class_set_props(dc, cputwo_uart_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo cputwo_uart_info = {
    .name = TYPE_CPUTWO_UART,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CPUTwoUartState),
    .class_init = cputwo_uart_class_init,
    .instance_init = cputwo_uart_instance_init,
};

static void cputwo_uart_register_types(void)
{
    type_register_static(&cputwo_uart_info);
}

type_init(cputwo_uart_register_types)

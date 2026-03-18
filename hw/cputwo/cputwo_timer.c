/*
 *  CPUTwo Timer
 *
 *  Periodic countdown timer at 0x03F01000.
 *  Registers:
 *    +0x00 Period/Count: write sets period & starts; read returns remaining
 *    +0x04 Control:      bit 0 = enable, bit 1 = IRQ enable
 *
 *  One "tick" = one instruction in the CPUTwo spec.
 *  We approximate this using a virtual-clock timer with a configurable
 *  nanoseconds-per-tick rate. Default: 100ns/tick = 10 MIPS.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/cputwo/cputwo_timer.h"
#include "hw/irq.h"
#include "qemu/log.h"
#include "qemu/timer.h"

/* Nanoseconds per tick. 100ns = 10 million instructions/sec. */
#define NS_PER_TICK 100

static void cputwo_timer_fire(void *opaque)
{
    CPUTwoTimerState *s = opaque;

    /* Timer reached zero — reload and fire IRQ */
    if (s->control & CPUTWO_TIMER_CTRL_IRQ_EN) {
        qemu_irq_pulse(s->irq);
    }

    /* Reload and reschedule (periodic) */
    if (s->control & CPUTWO_TIMER_CTRL_EN) {
        s->count = s->period;
        s->last_tick_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        timer_mod(s->timer,
                  s->last_tick_ns + (int64_t)s->period * NS_PER_TICK);
    }
}

static uint64_t cputwo_timer_read(void *opaque, hwaddr addr, unsigned size)
{
    CPUTwoTimerState *s = opaque;

    switch (addr) {
    case CPUTWO_TIMER_PERIOD: {
        /* Return approximate remaining count */
        if (!(s->control & CPUTWO_TIMER_CTRL_EN) || s->period == 0) {
            return s->count;
        }
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        int64_t elapsed_ticks = (now - s->last_tick_ns) / NS_PER_TICK;
        if (elapsed_ticks >= s->period) {
            return 0;
        }
        return s->period - (uint32_t)elapsed_ticks;
    }
    case CPUTWO_TIMER_CONTROL:
        return s->control;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cputwo_timer: bad read offset 0x%" HWADDR_PRIx "\n",
                      addr);
        return 0;
    }
}

static void cputwo_timer_write(void *opaque, hwaddr addr,
                                uint64_t data, unsigned size)
{
    CPUTwoTimerState *s = opaque;

    switch (addr) {
    case CPUTWO_TIMER_PERIOD:
        s->period = (uint32_t)data;
        s->count = s->period;
        /* Start counting if enabled */
        if ((s->control & CPUTWO_TIMER_CTRL_EN) && s->period > 0) {
            s->last_tick_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            timer_mod(s->timer,
                      s->last_tick_ns + (int64_t)s->period * NS_PER_TICK);
        }
        break;

    case CPUTWO_TIMER_CONTROL: {
        uint32_t old = s->control;
        s->control = data & 0x3;

        if ((s->control & CPUTWO_TIMER_CTRL_EN) && s->period > 0) {
            if (!(old & CPUTWO_TIMER_CTRL_EN)) {
                /* Just enabled — start counting from period */
                s->count = s->period;
                s->last_tick_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                timer_mod(s->timer,
                          s->last_tick_ns + (int64_t)s->period * NS_PER_TICK);
            }
        } else {
            /* Disabled — stop timer */
            timer_del(s->timer);
        }
        break;
    }

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cputwo_timer: bad write offset 0x%" HWADDR_PRIx "\n",
                      addr);
        break;
    }
}

static const MemoryRegionOps cputwo_timer_ops = {
    .read = cputwo_timer_read,
    .write = cputwo_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void cputwo_timer_reset(DeviceState *dev)
{
    CPUTwoTimerState *s = CPUTWO_TIMER(dev);
    s->period = 0;
    s->count = 0;
    s->control = 0;
    s->last_tick_ns = 0;
    timer_del(s->timer);
}

static void cputwo_timer_realize(DeviceState *dev, Error **errp)
{
    CPUTwoTimerState *s = CPUTWO_TIMER(dev);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cputwo_timer_fire, s);
}

static void cputwo_timer_instance_init(Object *obj)
{
    CPUTwoTimerState *s = CPUTWO_TIMER(obj);

    memory_region_init_io(&s->mmio, obj, &cputwo_timer_ops, s,
                          TYPE_CPUTWO_TIMER, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void cputwo_timer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, cputwo_timer_reset);
    dc->realize = cputwo_timer_realize;
}

static const TypeInfo cputwo_timer_info = {
    .name = TYPE_CPUTWO_TIMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CPUTwoTimerState),
    .class_init = cputwo_timer_class_init,
    .instance_init = cputwo_timer_instance_init,
};

static void cputwo_timer_register_types(void)
{
    type_register_static(&cputwo_timer_info);
}

type_init(cputwo_timer_register_types)

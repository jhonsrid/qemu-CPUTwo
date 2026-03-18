/*
 *  CPUTwo Supervisor Registers
 *
 *  Memory-mapped CPU control registers at 0x03FFF000.
 *  User-mode access raises bus error (cause 0x02) — this is handled by
 *  the TLB/MMU layer; this device just does the actual reads/writes.
 *
 *  Offsets:
 *    0x00 EPC       r/w
 *    0x04 EFLAGS    r/w
 *    0x08 EVEC      r/w
 *    0x0C CAUSE     r/o
 *    0x10 STATUS    r/w
 *    0x14 ESTATUS   r/w
 *    0x18 SATP      r/w  (write flushes TLB)
 *    0x1C BADADDR   r/o
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/cputwo/cputwo_svreg.h"
#include "exec/cputlb.h"
#include "qemu/log.h"

static uint64_t cputwo_svreg_read(void *opaque, hwaddr addr, unsigned size)
{
    CPUTwoSvregState *s = opaque;
    CPUTwoState *env = s->cpu_env;

    /* User-mode access check: the architecture says user-mode access to
     * these addresses raises a bus error. This should already be enforced
     * by the MMU/TLB layer, but double-check here. */
    if (!(env->status & STATUS_PRIV)) {
        /* Should not happen if MMU is working correctly */
        qemu_log_mask(LOG_GUEST_ERROR, "cputwo_svreg: user-mode read\n");
        return 0;
    }

    switch (addr) {
    case 0x00: return env->epc;
    case 0x04: return env->eflags;
    case 0x08: return env->evec;
    case 0x0C: return env->cause;
    case 0x10: return env->status;
    case 0x14: return env->estatus;
    case 0x18: return env->satp;
    case 0x1C: return env->badaddr;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cputwo_svreg: bad read 0x%" HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void cputwo_svreg_write(void *opaque, hwaddr addr,
                                uint64_t data, unsigned size)
{
    CPUTwoSvregState *s = opaque;
    CPUTwoState *env = s->cpu_env;

    if (!(env->status & STATUS_PRIV)) {
        qemu_log_mask(LOG_GUEST_ERROR, "cputwo_svreg: user-mode write\n");
        return;
    }

    switch (addr) {
    case 0x00: env->epc = data; break;
    case 0x04: env->eflags = data; break;
    case 0x08: env->evec = data; break;
    case 0x0C: break; /* CAUSE: read-only */
    case 0x10: env->status = data; break;
    case 0x14: env->estatus = data; break;
    case 0x18:
        env->satp = data;
        /* Writing SATP flushes non-global TLB entries */
        tlb_flush(env_cpu(env));
        break;
    case 0x1C: break; /* BADADDR: read-only */
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "cputwo_svreg: bad write 0x%" HWADDR_PRIx "\n", addr);
        break;
    }
}

static const MemoryRegionOps cputwo_svreg_ops = {
    .read = cputwo_svreg_read,
    .write = cputwo_svreg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void cputwo_svreg_instance_init(Object *obj)
{
    CPUTwoSvregState *s = CPUTWO_SVREG(obj);

    memory_region_init_io(&s->mmio, obj, &cputwo_svreg_ops, s,
                          TYPE_CPUTWO_SVREG, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void cputwo_svreg_class_init(ObjectClass *klass, const void *data)
{
    /* No special class init needed */
}

static const TypeInfo cputwo_svreg_info = {
    .name = TYPE_CPUTWO_SVREG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CPUTwoSvregState),
    .class_init = cputwo_svreg_class_init,
    .instance_init = cputwo_svreg_instance_init,
};

static void cputwo_svreg_register_types(void)
{
    type_register_static(&cputwo_svreg_info);
}

type_init(cputwo_svreg_register_types)

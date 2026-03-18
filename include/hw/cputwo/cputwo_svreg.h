/*
 *  CPUTwo Supervisor Registers (memory-mapped CPU control)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CPUTWO_SVREG_H
#define HW_CPUTWO_SVREG_H

#include "hw/sysbus.h"
#include "target/cputwo/cpu.h"

#define TYPE_CPUTWO_SVREG "cputwo-svreg"
OBJECT_DECLARE_SIMPLE_TYPE(CPUTwoSvregState, CPUTWO_SVREG)

struct CPUTwoSvregState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    CPUTwoState *cpu_env;   /* pointer to CPU state */
};

#endif

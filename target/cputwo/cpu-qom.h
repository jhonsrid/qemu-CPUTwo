/*
 *  QEMU CPUTwo CPU QOM header (target agnostic)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CPUTWO_CPU_QOM_H
#define CPUTWO_CPU_QOM_H

#include "hw/core/cpu.h"

#define TYPE_CPUTWO_CPU "cputwo-cpu"

OBJECT_DECLARE_CPU_TYPE(CPUTwoCPU, CPUTwoCPUClass, CPUTWO_CPU)

#endif

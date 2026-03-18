/*
 *  CPUTwo emulation definition
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CPUTWO_CPU_H
#define CPUTWO_CPU_H

#include "cpu-qom.h"

#include "exec/cpu-common.h"
#include "exec/cpu-defs.h"
#include "exec/cpu-interrupt.h"

#ifdef CONFIG_USER_ONLY
#error "CPUTwo does not support user mode emulation"
#endif

/* Number of general-purpose registers (r0-r15) */
#define CPUTWO_NUM_REGS 16

/* Exception / interrupt cause codes */
#define EXCP_ILLEGAL      0x00
#define EXCP_MISALIGNED   0x01
#define EXCP_BUS          0x02
#define EXCP_SYSCALL      0x03
#define EXCP_DIVZERO      0x04
#define EXCP_HALT         0x05
#define EXCP_IRQ          0x06
#define EXCP_IFAULT       0x07
#define EXCP_LFAULT       0x08
#define EXCP_SFAULT       0x09

/* Flags register bit positions */
#define FLAG_N_BIT  31
#define FLAG_Z_BIT  30
#define FLAG_C_BIT  29
#define FLAG_V_BIT  28
#define FLAG_N  (1u << FLAG_N_BIT)
#define FLAG_Z  (1u << FLAG_Z_BIT)
#define FLAG_C  (1u << FLAG_C_BIT)
#define FLAG_V  (1u << FLAG_V_BIT)

/* STATUS register bits */
#define STATUS_PRIV  (1u << 0)   /* 1 = supervisor */
#define STATUS_IE    (1u << 1)   /* 1 = interrupts enabled */

/* Memory map constants */
#define CPUTWO_MEM_SIZE     0x04000000u   /* 64 MB */
#define CPUTWO_MMIO_BASE    0x03F00000u
#define CPUTWO_SV_BASE      0x03FFF000u

/* MMU index */
#define MMU_IDX_USER       0
#define MMU_IDX_SUPERVISOR 1

typedef struct CPUArchState {
    /* CPU registers */
    uint32_t r[CPUTWO_NUM_REGS];  /* r0-r12 GPR, r13=sp, r14=lr, r15=pc */
    uint32_t flags;                /* N(31) Z(30) C(29) V(28) */

    /* Supervisor registers (memory-mapped at 0x03FFF000 in guest) */
    uint32_t epc;
    uint32_t eflags;
    uint32_t evec;
    uint32_t cause;
    uint32_t status;       /* bit0=priv(1=sv), bit1=IE */
    uint32_t estatus;
    uint32_t satp;         /* bit31=EN, bits[19:0]=PPN */
    uint32_t badaddr;

    /* Fields up to this point are cleared by a CPU reset */
    struct {} end_reset_fields;

    /* Internal state */
    uint32_t in_halt;
} CPUTwoState;

/*
 * CPUTwoCPU:
 * @env: #CPUTwoState
 *
 * A CPUTwo CPU
 */
struct ArchCPU {
    CPUState parent_obj;

    CPUTwoState env;
};

/*
 * CPUTwoCPUClass:
 * @parent_realize: The parent class' realize handler.
 * @parent_phases: The parent class' reset phase handlers.
 *
 * A CPUTwo CPU model.
 */
struct CPUTwoCPUClass {
    CPUClass parent_class;

    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
};

#define CPU_RESOLVING_TYPE TYPE_CPUTWO_CPU

void cputwo_cpu_do_interrupt(CPUState *cpu);
bool cputwo_cpu_exec_interrupt(CPUState *cpu, int int_req);
hwaddr cputwo_cpu_get_phys_page_debug(CPUState *cpu, vaddr addr);
void cputwo_cpu_dump_state(CPUState *cpu, FILE *f, int flags);
int cputwo_cpu_gdb_read_register(CPUState *cpu, GByteArray *buf, int reg);
int cputwo_cpu_gdb_write_register(CPUState *cpu, uint8_t *buf, int reg);

void cputwo_translate_init(void);
void cputwo_translate_code(CPUState *cs, TranslationBlock *tb,
                           int *max_insns, vaddr pc, void *host_pc);

#define CPUTWO_CPU_IRQ 0

#endif /* CPUTWO_CPU_H */

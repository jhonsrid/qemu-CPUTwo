/*
 *  CPUTwo helper functions
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "cpu.h"
#include "exec/log.h"
#include "system/memory.h"
#include "accel/tcg/cpu-ldst.h"
#include "hw/irq.h"
#include "qemu/plugin.h"

void cputwo_cpu_do_interrupt(CPUState *cs)
{
    CPUTwoState *env = cpu_env(cs);
    int excp = cs->exception_index;
    uint64_t last_pc = env->r[15];

    env->in_halt = 0;

    if (excp == EXCP_HALT) {
        cs->halted = 1;
        env->in_halt = 1;
        qemu_plugin_vcpu_exception_cb(cs, last_pc);
        qemu_log_mask(CPU_LOG_INT, "halt\n");
        return;
    }

    /* Standard exception entry sequence */
    env->estatus = env->status;
    if (excp == EXCP_SYSCALL) {
        /* SYSCALL: EPC = PC+4 (return address past SYSCALL) */
        env->epc = env->r[15];
    } else if (excp == EXCP_IRQ) {
        /* Hardware interrupt: EPC = next instruction to execute */
        env->epc = env->r[15];
    } else {
        /* Faults: EPC = faulting instruction address */
        env->epc = env->r[15];
    }
    env->eflags = env->flags;
    env->cause = excp;
    env->status = STATUS_PRIV; /* supervisor mode, IE=0 */

    /* Jump to handler via exception vector table */
    uint32_t vec_addr = env->evec + excp * 4;
    if (vec_addr + 3 < CPUTWO_MEM_SIZE) {
        env->r[15] = cpu_ldl_data(env, vec_addr);
    } else {
        env->r[15] = 0;
    }

    qemu_log_mask(CPU_LOG_INT,
                  "exception %d (cause=0x%02x) at pc=0x%08x -> handler=0x%08x\n",
                  excp, excp, (uint32_t)last_pc, env->r[15]);

    if (excp == EXCP_IRQ) {
        qemu_plugin_vcpu_interrupt_cb(cs, last_pc);
    } else {
        qemu_plugin_vcpu_exception_cb(cs, last_pc);
    }
}

bool cputwo_cpu_exec_interrupt(CPUState *cs, int interrupt_request)
{
    CPUTwoState *env = cpu_env(cs);

    if ((interrupt_request & CPU_INTERRUPT_HARD) &&
        (env->status & STATUS_IE)) {
        cs->exception_index = EXCP_IRQ;
        cputwo_cpu_do_interrupt(cs);
        return true;
    }
    return false;
}

hwaddr cputwo_cpu_get_phys_page_debug(CPUState *cs, vaddr addr)
{
    CPUTwoState *env = cpu_env(cs);

    /* If MMU disabled, identity mapping */
    if (!(env->satp & 0x80000000u)) {
        return addr;
    }

    /* MMIO bypass */
    if (addr >= CPUTWO_MMIO_BASE) {
        return addr;
    }

    /* Simple page table walk for debug */
    uint32_t va = (uint32_t)addr;
    uint32_t l1_base = (env->satp & 0x000FFFFFu) << 12;
    uint32_t vpn1 = (va >> 22) & 0x3FFu;
    uint32_t vpn0 = (va >> 12) & 0x3FFu;

    uint32_t l1_pte_addr = l1_base + vpn1 * 4;
    if (l1_pte_addr + 3 >= CPUTWO_MEM_SIZE) {
        return -1;
    }
    uint32_t l1_pte = ldl_le_phys(cs->as, l1_pte_addr);
    if (!(l1_pte & 1u)) {
        return -1;
    }

    /* Superpage check */
    if (l1_pte & (7u << 5)) {
        uint32_t ppn = (l1_pte >> 12) & 0xFFFFFu;
        return ((ppn & 0xFFC00u) << 12) | (va & 0x3FFFFFu);
    }

    uint32_t l2_base = ((l1_pte >> 12) & 0xFFFFFu) << 12;
    uint32_t l2_pte_addr = l2_base + vpn0 * 4;
    if (l2_pte_addr + 3 >= CPUTWO_MEM_SIZE) {
        return -1;
    }
    uint32_t l2_pte = ldl_le_phys(cs->as, l2_pte_addr);
    if (!(l2_pte & 1u)) {
        return -1;
    }

    uint32_t ppn2 = (l2_pte >> 12) & 0xFFFFFu;
    return (ppn2 << 12) | (va & 0xFFFu);
}

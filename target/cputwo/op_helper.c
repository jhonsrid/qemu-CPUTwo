/*
 *  CPUTwo op helpers (called from translated code)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "exec/cputlb.h"
#include "accel/tcg/cpu-ldst.h"

static inline G_NORETURN
void raise_exception(CPUTwoState *env, int index, uintptr_t retaddr)
{
    CPUState *cs = env_cpu(env);
    cs->exception_index = index;
    cpu_loop_exit_restore(cs, retaddr);
}

/*
 * Illegal instruction — GETPC() lets cpu_loop_exit_restore roll back
 * r[15] to the faulting instruction's PC (via insn_start data).
 */
G_NORETURN void helper_cputwo_illegal(CPUTwoState *env)
{
    raise_exception(env, EXCP_ILLEGAL, GETPC());
}

/*
 * HALT — stops the CPU.  retaddr=0 so r[15] stays at its current
 * value (already advanced past the HALT instruction).
 */
G_NORETURN void helper_cputwo_halt(CPUTwoState *env)
{
    CPUState *cs = env_cpu(env);
    cs->halted = 1;
    env->in_halt = 1;
    raise_exception(env, EXCP_HALT, 0);
}

/*
 * SYSCALL — translator has already advanced r[15] to PC+4.
 * retaddr=0: don't roll back, so EPC in do_interrupt = PC+4.
 */
G_NORETURN void helper_cputwo_syscall(CPUTwoState *env)
{
    raise_exception(env, EXCP_SYSCALL, 0);
}

/*
 * SYSRET — return to user mode.
 *   PC = EPC, flags = EFLAGS, STATUS = ESTATUS & ~1 (force user)
 * Must exit the TB because privilege mode (and thus MMU index) changes.
 */
G_NORETURN void helper_cputwo_sysret(CPUTwoState *env)
{
    if (!(env->status & STATUS_PRIV)) {
        raise_exception(env, EXCP_ILLEGAL, GETPC());
    }
    env->r[15] = env->epc;
    env->flags = env->eflags;
    env->status = env->estatus & ~STATUS_PRIV; /* force user mode */

    CPUState *cs = env_cpu(env);
    cpu_loop_exit(cs);
}

/*
 * KRET — kernel return.
 *   PC = EPC, flags = EFLAGS, STATUS = ESTATUS (full restore)
 * Must exit the TB because privilege mode and IE may change.
 */
G_NORETURN void helper_cputwo_kret(CPUTwoState *env)
{
    if (!(env->status & STATUS_PRIV)) {
        raise_exception(env, EXCP_ILLEGAL, GETPC());
    }
    env->r[15] = env->epc;
    env->flags = env->eflags;
    env->status = env->estatus; /* restore full status including priv bit */

    CPUState *cs = env_cpu(env);
    cpu_loop_exit(cs);
}

/*
 * SFENCE — flush non-global TLB entries.  Supervisor mode only.
 */
void helper_cputwo_sfence(CPUTwoState *env)
{
    CPUState *cs = env_cpu(env);
    if (!(env->status & STATUS_PRIV)) {
        raise_exception(env, EXCP_ILLEGAL, GETPC());
    }
    tlb_flush(cs);
}

/*
 * Division / modulo helpers — raise EXCP_DIVZERO on zero divisor.
 * GETPC() is used so cpu_loop_exit_restore rolls back r[15] to the
 * faulting instruction (allowing the OS to report the correct EPC).
 */
uint32_t helper_cputwo_div(CPUTwoState *env, uint32_t num, uint32_t den)
{
    if (den == 0) {
        raise_exception(env, EXCP_DIVZERO, GETPC());
    }
    return (uint32_t)((int32_t)num / (int32_t)den);
}

uint32_t helper_cputwo_divu(CPUTwoState *env, uint32_t num, uint32_t den)
{
    if (den == 0) {
        raise_exception(env, EXCP_DIVZERO, GETPC());
    }
    return num / den;
}

uint32_t helper_cputwo_mod(CPUTwoState *env, uint32_t num, uint32_t den)
{
    if (den == 0) {
        raise_exception(env, EXCP_DIVZERO, GETPC());
    }
    return (uint32_t)((int32_t)num % (int32_t)den);
}

uint32_t helper_cputwo_modu(CPUTwoState *env, uint32_t num, uint32_t den)
{
    if (den == 0) {
        raise_exception(env, EXCP_DIVZERO, GETPC());
    }
    return num % den;
}

/*
 * CAS — atomic compare-and-swap.
 * tmp = mem32[addr]; if tmp == rd then mem32[addr] = rs2, Z=1
 *                    else rd = tmp, Z=0
 */
void helper_cputwo_cas(CPUTwoState *env, uint32_t addr,
                        uint32_t rd_idx, uint32_t rs2_idx)
{
    if (addr & 3) {
        raise_exception(env, EXCP_MISALIGNED, GETPC());
    }
    uint32_t cur = cpu_ldl_data_ra(env, addr, GETPC());
    if (cur == env->r[rd_idx]) {
        cpu_stl_data_ra(env, addr, env->r[rs2_idx], GETPC());
        env->flags |= FLAG_Z;
    } else {
        env->r[rd_idx] = cur;
        env->flags &= ~FLAG_Z;
    }
}

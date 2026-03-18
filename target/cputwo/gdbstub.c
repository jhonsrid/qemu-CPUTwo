/*
 *  CPUTwo GDB server stub
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "cpu.h"
#include "gdbstub/helpers.h"

int cputwo_cpu_gdb_read_register(CPUState *cs, GByteArray *mem_buf, int n)
{
    CPUTwoState *env = cpu_env(cs);

    switch (n) {
    case 0 ... 15:
        return gdb_get_regl(mem_buf, env->r[n]);
    case 16:
        return gdb_get_regl(mem_buf, env->flags);
    case 17:
        return gdb_get_regl(mem_buf, env->status);
    case 18:
        return gdb_get_regl(mem_buf, env->epc);
    case 19:
        return gdb_get_regl(mem_buf, env->eflags);
    case 20:
        return gdb_get_regl(mem_buf, env->evec);
    case 21:
        return gdb_get_regl(mem_buf, env->cause);
    case 22:
        return gdb_get_regl(mem_buf, env->estatus);
    case 23:
        return gdb_get_regl(mem_buf, env->satp);
    case 24:
        return gdb_get_regl(mem_buf, env->badaddr);
    }
    return 0;
}

int cputwo_cpu_gdb_write_register(CPUState *cs, uint8_t *mem_buf, int n)
{
    CPUTwoState *env = cpu_env(cs);

    switch (n) {
    case 0 ... 15:
        env->r[n] = ldl_p(mem_buf);
        break;
    case 16:
        env->flags = ldl_p(mem_buf);
        break;
    case 17:
        env->status = ldl_p(mem_buf);
        break;
    case 18:
        env->epc = ldl_p(mem_buf);
        break;
    case 19:
        env->eflags = ldl_p(mem_buf);
        break;
    case 20:
        env->evec = ldl_p(mem_buf);
        break;
    case 21:
        env->cause = ldl_p(mem_buf);
        break;
    case 22:
        env->estatus = ldl_p(mem_buf);
        break;
    case 23:
        env->satp = ldl_p(mem_buf);
        break;
    case 24:
        env->badaddr = ldl_p(mem_buf);
        break;
    default:
        return 0;
    }
    return 4;
}

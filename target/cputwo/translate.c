/*
 *  CPUTwo translation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/qemu-print.h"
#include "cpu.h"
#include "tcg/tcg-op.h"
#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#include "exec/translator.h"
#include "exec/translation-block.h"
#include "exec/log.h"

#define HELPER_H "helper.h"
#include "exec/helper-info.c.inc"
#undef  HELPER_H

typedef struct DisasContext {
    DisasContextBase base;
    CPUTwoState *env;
    uint32_t pc;     /* PC of current instruction */
    uint32_t tb_flags;
} DisasContext;

/* Target-specific values for dc->base.is_jmp */
#define DISAS_JUMP    DISAS_TARGET_0
#define DISAS_UPDATE  DISAS_TARGET_1
#define DISAS_EXIT    DISAS_TARGET_2

/* Global register TCG variables */
static TCGv_i32 cpu_r[CPUTWO_NUM_REGS];
static TCGv_i32 cpu_flags;
static TCGv_i32 cpu_status;
static TCGv_i32 cpu_epc, cpu_eflags, cpu_evec, cpu_cause;
static TCGv_i32 cpu_estatus, cpu_satp, cpu_badaddr;

#define cpu_pc cpu_r[15]

/* Include the auto-generated decoder */
#include "decode-insn.c.inc"

/*
 * CPUTwo allows r15 (PC) as the destination of any instruction.
 * Writing to r15 redirects control flow — the TB must end.
 */
static inline void check_rd_pc(DisasContext *ctx, int rd)
{
    if (rd == 15) {
        ctx->base.is_jmp = DISAS_JUMP;
    }
}

/* ── TCG goto_tb helper ───────────────────────────────────────────── */

static void gen_goto_tb(DisasContext *dc, unsigned tb_slot_idx, vaddr dest)
{
    if (translator_use_goto_tb(&dc->base, dest)) {
        tcg_gen_goto_tb(tb_slot_idx);
        tcg_gen_movi_i32(cpu_pc, dest);
        tcg_gen_exit_tb(dc->base.tb, tb_slot_idx);
    } else {
        tcg_gen_movi_i32(cpu_pc, dest);
        tcg_gen_lookup_and_goto_ptr();
    }
    dc->base.is_jmp = DISAS_NORETURN;
}

/* ── Flag update helpers ──────────────────────────────────────────── */

/*
 * Update N and Z flags from result, clear C and V.
 * Used by AND, OR, XOR, NOT, shifts, rotates.
 */
static void gen_update_flags_nz_clear_cv(TCGv_i32 result)
{
    TCGv_i32 f = tcg_temp_new_i32();

    /* Start with 0 */
    tcg_gen_movi_i32(f, 0);

    /* N = result[31] */
    TCGv_i32 n = tcg_temp_new_i32();
    tcg_gen_andi_i32(n, result, 0x80000000u);
    tcg_gen_or_i32(f, f, n); /* N is already in bit 31 */

    /* Z = (result == 0) ? FLAG_Z : 0 */
    TCGv_i32 z = tcg_temp_new_i32();
    tcg_gen_setcondi_i32(TCG_COND_EQ, z, result, 0);
    tcg_gen_shli_i32(z, z, FLAG_Z_BIT);
    tcg_gen_or_i32(f, f, z);

    /* C=0, V=0 already since f started as 0 */
    tcg_gen_mov_i32(cpu_flags, f);
}

/*
 * Update N and Z only (for MUL/MULH/MULHU/DIV/DIVU/MOD/MODU).
 * C and V unchanged.
 */
static void gen_update_flags_nz(TCGv_i32 result)
{
    TCGv_i32 f = tcg_temp_new_i32();

    /* Clear N and Z bits, keep C and V */
    tcg_gen_andi_i32(f, cpu_flags, FLAG_C | FLAG_V);

    /* N = result[31] */
    TCGv_i32 n = tcg_temp_new_i32();
    tcg_gen_andi_i32(n, result, 0x80000000u);
    tcg_gen_or_i32(f, f, n);

    /* Z */
    TCGv_i32 z = tcg_temp_new_i32();
    tcg_gen_setcondi_i32(TCG_COND_EQ, z, result, 0);
    tcg_gen_shli_i32(z, z, FLAG_Z_BIT);
    tcg_gen_or_i32(f, f, z);

    tcg_gen_mov_i32(cpu_flags, f);
}

/*
 * Full NZCV update for ADD: result = a + b
 */
static void gen_update_flags_add(TCGv_i32 a, TCGv_i32 b, TCGv_i32 result)
{
    TCGv_i32 f = tcg_temp_new_i32();
    tcg_gen_movi_i32(f, 0);

    /* N */
    TCGv_i32 n = tcg_temp_new_i32();
    tcg_gen_andi_i32(n, result, 0x80000000u);
    tcg_gen_or_i32(f, f, n);

    /* Z */
    TCGv_i32 z = tcg_temp_new_i32();
    tcg_gen_setcondi_i32(TCG_COND_EQ, z, result, 0);
    tcg_gen_shli_i32(z, z, FLAG_Z_BIT);
    tcg_gen_or_i32(f, f, z);

    /* C = unsigned carry: result < a */
    TCGv_i32 c = tcg_temp_new_i32();
    tcg_gen_setcond_i32(TCG_COND_LTU, c, result, a);
    tcg_gen_shli_i32(c, c, FLAG_C_BIT);
    tcg_gen_or_i32(f, f, c);

    /* V = signed overflow: ~(a^b) & (result^a) & 0x80000000 */
    TCGv_i32 v = tcg_temp_new_i32();
    TCGv_i32 t1 = tcg_temp_new_i32();
    TCGv_i32 t2 = tcg_temp_new_i32();
    tcg_gen_xor_i32(t1, a, b);
    tcg_gen_not_i32(t1, t1);
    tcg_gen_xor_i32(t2, result, a);
    tcg_gen_and_i32(v, t1, t2);
    tcg_gen_andi_i32(v, v, 0x80000000u);
    tcg_gen_shri_i32(v, v, FLAG_N_BIT - FLAG_V_BIT);
    tcg_gen_or_i32(f, f, v);

    tcg_gen_mov_i32(cpu_flags, f);
}

/*
 * Full NZCV update for SUB: result = a - b
 */
static void gen_update_flags_sub(TCGv_i32 a, TCGv_i32 b, TCGv_i32 result)
{
    TCGv_i32 f = tcg_temp_new_i32();
    tcg_gen_movi_i32(f, 0);

    /* N */
    TCGv_i32 n = tcg_temp_new_i32();
    tcg_gen_andi_i32(n, result, 0x80000000u);
    tcg_gen_or_i32(f, f, n);

    /* Z */
    TCGv_i32 z = tcg_temp_new_i32();
    tcg_gen_setcondi_i32(TCG_COND_EQ, z, result, 0);
    tcg_gen_shli_i32(z, z, FLAG_Z_BIT);
    tcg_gen_or_i32(f, f, z);

    /* C = borrow: b > a */
    TCGv_i32 c = tcg_temp_new_i32();
    tcg_gen_setcond_i32(TCG_COND_LTU, c, a, b);
    tcg_gen_shli_i32(c, c, FLAG_C_BIT);
    tcg_gen_or_i32(f, f, c);

    /* V = signed overflow: (a^b) & (result^a) & 0x80000000 */
    TCGv_i32 v = tcg_temp_new_i32();
    TCGv_i32 t1 = tcg_temp_new_i32();
    TCGv_i32 t2 = tcg_temp_new_i32();
    tcg_gen_xor_i32(t1, a, b);
    tcg_gen_xor_i32(t2, result, a);
    tcg_gen_and_i32(v, t1, t2);
    tcg_gen_andi_i32(v, v, 0x80000000u);
    tcg_gen_shri_i32(v, v, FLAG_N_BIT - FLAG_V_BIT);
    tcg_gen_or_i32(f, f, v);

    tcg_gen_mov_i32(cpu_flags, f);
}

/* ── Instruction translation functions ──────────────────────────── */

/* R-type ALU */
static bool trans_ADD(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 result = tcg_temp_new_i32();
    tcg_gen_add_i32(result, cpu_r[a->rs1], cpu_r[a->rs2]);
    gen_update_flags_add(cpu_r[a->rs1], cpu_r[a->rs2], result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_SUB(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 result = tcg_temp_new_i32();
    tcg_gen_sub_i32(result, cpu_r[a->rs1], cpu_r[a->rs2]);
    gen_update_flags_sub(cpu_r[a->rs1], cpu_r[a->rs2], result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_AND(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 result = tcg_temp_new_i32();
    tcg_gen_and_i32(result, cpu_r[a->rs1], cpu_r[a->rs2]);
    gen_update_flags_nz_clear_cv(result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_OR(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 result = tcg_temp_new_i32();
    tcg_gen_or_i32(result, cpu_r[a->rs1], cpu_r[a->rs2]);
    gen_update_flags_nz_clear_cv(result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_XOR(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 result = tcg_temp_new_i32();
    tcg_gen_xor_i32(result, cpu_r[a->rs1], cpu_r[a->rs2]);
    gen_update_flags_nz_clear_cv(result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_NOT(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 result = tcg_temp_new_i32();
    tcg_gen_not_i32(result, cpu_r[a->rs1]);
    gen_update_flags_nz_clear_cv(result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_LSL(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 result = tcg_temp_new_i32();
    tcg_gen_shli_i32(result, cpu_r[a->rs1], a->shift);
    gen_update_flags_nz_clear_cv(result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_LSR(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 result = tcg_temp_new_i32();
    tcg_gen_shri_i32(result, cpu_r[a->rs1], a->shift);
    gen_update_flags_nz_clear_cv(result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_ASR(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 result = tcg_temp_new_i32();
    tcg_gen_sari_i32(result, cpu_r[a->rs1], a->shift);
    gen_update_flags_nz_clear_cv(result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_MUL(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 result = tcg_temp_new_i32();
    tcg_gen_mul_i32(result, cpu_r[a->rs1], cpu_r[a->rs2]);
    gen_update_flags_nz(result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_DIV(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 result = tcg_temp_new_i32();
    gen_helper_cputwo_div(result, tcg_env, cpu_r[a->rs1], cpu_r[a->rs2]);
    gen_update_flags_nz(result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

/* Loads and stores */
static bool trans_LW(DisasContext *ctx, arg_i *a)
{
    TCGv_i32 addr = tcg_temp_new_i32();
    tcg_gen_addi_i32(addr, cpu_r[a->rs1], a->imm16);
    tcg_gen_qemu_ld_i32(cpu_r[a->rd], addr, ctx->tb_flags & STATUS_PRIV
                         ? MMU_IDX_SUPERVISOR : MMU_IDX_USER,
                         MO_LEUL);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_SW(DisasContext *ctx, arg_i *a)
{
    TCGv_i32 addr = tcg_temp_new_i32();
    tcg_gen_addi_i32(addr, cpu_r[a->rs1], a->imm16);
    tcg_gen_qemu_st_i32(cpu_r[a->rd], addr, ctx->tb_flags & STATUS_PRIV
                         ? MMU_IDX_SUPERVISOR : MMU_IDX_USER,
                         MO_LEUL);
    return true;
}

static bool trans_LH(DisasContext *ctx, arg_i *a)
{
    TCGv_i32 addr = tcg_temp_new_i32();
    int midx = (ctx->tb_flags & STATUS_PRIV) ? MMU_IDX_SUPERVISOR : MMU_IDX_USER;
    tcg_gen_addi_i32(addr, cpu_r[a->rs1], a->imm16);
    tcg_gen_qemu_ld_i32(cpu_r[a->rd], addr, midx, MO_LESW);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_LHU(DisasContext *ctx, arg_i *a)
{
    TCGv_i32 addr = tcg_temp_new_i32();
    int midx = (ctx->tb_flags & STATUS_PRIV) ? MMU_IDX_SUPERVISOR : MMU_IDX_USER;
    tcg_gen_addi_i32(addr, cpu_r[a->rs1], a->imm16);
    tcg_gen_qemu_ld_i32(cpu_r[a->rd], addr, midx, MO_LEUW);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_LB(DisasContext *ctx, arg_i *a)
{
    TCGv_i32 addr = tcg_temp_new_i32();
    int midx = (ctx->tb_flags & STATUS_PRIV) ? MMU_IDX_SUPERVISOR : MMU_IDX_USER;
    tcg_gen_addi_i32(addr, cpu_r[a->rs1], a->imm16);
    tcg_gen_qemu_ld_i32(cpu_r[a->rd], addr, midx, MO_SB);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_LBU(DisasContext *ctx, arg_i *a)
{
    TCGv_i32 addr = tcg_temp_new_i32();
    int midx = (ctx->tb_flags & STATUS_PRIV) ? MMU_IDX_SUPERVISOR : MMU_IDX_USER;
    tcg_gen_addi_i32(addr, cpu_r[a->rs1], a->imm16);
    tcg_gen_qemu_ld_i32(cpu_r[a->rd], addr, midx, MO_UB);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_SH(DisasContext *ctx, arg_i *a)
{
    TCGv_i32 addr = tcg_temp_new_i32();
    int midx = (ctx->tb_flags & STATUS_PRIV) ? MMU_IDX_SUPERVISOR : MMU_IDX_USER;
    tcg_gen_addi_i32(addr, cpu_r[a->rs1], a->imm16);
    tcg_gen_qemu_st_i32(cpu_r[a->rd], addr, midx, MO_LEUW);
    return true;
}

static bool trans_SB(DisasContext *ctx, arg_i *a)
{
    TCGv_i32 addr = tcg_temp_new_i32();
    int midx = (ctx->tb_flags & STATUS_PRIV) ? MMU_IDX_SUPERVISOR : MMU_IDX_USER;
    tcg_gen_addi_i32(addr, cpu_r[a->rs1], a->imm16);
    tcg_gen_qemu_st_i32(cpu_r[a->rd], addr, midx, MO_UB);
    return true;
}

/* Indexed memory operations (R-type encoding) */
static bool trans_LWX(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 addr = tcg_temp_new_i32();
    int midx = (ctx->tb_flags & STATUS_PRIV) ? MMU_IDX_SUPERVISOR : MMU_IDX_USER;
    tcg_gen_add_i32(addr, cpu_r[a->rs1], cpu_r[a->rs2]);
    tcg_gen_qemu_ld_i32(cpu_r[a->rd], addr, midx, MO_LEUL);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_LBX(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 addr = tcg_temp_new_i32();
    int midx = (ctx->tb_flags & STATUS_PRIV) ? MMU_IDX_SUPERVISOR : MMU_IDX_USER;
    tcg_gen_add_i32(addr, cpu_r[a->rs1], cpu_r[a->rs2]);
    tcg_gen_qemu_ld_i32(cpu_r[a->rd], addr, midx, MO_SB);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_LBUX(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 addr = tcg_temp_new_i32();
    int midx = (ctx->tb_flags & STATUS_PRIV) ? MMU_IDX_SUPERVISOR : MMU_IDX_USER;
    tcg_gen_add_i32(addr, cpu_r[a->rs1], cpu_r[a->rs2]);
    tcg_gen_qemu_ld_i32(cpu_r[a->rd], addr, midx, MO_UB);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_SWX(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 addr = tcg_temp_new_i32();
    int midx = (ctx->tb_flags & STATUS_PRIV) ? MMU_IDX_SUPERVISOR : MMU_IDX_USER;
    tcg_gen_add_i32(addr, cpu_r[a->rs1], cpu_r[a->rs2]);
    tcg_gen_qemu_st_i32(cpu_r[a->rd], addr, midx, MO_LEUL);
    return true;
}

static bool trans_SBX(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 addr = tcg_temp_new_i32();
    int midx = (ctx->tb_flags & STATUS_PRIV) ? MMU_IDX_SUPERVISOR : MMU_IDX_USER;
    tcg_gen_add_i32(addr, cpu_r[a->rs1], cpu_r[a->rs2]);
    tcg_gen_qemu_st_i32(cpu_r[a->rd], addr, midx, MO_UB);
    return true;
}

static bool trans_LHX(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 addr = tcg_temp_new_i32();
    int midx = (ctx->tb_flags & STATUS_PRIV) ? MMU_IDX_SUPERVISOR : MMU_IDX_USER;
    tcg_gen_add_i32(addr, cpu_r[a->rs1], cpu_r[a->rs2]);
    tcg_gen_qemu_ld_i32(cpu_r[a->rd], addr, midx, MO_LESW);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_LHUX(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 addr = tcg_temp_new_i32();
    int midx = (ctx->tb_flags & STATUS_PRIV) ? MMU_IDX_SUPERVISOR : MMU_IDX_USER;
    tcg_gen_add_i32(addr, cpu_r[a->rs1], cpu_r[a->rs2]);
    tcg_gen_qemu_ld_i32(cpu_r[a->rd], addr, midx, MO_LEUW);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_SHX(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 addr = tcg_temp_new_i32();
    int midx = (ctx->tb_flags & STATUS_PRIV) ? MMU_IDX_SUPERVISOR : MMU_IDX_USER;
    tcg_gen_add_i32(addr, cpu_r[a->rs1], cpu_r[a->rs2]);
    tcg_gen_qemu_st_i32(cpu_r[a->rd], addr, midx, MO_LEUW);
    return true;
}

/* Branch */
static bool trans_Bcc(DisasContext *ctx, arg_b *a)
{
    uint32_t target = ctx->pc + a->offset20;
    int cond = a->cond;

    if (cond >= 11) {
        /* Reserved condition codes — illegal instruction */
        gen_helper_cputwo_illegal(tcg_env);
        ctx->base.is_jmp = DISAS_NORETURN;
        return true;
    }

    if (cond == 6) {
        /* BA — unconditional */
        gen_goto_tb(ctx, 0, target);
        return true;
    }

    /* Conditional branch: test flags */
    TCGv_i32 flag_n = tcg_temp_new_i32();
    TCGv_i32 flag_z = tcg_temp_new_i32();
    TCGv_i32 flag_c = tcg_temp_new_i32();
    TCGv_i32 flag_v = tcg_temp_new_i32();
    TCGv_i32 tmp = tcg_temp_new_i32();
    TCGv_i32 taken = tcg_temp_new_i32();

    tcg_gen_shri_i32(flag_n, cpu_flags, FLAG_N_BIT);
    tcg_gen_andi_i32(flag_n, flag_n, 1);
    tcg_gen_shri_i32(flag_z, cpu_flags, FLAG_Z_BIT);
    tcg_gen_andi_i32(flag_z, flag_z, 1);
    tcg_gen_shri_i32(flag_c, cpu_flags, FLAG_C_BIT);
    tcg_gen_andi_i32(flag_c, flag_c, 1);
    tcg_gen_shri_i32(flag_v, cpu_flags, FLAG_V_BIT);
    tcg_gen_andi_i32(flag_v, flag_v, 1);

    switch (cond) {
    case 0: /* BEQ: Z=1 */
        tcg_gen_mov_i32(taken, flag_z);
        break;
    case 1: /* BNE: Z=0 */
        tcg_gen_xori_i32(taken, flag_z, 1);
        break;
    case 2: /* BLT: N!=V */
        tcg_gen_xor_i32(taken, flag_n, flag_v);
        break;
    case 3: /* BGE: N==V */
        tcg_gen_xor_i32(taken, flag_n, flag_v);
        tcg_gen_xori_i32(taken, taken, 1);
        break;
    case 4: /* BLTU: C=1 */
        tcg_gen_mov_i32(taken, flag_c);
        break;
    case 5: /* BGEU: C=0 */
        tcg_gen_xori_i32(taken, flag_c, 1);
        break;
    case 7: /* BGT: Z=0 && N==V */
        tcg_gen_xor_i32(tmp, flag_n, flag_v);
        tcg_gen_or_i32(tmp, tmp, flag_z);
        tcg_gen_xori_i32(taken, tmp, 1);
        break;
    case 8: /* BLE: Z=1 || N!=V */
        tcg_gen_xor_i32(tmp, flag_n, flag_v);
        tcg_gen_or_i32(taken, tmp, flag_z);
        break;
    case 9: /* BGTU: C=0 && Z=0 */
        tcg_gen_or_i32(tmp, flag_c, flag_z);
        tcg_gen_xori_i32(taken, tmp, 1);
        break;
    case 10: /* BLEU: C=1 || Z=1 */
        tcg_gen_or_i32(taken, flag_c, flag_z);
        break;
    default:
        g_assert_not_reached();
    }

    TCGLabel *l_not_taken = gen_new_label();
    tcg_gen_brcondi_i32(TCG_COND_EQ, taken, 0, l_not_taken);
    gen_goto_tb(ctx, 0, target);

    gen_set_label(l_not_taken);
    gen_goto_tb(ctx, 1, ctx->base.pc_next);

    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

/* Jump */
static bool trans_JMP(DisasContext *ctx, arg_j *a)
{
    uint32_t target = ctx->pc + a->offset20;
    /* rd = PC + 4 (already advanced) */
    tcg_gen_movi_i32(cpu_r[a->rd], ctx->base.pc_next);
    gen_goto_tb(ctx, 0, target);
    return true;
}

/* I-type ALU */
static bool trans_MOVI(DisasContext *ctx, arg_i *a)
{
    /* Zero-extended imm16 */
    tcg_gen_movi_i32(cpu_r[a->rd], (uint16_t)a->imm16);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_MOVHI(DisasContext *ctx, arg_i *a)
{
    /* rd = (rd & 0xFFFF) | (imm16 << 16) */
    TCGv_i32 lo = tcg_temp_new_i32();
    tcg_gen_andi_i32(lo, cpu_r[a->rd], 0xFFFF);
    tcg_gen_ori_i32(cpu_r[a->rd], lo, ((uint32_t)(uint16_t)a->imm16) << 16);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_LUI(DisasContext *ctx, arg_i *a)
{
    tcg_gen_movi_i32(cpu_r[a->rd], ((uint32_t)(uint16_t)a->imm16) << 16);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_ADDI(DisasContext *ctx, arg_i *a)
{
    TCGv_i32 imm = tcg_constant_i32(a->imm16);
    TCGv_i32 result = tcg_temp_new_i32();
    tcg_gen_add_i32(result, cpu_r[a->rs1], imm);
    gen_update_flags_add(cpu_r[a->rs1], imm, result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_SUBI(DisasContext *ctx, arg_i *a)
{
    TCGv_i32 imm = tcg_constant_i32(a->imm16);
    TCGv_i32 result = tcg_temp_new_i32();
    tcg_gen_sub_i32(result, cpu_r[a->rs1], imm);
    gen_update_flags_sub(cpu_r[a->rs1], imm, result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_ANDI(DisasContext *ctx, arg_i *a)
{
    TCGv_i32 result = tcg_temp_new_i32();
    tcg_gen_andi_i32(result, cpu_r[a->rs1], (uint16_t)a->imm16);
    gen_update_flags_nz_clear_cv(result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_ORI(DisasContext *ctx, arg_i *a)
{
    TCGv_i32 result = tcg_temp_new_i32();
    tcg_gen_ori_i32(result, cpu_r[a->rs1], (uint16_t)a->imm16);
    gen_update_flags_nz_clear_cv(result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_XORI(DisasContext *ctx, arg_i *a)
{
    TCGv_i32 result = tcg_temp_new_i32();
    tcg_gen_xori_i32(result, cpu_r[a->rs1], (uint16_t)a->imm16);
    gen_update_flags_nz_clear_cv(result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_LSLI(DisasContext *ctx, arg_i *a)
{
    TCGv_i32 result = tcg_temp_new_i32();
    tcg_gen_shli_i32(result, cpu_r[a->rs1], (uint16_t)a->imm16 & 0x1F);
    gen_update_flags_nz_clear_cv(result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_LSRI(DisasContext *ctx, arg_i *a)
{
    TCGv_i32 result = tcg_temp_new_i32();
    tcg_gen_shri_i32(result, cpu_r[a->rs1], (uint16_t)a->imm16 & 0x1F);
    gen_update_flags_nz_clear_cv(result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_ASRI(DisasContext *ctx, arg_i *a)
{
    TCGv_i32 result = tcg_temp_new_i32();
    tcg_gen_sari_i32(result, cpu_r[a->rs1], (uint16_t)a->imm16 & 0x1F);
    gen_update_flags_nz_clear_cv(result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

/* More R-type ALU */
static bool trans_MULH(DisasContext *ctx, arg_r *a)
{
    TCGv_i64 r64 = tcg_temp_new_i64();
    TCGv_i64 a64 = tcg_temp_new_i64();
    TCGv_i64 b64 = tcg_temp_new_i64();
    tcg_gen_ext_i32_i64(a64, cpu_r[a->rs1]);
    tcg_gen_ext_i32_i64(b64, cpu_r[a->rs2]);
    tcg_gen_mul_i64(r64, a64, b64);
    TCGv_i32 hi = tcg_temp_new_i32();
    tcg_gen_shri_i64(r64, r64, 32);
    tcg_gen_extrl_i64_i32(hi, r64);
    gen_update_flags_nz(hi);
    tcg_gen_mov_i32(cpu_r[a->rd], hi);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_MULHU(DisasContext *ctx, arg_r *a)
{
    TCGv_i64 r64 = tcg_temp_new_i64();
    TCGv_i64 a64 = tcg_temp_new_i64();
    TCGv_i64 b64 = tcg_temp_new_i64();
    tcg_gen_extu_i32_i64(a64, cpu_r[a->rs1]);
    tcg_gen_extu_i32_i64(b64, cpu_r[a->rs2]);
    tcg_gen_mul_i64(r64, a64, b64);
    TCGv_i32 hi = tcg_temp_new_i32();
    tcg_gen_shri_i64(r64, r64, 32);
    tcg_gen_extrl_i64_i32(hi, r64);
    gen_update_flags_nz(hi);
    tcg_gen_mov_i32(cpu_r[a->rd], hi);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_DIVU(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 result = tcg_temp_new_i32();
    gen_helper_cputwo_divu(result, tcg_env, cpu_r[a->rs1], cpu_r[a->rs2]);
    gen_update_flags_nz(result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_MOD(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 result = tcg_temp_new_i32();
    gen_helper_cputwo_mod(result, tcg_env, cpu_r[a->rs1], cpu_r[a->rs2]);
    gen_update_flags_nz(result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_MODU(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 result = tcg_temp_new_i32();
    gen_helper_cputwo_modu(result, tcg_env, cpu_r[a->rs1], cpu_r[a->rs2]);
    gen_update_flags_nz(result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_MOV(DisasContext *ctx, arg_r *a)
{
    /* MOV does NOT update flags */
    tcg_gen_mov_i32(cpu_r[a->rd], cpu_r[a->rs1]);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_CMP(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 result = tcg_temp_new_i32();
    tcg_gen_sub_i32(result, cpu_r[a->rs1], cpu_r[a->rs2]);
    gen_update_flags_sub(cpu_r[a->rs1], cpu_r[a->rs2], result);
    /* No rd write */
    return true;
}

static bool trans_CMPI(DisasContext *ctx, arg_i *a)
{
    TCGv_i32 imm = tcg_constant_i32((uint16_t)a->imm16);
    TCGv_i32 result = tcg_temp_new_i32();
    tcg_gen_sub_i32(result, cpu_r[a->rs1], imm);
    gen_update_flags_sub(cpu_r[a->rs1], imm, result);
    return true;
}

static bool trans_CALLR(DisasContext *ctx, arg_r *a)
{
    /* rd = PC + 4, PC = rs1 */
    TCGv_i32 target = tcg_temp_new_i32();
    tcg_gen_mov_i32(target, cpu_r[a->rs1]);
    tcg_gen_movi_i32(cpu_r[a->rd], ctx->base.pc_next);
    tcg_gen_mov_i32(cpu_pc, target);
    ctx->base.is_jmp = DISAS_JUMP;
    return true;
}

static bool trans_ADDC(DisasContext *ctx, arg_r *a)
{
    /* rd = rs1 + rs2 + C */
    TCGv_i32 cin = tcg_temp_new_i32();
    tcg_gen_shri_i32(cin, cpu_flags, FLAG_C_BIT);
    tcg_gen_andi_i32(cin, cin, 1);

    TCGv_i32 tmp = tcg_temp_new_i32();
    TCGv_i32 result = tcg_temp_new_i32();
    tcg_gen_add_i32(tmp, cpu_r[a->rs1], cpu_r[a->rs2]);
    tcg_gen_add_i32(result, tmp, cin);

    /* Flag update for ADDC is complex. Use the full add approach. */
    /* For simplicity, compute flags on the full 3-operand add */
    TCGv_i32 f = tcg_temp_new_i32();
    tcg_gen_movi_i32(f, 0);

    /* N */
    TCGv_i32 n = tcg_temp_new_i32();
    tcg_gen_andi_i32(n, result, 0x80000000u);
    tcg_gen_or_i32(f, f, n);

    /* Z */
    TCGv_i32 z = tcg_temp_new_i32();
    tcg_gen_setcondi_i32(TCG_COND_EQ, z, result, 0);
    tcg_gen_shli_i32(z, z, FLAG_Z_BIT);
    tcg_gen_or_i32(f, f, z);

    /* C: carry out of rs1+rs2+cin. Use 64-bit check via two 32-bit carries */
    TCGv_i32 c1 = tcg_temp_new_i32();
    TCGv_i32 c2 = tcg_temp_new_i32();
    TCGv_i32 c = tcg_temp_new_i32();
    tcg_gen_setcond_i32(TCG_COND_LTU, c1, tmp, cpu_r[a->rs1]);
    tcg_gen_setcond_i32(TCG_COND_LTU, c2, result, tmp);
    tcg_gen_or_i32(c, c1, c2);
    tcg_gen_shli_i32(c, c, FLAG_C_BIT);
    tcg_gen_or_i32(f, f, c);

    /* V: overflow of signed add */
    TCGv_i32 v = tcg_temp_new_i32();
    TCGv_i32 t1 = tcg_temp_new_i32();
    TCGv_i32 t2 = tcg_temp_new_i32();
    tcg_gen_xor_i32(t1, cpu_r[a->rs1], cpu_r[a->rs2]);
    tcg_gen_not_i32(t1, t1);
    tcg_gen_xor_i32(t2, result, cpu_r[a->rs1]);
    tcg_gen_and_i32(v, t1, t2);
    tcg_gen_andi_i32(v, v, 0x80000000u);
    tcg_gen_shri_i32(v, v, FLAG_N_BIT - FLAG_V_BIT);
    tcg_gen_or_i32(f, f, v);

    tcg_gen_mov_i32(cpu_flags, f);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_SUBC(DisasContext *ctx, arg_r *a)
{
    /* rd = rs1 - rs2 - C */
    TCGv_i32 cin = tcg_temp_new_i32();
    tcg_gen_shri_i32(cin, cpu_flags, FLAG_C_BIT);
    tcg_gen_andi_i32(cin, cin, 1);

    TCGv_i32 tmp = tcg_temp_new_i32();
    TCGv_i32 result = tcg_temp_new_i32();
    tcg_gen_sub_i32(tmp, cpu_r[a->rs1], cpu_r[a->rs2]);
    tcg_gen_sub_i32(result, tmp, cin);

    TCGv_i32 f = tcg_temp_new_i32();
    tcg_gen_movi_i32(f, 0);

    /* N */
    TCGv_i32 n = tcg_temp_new_i32();
    tcg_gen_andi_i32(n, result, 0x80000000u);
    tcg_gen_or_i32(f, f, n);

    /* Z */
    TCGv_i32 z = tcg_temp_new_i32();
    tcg_gen_setcondi_i32(TCG_COND_EQ, z, result, 0);
    tcg_gen_shli_i32(z, z, FLAG_Z_BIT);
    tcg_gen_or_i32(f, f, z);

    /* C: borrow */
    TCGv_i32 c1 = tcg_temp_new_i32();
    TCGv_i32 c2 = tcg_temp_new_i32();
    TCGv_i32 c = tcg_temp_new_i32();
    tcg_gen_setcond_i32(TCG_COND_LTU, c1, cpu_r[a->rs1], cpu_r[a->rs2]);
    tcg_gen_setcond_i32(TCG_COND_LTU, c2, tmp, cin);
    tcg_gen_or_i32(c, c1, c2);
    tcg_gen_shli_i32(c, c, FLAG_C_BIT);
    tcg_gen_or_i32(f, f, c);

    /* V */
    TCGv_i32 v = tcg_temp_new_i32();
    TCGv_i32 t1 = tcg_temp_new_i32();
    TCGv_i32 t2 = tcg_temp_new_i32();
    tcg_gen_xor_i32(t1, cpu_r[a->rs1], cpu_r[a->rs2]);
    tcg_gen_xor_i32(t2, result, cpu_r[a->rs1]);
    tcg_gen_and_i32(v, t1, t2);
    tcg_gen_andi_i32(v, v, 0x80000000u);
    tcg_gen_shri_i32(v, v, FLAG_N_BIT - FLAG_V_BIT);
    tcg_gen_or_i32(f, f, v);

    tcg_gen_mov_i32(cpu_flags, f);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

/* Register shifts */
static bool trans_LSLR(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 sh = tcg_temp_new_i32();
    TCGv_i32 result = tcg_temp_new_i32();
    tcg_gen_andi_i32(sh, cpu_r[a->rs2], 0x1F);
    tcg_gen_shl_i32(result, cpu_r[a->rs1], sh);
    gen_update_flags_nz_clear_cv(result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_LSRR(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 sh = tcg_temp_new_i32();
    TCGv_i32 result = tcg_temp_new_i32();
    tcg_gen_andi_i32(sh, cpu_r[a->rs2], 0x1F);
    tcg_gen_shr_i32(result, cpu_r[a->rs1], sh);
    gen_update_flags_nz_clear_cv(result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_ASRR(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 sh = tcg_temp_new_i32();
    TCGv_i32 result = tcg_temp_new_i32();
    tcg_gen_andi_i32(sh, cpu_r[a->rs2], 0x1F);
    tcg_gen_sar_i32(result, cpu_r[a->rs1], sh);
    gen_update_flags_nz_clear_cv(result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

/* Rotates */
static bool trans_ROLR(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 sh = tcg_temp_new_i32();
    TCGv_i32 result = tcg_temp_new_i32();
    tcg_gen_andi_i32(sh, cpu_r[a->rs2], 0x1F);
    tcg_gen_rotl_i32(result, cpu_r[a->rs1], sh);
    gen_update_flags_nz_clear_cv(result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_RORR(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 sh = tcg_temp_new_i32();
    TCGv_i32 result = tcg_temp_new_i32();
    tcg_gen_andi_i32(sh, cpu_r[a->rs2], 0x1F);
    tcg_gen_rotr_i32(result, cpu_r[a->rs1], sh);
    gen_update_flags_nz_clear_cv(result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    check_rd_pc(ctx, a->rd);
    return true;
}

static bool trans_ROLI(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 result = tcg_temp_new_i32();
    if (a->shift) {
        tcg_gen_rotli_i32(result, cpu_r[a->rs1], a->shift);
    } else {
        tcg_gen_mov_i32(result, cpu_r[a->rs1]);
    }
    gen_update_flags_nz_clear_cv(result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    return true;
}

static bool trans_RORI(DisasContext *ctx, arg_r *a)
{
    TCGv_i32 result = tcg_temp_new_i32();
    if (a->shift) {
        tcg_gen_rotri_i32(result, cpu_r[a->rs1], a->shift);
    } else {
        tcg_gen_mov_i32(result, cpu_r[a->rs1]);
    }
    gen_update_flags_nz_clear_cv(result);
    tcg_gen_mov_i32(cpu_r[a->rd], result);
    return true;
}

/* CAS — Atomic compare-and-swap (via helper) */
static bool trans_CAS(DisasContext *ctx, arg_r *a)
{
    gen_helper_cputwo_cas(tcg_env, cpu_r[a->rs1],
                           tcg_constant_i32(a->rd),
                           tcg_constant_i32(a->rs2));
    return true;
}

/* Privileged / System */
static bool trans_SYSCALL(DisasContext *ctx, arg_SYSCALL *a)
{
    /* Set r[15] = PC+4 so do_interrupt stores the correct return address
     * in EPC.  The translator advances pc_next but the TCG cpu_pc global
     * still holds the old insn_start value until we write it. */
    tcg_gen_movi_i32(cpu_pc, ctx->base.pc_next);
    gen_helper_cputwo_syscall(tcg_env);
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_SYSRET(DisasContext *ctx, arg_SYSRET *a)
{
    gen_helper_cputwo_sysret(tcg_env);
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_KRET(DisasContext *ctx, arg_KRET *a)
{
    gen_helper_cputwo_kret(tcg_env);
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_HALT(DisasContext *ctx, arg_HALT *a)
{
    gen_helper_cputwo_halt(tcg_env);
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_SFENCE(DisasContext *ctx, arg_SFENCE *a)
{
    gen_helper_cputwo_sfence(tcg_env);
    return true;
}

/* ── CPU dump state ───────────────────────────────────────────────── */

void cputwo_cpu_dump_state(CPUState *cs, FILE *f, int flags)
{
    CPUTwoState *env = cpu_env(cs);
    int i;

    qemu_fprintf(f, "pc=0x%08x flags=0x%08x status=0x%08x\n",
                 env->r[15], env->flags, env->status);
    for (i = 0; i < 16; i += 4) {
        qemu_fprintf(f, "r%d=0x%08x r%d=0x%08x r%d=0x%08x r%d=0x%08x\n",
                     i, env->r[i], i + 1, env->r[i + 1],
                     i + 2, env->r[i + 2], i + 3, env->r[i + 3]);
    }
    qemu_fprintf(f, "epc=0x%08x evec=0x%08x cause=0x%08x satp=0x%08x\n",
                 env->epc, env->evec, env->cause, env->satp);
}

/* ── Translator ops ───────────────────────────────────────────────── */

static void cputwo_tr_init_disas_context(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    ctx->env = cpu_env(cs);
    ctx->tb_flags = ctx->base.tb->flags;
}

static void cputwo_tr_tb_start(DisasContextBase *dcbase, CPUState *cs)
{
}

static void cputwo_tr_insn_start(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    tcg_gen_insn_start(ctx->base.pc_next);
}

static void cputwo_tr_translate_insn(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    uint32_t insn;

    ctx->pc = ctx->base.pc_next;
    insn = translator_ldl(ctx->env, &ctx->base, ctx->base.pc_next);
    ctx->base.pc_next += 4;

    if (!decode(ctx, insn)) {
        gen_helper_cputwo_illegal(tcg_env);
        ctx->base.is_jmp = DISAS_NORETURN;
    }
}

static void cputwo_tr_tb_stop(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);

    switch (ctx->base.is_jmp) {
    case DISAS_NEXT:
    case DISAS_TOO_MANY:
        gen_goto_tb(ctx, 0, dcbase->pc_next);
        break;
    case DISAS_JUMP:
        tcg_gen_lookup_and_goto_ptr();
        break;
    case DISAS_UPDATE:
        tcg_gen_movi_i32(cpu_pc, ctx->base.pc_next);
        /* fall through */
    case DISAS_EXIT:
        tcg_gen_exit_tb(NULL, 0);
        break;
    case DISAS_NORETURN:
        break;
    default:
        g_assert_not_reached();
    }
}

static const TranslatorOps cputwo_tr_ops = {
    .init_disas_context = cputwo_tr_init_disas_context,
    .tb_start           = cputwo_tr_tb_start,
    .insn_start         = cputwo_tr_insn_start,
    .translate_insn     = cputwo_tr_translate_insn,
    .tb_stop            = cputwo_tr_tb_stop,
};

void cputwo_translate_code(CPUState *cs, TranslationBlock *tb,
                            int *max_insns, vaddr pc, void *host_pc)
{
    DisasContext dc;
    translator_loop(cs, tb, max_insns, pc, host_pc, &cputwo_tr_ops, &dc.base);
}

/* ── TCG global register init ─────────────────────────────────────── */

#define ALLOC_REG(sym, name) \
    cpu_##sym = tcg_global_mem_new_i32(tcg_env, \
                                       offsetof(CPUTwoState, sym), name)

void cputwo_translate_init(void)
{
    static const char * const regnames[CPUTWO_NUM_REGS] = {
        "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7",
        "R8", "R9", "R10", "R11", "R12", "SP", "LR", "PC"
    };
    int i;

    for (i = 0; i < CPUTWO_NUM_REGS; i++) {
        cpu_r[i] = tcg_global_mem_new_i32(tcg_env,
                                           offsetof(CPUTwoState, r[i]),
                                           regnames[i]);
    }
    ALLOC_REG(flags, "FLAGS");
    ALLOC_REG(status, "STATUS");
    ALLOC_REG(epc, "EPC");
    ALLOC_REG(eflags, "EFLAGS");
    ALLOC_REG(evec, "EVEC");
    ALLOC_REG(cause, "CAUSE");
    ALLOC_REG(estatus, "ESTATUS");
    ALLOC_REG(satp, "SATP");
    ALLOC_REG(badaddr, "BADADDR");
}

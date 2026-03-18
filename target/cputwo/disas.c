/*
 *  CPUTwo Disassembler
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "disas/dis-asm.h"
#include "cpu.h"

static const char *reg_name(int r)
{
    static const char *names[] = {
        "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
        "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc"
    };
    return names[r & 15];
}

static const char *cond_names[] = {
    "beq", "bne", "blt", "bge", "bltu", "bgeu", "ba",
    "bgt", "ble", "bgtu", "bleu"
};

int print_insn_cputwo(bfd_vma addr, disassemble_info *info)
{
    bfd_byte buffer[4];
    int status;

    status = info->read_memory_func(addr, buffer, 4, info);
    if (status != 0) {
        info->memory_error_func(status, addr, info);
        return -1;
    }

    uint32_t instr = (uint32_t)buffer[0] | ((uint32_t)buffer[1] << 8) |
                     ((uint32_t)buffer[2] << 16) | ((uint32_t)buffer[3] << 24);

    uint8_t  op    = (instr >> 24) & 0xFF;
    uint8_t  rd    = (instr >> 20) & 0x0F;
    uint8_t  rs1   = (instr >> 16) & 0x0F;
    uint8_t  rs2   = (instr >> 12) & 0x0F;
    uint8_t  shift = (instr >>  7) & 0x1F;
    uint16_t imm16 = instr & 0xFFFF;
    int16_t  simm  = (int16_t)imm16;
    uint32_t off20 = instr & 0xFFFFF;
    int32_t  soff  = (off20 & 0x80000) ? (int32_t)(off20 | 0xFFF00000u)
                                        : (int32_t)off20;
    uint32_t tgt   = (uint32_t)(addr + soff);

    switch (op) {
    case 0x00: info->fprintf_func(info->stream, "add  %s, %s, %s", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x01: info->fprintf_func(info->stream, "sub  %s, %s, %s", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x02: info->fprintf_func(info->stream, "and  %s, %s, %s", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x03: info->fprintf_func(info->stream, "or   %s, %s, %s", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x04: info->fprintf_func(info->stream, "xor  %s, %s, %s", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x05: info->fprintf_func(info->stream, "not  %s, %s", reg_name(rd), reg_name(rs1)); break;
    case 0x06: info->fprintf_func(info->stream, "lsl  %s, %s, %u", reg_name(rd), reg_name(rs1), shift); break;
    case 0x07: info->fprintf_func(info->stream, "lsr  %s, %s, %u", reg_name(rd), reg_name(rs1), shift); break;
    case 0x08: info->fprintf_func(info->stream, "asr  %s, %s, %u", reg_name(rd), reg_name(rs1), shift); break;
    case 0x09: info->fprintf_func(info->stream, "mul  %s, %s, %s", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x0A: info->fprintf_func(info->stream, "div  %s, %s, %s", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x0B: info->fprintf_func(info->stream, "lw   %s, [%s%+d]", reg_name(rd), reg_name(rs1), simm); break;
    case 0x0C: info->fprintf_func(info->stream, "sw   %s, [%s%+d]", reg_name(rd), reg_name(rs1), simm); break;
    case 0x0D:
        if (rd < 11) {
            info->fprintf_func(info->stream, "%-4s 0x%08x", cond_names[rd], tgt);
        } else {
            info->fprintf_func(info->stream, "b??  (illegal cond %u)", rd);
        }
        break;
    case 0x0E: info->fprintf_func(info->stream, "jmp  %s, 0x%08x", reg_name(rd), tgt); break;
    case 0x0F: info->fprintf_func(info->stream, "movi %s, 0x%04x", reg_name(rd), imm16); break;
    case 0x10: info->fprintf_func(info->stream, "syscall"); break;
    case 0x11: info->fprintf_func(info->stream, "sysret"); break;
    case 0x12: info->fprintf_func(info->stream, "halt"); break;
    case 0x13: info->fprintf_func(info->stream, "movhi %s, 0x%04x", reg_name(rd), imm16); break;
    case 0x14: info->fprintf_func(info->stream, "addi %s, %s, %d", reg_name(rd), reg_name(rs1), simm); break;
    case 0x15: info->fprintf_func(info->stream, "subi %s, %s, %d", reg_name(rd), reg_name(rs1), simm); break;
    case 0x16: info->fprintf_func(info->stream, "andi %s, %s, 0x%04x", reg_name(rd), reg_name(rs1), imm16); break;
    case 0x17: info->fprintf_func(info->stream, "ori  %s, %s, 0x%04x", reg_name(rd), reg_name(rs1), imm16); break;
    case 0x18: info->fprintf_func(info->stream, "xori %s, %s, 0x%04x", reg_name(rd), reg_name(rs1), imm16); break;
    case 0x19: info->fprintf_func(info->stream, "lsli %s, %s, %u", reg_name(rd), reg_name(rs1), imm16 & 0x1F); break;
    case 0x1A: info->fprintf_func(info->stream, "lsri %s, %s, %u", reg_name(rd), reg_name(rs1), imm16 & 0x1F); break;
    case 0x1B: info->fprintf_func(info->stream, "asri %s, %s, %u", reg_name(rd), reg_name(rs1), imm16 & 0x1F); break;
    case 0x1C: info->fprintf_func(info->stream, "lh   %s, [%s%+d]", reg_name(rd), reg_name(rs1), simm); break;
    case 0x1D: info->fprintf_func(info->stream, "lhu  %s, [%s%+d]", reg_name(rd), reg_name(rs1), simm); break;
    case 0x1E: info->fprintf_func(info->stream, "lb   %s, [%s%+d]", reg_name(rd), reg_name(rs1), simm); break;
    case 0x1F: info->fprintf_func(info->stream, "lbu  %s, [%s%+d]", reg_name(rd), reg_name(rs1), simm); break;
    case 0x20: info->fprintf_func(info->stream, "sh   %s, [%s%+d]", reg_name(rd), reg_name(rs1), simm); break;
    case 0x21: info->fprintf_func(info->stream, "sb   %s, [%s%+d]", reg_name(rd), reg_name(rs1), simm); break;
    case 0x22: info->fprintf_func(info->stream, "mulh %s, %s, %s", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x23: info->fprintf_func(info->stream, "mulhu %s, %s, %s", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x24: info->fprintf_func(info->stream, "divu %s, %s, %s", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x25: info->fprintf_func(info->stream, "mod  %s, %s, %s", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x26: info->fprintf_func(info->stream, "modu %s, %s, %s", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x27: info->fprintf_func(info->stream, "mov  %s, %s", reg_name(rd), reg_name(rs1)); break;
    case 0x28: info->fprintf_func(info->stream, "cmp  %s, %s", reg_name(rs1), reg_name(rs2)); break;
    case 0x29: info->fprintf_func(info->stream, "cmpi %s, 0x%04x", reg_name(rs1), imm16); break;
    case 0x2A: info->fprintf_func(info->stream, "callr %s, %s", reg_name(rd), reg_name(rs1)); break;
    case 0x2B: info->fprintf_func(info->stream, "addc %s, %s, %s", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x2C: info->fprintf_func(info->stream, "subc %s, %s, %s", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x2D: info->fprintf_func(info->stream, "lslr %s, %s, %s", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x2E: info->fprintf_func(info->stream, "lsrr %s, %s, %s", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x2F: info->fprintf_func(info->stream, "asrr %s, %s, %s", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x30: info->fprintf_func(info->stream, "lwx  %s, [%s+%s]", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x31: info->fprintf_func(info->stream, "lbx  %s, [%s+%s]", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x32: info->fprintf_func(info->stream, "lbux %s, [%s+%s]", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x33: info->fprintf_func(info->stream, "swx  %s, [%s+%s]", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x34: info->fprintf_func(info->stream, "sbx  %s, [%s+%s]", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x35: info->fprintf_func(info->stream, "lhx  %s, [%s+%s]", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x36: info->fprintf_func(info->stream, "lhux %s, [%s+%s]", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x37: info->fprintf_func(info->stream, "shx  %s, [%s+%s]", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x38: info->fprintf_func(info->stream, "lui  %s, 0x%04x", reg_name(rd), imm16); break;
    case 0x39: info->fprintf_func(info->stream, "rolr %s, %s, %s", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x3A: info->fprintf_func(info->stream, "rorr %s, %s, %s", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x3B: info->fprintf_func(info->stream, "roli %s, %s, %u", reg_name(rd), reg_name(rs1), shift); break;
    case 0x3C: info->fprintf_func(info->stream, "rori %s, %s, %u", reg_name(rd), reg_name(rs1), shift); break;
    case 0x3D: info->fprintf_func(info->stream, "cas  %s, [%s], %s", reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
    case 0x3E: info->fprintf_func(info->stream, "sfence"); break;
    case 0x3F: info->fprintf_func(info->stream, "kret"); break;
    default:   info->fprintf_func(info->stream, ".word 0x%08x", instr); break;
    }

    return 4; /* all instructions are 4 bytes */
}

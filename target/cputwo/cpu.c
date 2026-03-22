/*
 *  QEMU CPUTwo CPU
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/qemu-print.h"
#include "qapi/error.h"
#include "cpu.h"
#include "migration/vmstate.h"
#include "exec/cputlb.h"
#include "exec/page-protection.h"
#include "exec/translation-block.h"
#include "exec/target_page.h"
#include "hw/loader.h"
#include "system/memory.h"
#include "tcg/debug-assert.h"
#include "accel/tcg/cpu-ops.h"

static void cputwo_cpu_set_pc(CPUState *cs, vaddr value)
{
    CPUTwoCPU *cpu = CPUTWO_CPU(cs);
    cpu->env.r[15] = value;
}

static vaddr cputwo_cpu_get_pc(CPUState *cs)
{
    CPUTwoCPU *cpu = CPUTWO_CPU(cs);
    return cpu->env.r[15];
}

static TCGTBCPUState cputwo_get_tb_cpu_state(CPUState *cs)
{
    CPUTwoState *env = cpu_env(cs);
    uint32_t flags = env->status & (STATUS_PRIV | STATUS_IE);
    return (TCGTBCPUState){ .pc = env->r[15], .flags = flags };
}

static void cputwo_cpu_synchronize_from_tb(CPUState *cs,
                                            const TranslationBlock *tb)
{
    CPUTwoCPU *cpu = CPUTWO_CPU(cs);
    tcg_debug_assert(!tcg_cflags_has(cs, CF_PCREL));
    cpu->env.r[15] = tb->pc;
}

static void cputwo_restore_state_to_opc(CPUState *cs,
                                         const TranslationBlock *tb,
                                         const uint64_t *data)
{
    CPUTwoCPU *cpu = CPUTWO_CPU(cs);
    cpu->env.r[15] = data[0];
}

static bool cputwo_cpu_has_work(CPUState *cs)
{
    return cpu_test_interrupt(cs, CPU_INTERRUPT_HARD);
}

static int cputwo_cpu_mmu_index(CPUState *cs, bool ifunc)
{
    CPUTwoState *env = cpu_env(cs);
    return (env->status & STATUS_PRIV) ? MMU_IDX_SUPERVISOR : MMU_IDX_USER;
}

static void cputwo_cpu_reset_hold(Object *obj, ResetType type)
{
    CPUState *cs = CPU(obj);
    CPUTwoCPUClass *mcc = CPUTWO_CPU_GET_CLASS(obj);
    CPUTwoState *env = cpu_env(cs);

    if (mcc->parent_phases.hold) {
        mcc->parent_phases.hold(obj, type);
    }

    memset(env, 0, offsetof(CPUTwoState, end_reset_fields));

    /* Reset state per architecture spec */
    env->r[15] = 0x00000000;  /* PC = 0 */
    env->status = STATUS_PRIV; /* supervisor mode, IE=0 */
    env->estatus = STATUS_IE;  /* user mode + IE=1 for first SYSRET */
    /* satp = 0: MMU disabled */
    /* All other registers and flags = 0 */
}

static ObjectClass *cputwo_cpu_class_by_name(const char *cpu_model)
{
    ObjectClass *oc;

    oc = object_class_by_name(cpu_model);
    if (oc != NULL && object_class_dynamic_cast(oc, TYPE_CPUTWO_CPU) != NULL) {
        return oc;
    }
    return NULL;
}

static void cputwo_cpu_realize(DeviceState *dev, Error **errp)
{
    CPUState *cs = CPU(dev);
    CPUTwoCPUClass *mcc = CPUTWO_CPU_GET_CLASS(dev);
    Error *local_err = NULL;

    cpu_exec_realizefn(cs, &local_err);
    if (local_err != NULL) {
        error_propagate(errp, local_err);
        return;
    }

    qemu_init_vcpu(cs);
    cpu_reset(cs);

    mcc->parent_realize(dev, errp);
}

static void cputwo_cpu_set_irq(void *opaque, int no, int request)
{
    CPUTwoCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    if (request) {
        cpu_interrupt(cs, CPU_INTERRUPT_HARD);
    } else {
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
    }
}

static void cputwo_cpu_disas_set_info(CPUState *cpu, disassemble_info *info)
{
    info->endian = BFD_ENDIAN_LITTLE;
    info->print_insn = print_insn_cputwo;
}

static inline bool check_access(MMUAccessType access_type, int prot)
{
    switch (access_type) {
    case MMU_INST_FETCH:
        return (prot & PAGE_EXEC) != 0;
    case MMU_DATA_LOAD:
        return (prot & PAGE_READ) != 0;
    case MMU_DATA_STORE:
        return (prot & PAGE_WRITE) != 0;
    default:
        return false;
    }
}

/*
 * MMU / TLB fill
 *
 * Performs the Sv32-compatible page table walk for CPUTwo.
 * When SATP.EN=0, all accesses are identity-mapped.
 * When SATP.EN=1, two-level page tables are walked.
 * MMIO region (>= 0x03F00000) always bypasses translation.
 */
static bool cputwo_cpu_tlb_fill(CPUState *cs, vaddr addr, int size,
                                 MMUAccessType access_type, int mmu_idx,
                                 bool probe, uintptr_t retaddr)
{
    CPUTwoState *env = cpu_env(cs);
    uint32_t va = (uint32_t)addr;
    uint32_t pa;
    int prot;

    /* If MMU disabled, identity mapping */
    if (!(env->satp & 0x80000000u)) {
        pa = va & TARGET_PAGE_MASK;
        prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        tlb_set_page(cs, va & TARGET_PAGE_MASK, pa, prot, mmu_idx,
                     TARGET_PAGE_SIZE);
        return true;
    }

    /* MMIO region always bypasses MMU (only for VAs within physical range) */
    if (va >= CPUTWO_MMIO_BASE && va < CPUTWO_MEM_SIZE) {
        pa = va & TARGET_PAGE_MASK;
        prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        tlb_set_page(cs, va & TARGET_PAGE_MASK, pa, prot, mmu_idx,
                     TARGET_PAGE_SIZE);
        return true;
    }

    /* Two-level Sv32 page table walk */
    uint32_t l1_base = (env->satp & 0x000FFFFFu) << 12;
    uint32_t vpn1 = (va >> 22) & 0x3FFu;
    uint32_t vpn0 = (va >> 12) & 0x3FFu;

    /* Read L1 PTE from physical memory */
    uint32_t l1_pte_addr = l1_base + vpn1 * 4;
    if (l1_pte_addr + 3 >= CPUTWO_MEM_SIZE || l1_pte_addr >= CPUTWO_MMIO_BASE) {
        goto fault;
    }
    uint32_t l1_pte = ldl_le_phys(cs->as, l1_pte_addr);

    /* V bit check */
    if (!(l1_pte & 1u)) {
        goto fault;
    }

    /* Superpage: if R|W|X set at L1 level */
    if (l1_pte & (7u << 5)) {
        uint32_t ppn = (l1_pte >> 12) & 0xFFFFFu;
        pa = ((ppn & 0xFFC00u) << 12) | (va & 0x3FFFFFu);

        prot = 0;
        if (l1_pte & (1u << 5)) prot |= PAGE_READ;
        if (l1_pte & (1u << 6)) prot |= PAGE_WRITE;
        if (l1_pte & (1u << 7)) prot |= PAGE_EXEC;

        /* User-mode check */
        if (mmu_idx == MMU_IDX_USER && !(l1_pte & (1u << 8))) {
            goto fault;
        }

        /* Check access type */
        if (!check_access(access_type, prot)) {
            goto fault;
        }

        /* Set D bit on stores */
        if (access_type == MMU_DATA_STORE && !(l1_pte & (1u << 1))) {
            l1_pte |= (1u << 1);
            stl_le_phys(cs->as, l1_pte_addr, l1_pte);
        }

        /* For superpages, map a single 4KB page at a time (QEMU standard) */
        pa = (pa & TARGET_PAGE_MASK);
        tlb_set_page(cs, va & TARGET_PAGE_MASK, pa, prot, mmu_idx,
                     TARGET_PAGE_SIZE);
        return true;
    }

    /* Non-leaf L1 PTE: walk to L2 */
    uint32_t l2_base = ((l1_pte >> 12) & 0xFFFFFu) << 12;
    uint32_t l2_pte_addr = l2_base + vpn0 * 4;
    if (l2_pte_addr + 3 >= CPUTWO_MEM_SIZE || l2_pte_addr >= CPUTWO_MMIO_BASE) {
        goto fault;
    }
    uint32_t l2_pte = ldl_le_phys(cs->as, l2_pte_addr);

    if (!(l2_pte & 1u)) {
        goto fault;
    }

    prot = 0;
    if (l2_pte & (1u << 5)) prot |= PAGE_READ;
    if (l2_pte & (1u << 6)) prot |= PAGE_WRITE;
    if (l2_pte & (1u << 7)) prot |= PAGE_EXEC;

    /* User-mode check */
    if (mmu_idx == MMU_IDX_USER && !(l2_pte & (1u << 8))) {
        goto fault;
    }

    if (!check_access(access_type, prot)) {
        goto fault;
    }

    uint32_t ppn2 = (l2_pte >> 12) & 0xFFFFFu;
    pa = (ppn2 << 12) | (va & 0xFFFu);

    /* Set D bit on stores */
    if (access_type == MMU_DATA_STORE && !(l2_pte & (1u << 1))) {
        l2_pte |= (1u << 1);
        stl_le_phys(cs->as, l2_pte_addr, l2_pte);
    }

    tlb_set_page(cs, va & TARGET_PAGE_MASK, pa & TARGET_PAGE_MASK, prot,
                 mmu_idx, TARGET_PAGE_SIZE);
    return true;

fault:
    if (probe) {
        return false;
    }
    env->badaddr = va;
    /* Determine exception type */
    int excp;
    switch (access_type) {
    case MMU_INST_FETCH:
        excp = EXCP_IFAULT;
        break;
    case MMU_DATA_LOAD:
        excp = EXCP_LFAULT;
        break;
    case MMU_DATA_STORE:
        excp = EXCP_SFAULT;
        break;
    default:
        excp = EXCP_BUS;
        break;
    }
    cs->exception_index = excp;
    cpu_loop_exit_restore(cs, retaddr);
}

static void cputwo_cpu_init(Object *obj)
{
    CPUTwoCPU *cpu = CPUTWO_CPU(obj);
    qdev_init_gpio_in(DEVICE(cpu), cputwo_cpu_set_irq, 1);
}

#include "hw/core/sysemu-cpu-ops.h"

static const struct SysemuCPUOps cputwo_sysemu_ops = {
    .has_work = cputwo_cpu_has_work,
    .get_phys_page_debug = cputwo_cpu_get_phys_page_debug,
};

static const TCGCPUOps cputwo_tcg_ops = {
    .guest_default_memory_order = TCG_MO_ALL,
    .mttcg_supported = false,

    .initialize = cputwo_translate_init,
    .translate_code = cputwo_translate_code,
    .get_tb_cpu_state = cputwo_get_tb_cpu_state,
    .synchronize_from_tb = cputwo_cpu_synchronize_from_tb,
    .restore_state_to_opc = cputwo_restore_state_to_opc,
    .mmu_index = cputwo_cpu_mmu_index,
    .tlb_fill = cputwo_cpu_tlb_fill,
    .pointer_wrap = cpu_pointer_wrap_uint32,

    .cpu_exec_interrupt = cputwo_cpu_exec_interrupt,
    .cpu_exec_halt = cputwo_cpu_has_work,
    .cpu_exec_reset = cpu_reset,
    .do_interrupt = cputwo_cpu_do_interrupt,
};

static void cputwo_cpu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    CPUClass *cc = CPU_CLASS(klass);
    CPUTwoCPUClass *mcc = CPUTWO_CPU_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    device_class_set_parent_realize(dc, cputwo_cpu_realize,
                                    &mcc->parent_realize);
    resettable_class_set_parent_phases(rc, NULL, cputwo_cpu_reset_hold, NULL,
                                       &mcc->parent_phases);

    cc->class_by_name = cputwo_cpu_class_by_name;
    cc->dump_state = cputwo_cpu_dump_state;
    cc->set_pc = cputwo_cpu_set_pc;
    cc->get_pc = cputwo_cpu_get_pc;

    cc->sysemu_ops = &cputwo_sysemu_ops;
    cc->gdb_read_register = cputwo_cpu_gdb_read_register;
    cc->gdb_write_register = cputwo_cpu_gdb_write_register;
    cc->disas_set_info = cputwo_cpu_disas_set_info;

    cc->gdb_core_xml_file = "cputwo-cpu.xml";
    cc->tcg_ops = &cputwo_tcg_ops;
}

static const TypeInfo cputwo_cpu_info = {
    .name = TYPE_CPUTWO_CPU,
    .parent = TYPE_CPU,
    .instance_size = sizeof(CPUTwoCPU),
    .instance_align = __alignof(CPUTwoCPU),
    .instance_init = cputwo_cpu_init,
    .class_size = sizeof(CPUTwoCPUClass),
    .class_init = cputwo_cpu_class_init,
};

static void cputwo_cpu_register_types(void)
{
    type_register_static(&cputwo_cpu_info);
}

type_init(cputwo_cpu_register_types)

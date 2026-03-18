/*
 *  CPUTwo board/machine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/loader.h"
#include "hw/boards.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/misc/unimp.h"
#include "hw/cputwo/cputwo_uart.h"
#include "hw/cputwo/cputwo_ic.h"
#include "hw/cputwo/cputwo_timer.h"
#include "hw/cputwo/cputwo_svreg.h"
#include "hw/cputwo/cputwo_blk.h"
#include "system/system.h"
#include "system/block-backend.h"
#include "elf.h"
#include "target/cputwo/cpu.h"

#define CPUTWO_RAM_SIZE     (63 * MiB)  /* 0x00000000 - 0x03EFFFFF */
#define CPUTWO_UART_BASE    0x03F00000u
#define CPUTWO_TIMER_BASE   0x03F01000u
#define CPUTWO_IC_BASE      0x03F02000u
#define CPUTWO_BLK_BASE     0x03F03000u
#define CPUTWO_SV_BASE      0x03FFF000u

typedef struct CPUTwoBoardState {
    MachineState parent_obj;
    CPUTwoCPU *cpu;
} CPUTwoBoardState;

#define TYPE_CPUTWO_BOARD MACHINE_TYPE_NAME("cputwo-board")
DECLARE_INSTANCE_CHECKER(CPUTwoBoardState, CPUTWO_BOARD, TYPE_CPUTWO_BOARD)

static void cputwo_board_init(MachineState *machine)
{
    CPUTwoBoardState *s = CPUTWO_BOARD(machine);
    MemoryRegion *sysmem = get_system_memory();
    const char *kernel_filename = machine->kernel_filename;
    DeviceState *ic_dev, *uart_dev, *timer_dev, *svreg_dev, *blk_dev;

    /* Create CPU */
    s->cpu = CPUTWO_CPU(cpu_create(TYPE_CPUTWO_CPU));

    /* Create RAM (63 MB, below MMIO region) */
    memory_region_add_subregion(sysmem, 0, machine->ram);

    /*
     * Interrupt Controller
     *   4 inputs: timer(0), uart_rx(1), uart_tx(2), blk(3)
     *   1 output: CPU IRQ line
     */
    ic_dev = qdev_new(TYPE_CPUTWO_IC);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ic_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(ic_dev), 0, CPUTWO_IC_BASE);
    /* Connect IC output to CPU IRQ input (gpio-in line 0) */
    sysbus_connect_irq(SYS_BUS_DEVICE(ic_dev), 0,
                       qdev_get_gpio_in(DEVICE(s->cpu), CPUTWO_CPU_IRQ));

    /*
     * UART
     *   2 IRQ outputs: RX(0), TX(1) -> IC inputs 1, 2
     */
    uart_dev = qdev_new(TYPE_CPUTWO_UART);
    qdev_prop_set_chr(uart_dev, "chardev", serial_hd(0));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(uart_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(uart_dev), 0, CPUTWO_UART_BASE);
    /* UART RX IRQ -> IC input 1 */
    sysbus_connect_irq(SYS_BUS_DEVICE(uart_dev), 0,
                       qdev_get_gpio_in(ic_dev, 1));
    /* UART TX IRQ -> IC input 2 */
    sysbus_connect_irq(SYS_BUS_DEVICE(uart_dev), 1,
                       qdev_get_gpio_in(ic_dev, 2));

    /*
     * Timer
     *   1 IRQ output -> IC input 0
     */
    timer_dev = qdev_new(TYPE_CPUTWO_TIMER);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(timer_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(timer_dev), 0, CPUTWO_TIMER_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(timer_dev), 0,
                       qdev_get_gpio_in(ic_dev, 0));

    /* Supervisor registers (memory-mapped CPU control) */
    svreg_dev = qdev_new(TYPE_CPUTWO_SVREG);
    CPUTWO_SVREG(svreg_dev)->cpu_env = &s->cpu->env;
    sysbus_realize_and_unref(SYS_BUS_DEVICE(svreg_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(svreg_dev), 0, CPUTWO_SV_BASE);

    /*
     * Block device
     *   1 IRQ output -> IC input 3
     */
    blk_dev = qdev_new(TYPE_CPUTWO_BLK);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(blk_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(blk_dev), 0, CPUTWO_BLK_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(blk_dev), 0,
                       qdev_get_gpio_in(ic_dev, 3));

    /* Load kernel */
    if (kernel_filename) {
        uint64_t entry;
        int kernel_size;

        kernel_size = load_elf(kernel_filename, NULL, NULL, NULL,
                               &entry, NULL, NULL, NULL,
                               0, EM_NONE, 0, 0);
        if (kernel_size > 0) {
            s->cpu->env.r[15] = (uint32_t)entry;
        } else {
            kernel_size = load_image_targphys(kernel_filename, 0,
                                              CPUTWO_RAM_SIZE, NULL);
            if (kernel_size < 0) {
                error_report("Could not load kernel '%s'", kernel_filename);
                exit(1);
            }
            s->cpu->env.r[15] = 0;
        }
    }
}

static void cputwo_board_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "CPUTwo Board";
    mc->init = cputwo_board_init;
    mc->default_cpu_type = TYPE_CPUTWO_CPU;
    mc->default_ram_size = CPUTWO_RAM_SIZE;
    mc->default_ram_id = "cputwo.ram";
}

static const TypeInfo cputwo_board_types[] = {
    {
        .name           = TYPE_CPUTWO_BOARD,
        .parent         = TYPE_MACHINE,
        .instance_size  = sizeof(CPUTwoBoardState),
        .class_init     = cputwo_board_class_init,
    }
};

DEFINE_TYPES(cputwo_board_types)

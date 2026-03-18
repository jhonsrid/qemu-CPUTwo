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
#include "system/system.h"
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
    DeviceState *uart_dev;

    /* Create CPU */
    s->cpu = CPUTWO_CPU(cpu_create(TYPE_CPUTWO_CPU));

    /* Create RAM (63 MB, below MMIO region) */
    memory_region_add_subregion(sysmem, 0, machine->ram);

    /* UART */
    uart_dev = qdev_new(TYPE_CPUTWO_UART);
    qdev_prop_set_chr(uart_dev, "chardev", serial_hd(0));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(uart_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(uart_dev), 0, CPUTWO_UART_BASE);

    /* Other MMIO devices — stubs for now */
    create_unimplemented_device("cputwo-timer", CPUTWO_TIMER_BASE, 0x1000);
    create_unimplemented_device("cputwo-ic",    CPUTWO_IC_BASE,    0x1000);
    create_unimplemented_device("cputwo-blk",   CPUTWO_BLK_BASE,   0x1000);
    create_unimplemented_device("cputwo-sv",    CPUTWO_SV_BASE,    0x1000);

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
            /* Try loading as raw binary at address 0 */
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

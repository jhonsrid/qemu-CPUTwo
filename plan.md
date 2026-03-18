# Plan: Adding CPUTwo Architecture to QEMU 10.2

## Architecture Summary

CPUTwo is a 32-bit RISC architecture with:
- 16 registers (r0–r12 GPR, r13/sp, r14/lr, r15/pc)
- Fixed 32-bit instruction encoding (5 formats: R, I, B, M, J)
- 64 opcodes (0x00–0x3F)
- N/Z/C/V flags register
- Two privilege modes (User / Supervisor)
- Memory-mapped supervisor registers at 0x03FFF000
- Sv32-compatible two-level MMU with 4 KB pages and 4 MB superpages
- 64 MB address space (0x00000000–0x03FFFFFF)
- Memory-mapped I/O: UART, Timer, Interrupt Controller, Block Device
- Little-endian, byte-addressable

---

## Phase 1: Build System & Configuration

### 1.1 Create `configs/targets/cputwo-softmmu.mak`

```makefile
TARGET_ARCH=cputwo
TARGET_XML_FILES= gdb-xml/cputwo-cpu.xml
TARGET_LONG_BITS=32
```

### 1.2 Create `configs/targets/cputwo-softmmu-Kconfig`

Define the board/machine config symbols:

```
config CPUTWO_BOARD
    bool
    default y
    depends on CPUTWO
    select UNIMP
```

### 1.3 Register in `target/meson.build`

Add `subdir('cputwo')` to the target list.

### 1.4 Register in `hw/meson.build`

Add `subdir('cputwo')` to the hw list.

### 1.5 Default configs

Create `configs/devices/cputwo-softmmu/default.mak`:
```makefile
CONFIG_CPUTWO_BOARD=y
```

---

## Phase 2: CPU Target (`target/cputwo/`)

This is the core of the port — defining the CPU model, instruction translation, and exception handling. Use `target/rx/` and `target/avr/` as reference implementations since they are simple 32-bit architectures of similar complexity.

### 2.1 `target/cputwo/cpu-param.h` — Target Parameters

```c
#define TARGET_LONG_BITS         32
#define TARGET_PAGE_BITS         12        /* 4 KB pages */
#define TARGET_PHYS_ADDR_SPACE_BITS 26    /* 64 MB physical */
#define TARGET_VIRT_ADDR_SPACE_BITS 32    /* full 32-bit VA */
#define TARGET_INSN_START_EXTRA_WORDS 0
```

### 2.2 `target/cputwo/cpu-qom.h` — QOM Type Declarations

Define the QOM (QEMU Object Model) type strings and macros:
- `TYPE_CPUTWO_CPU` = `"cputwo-cpu"`
- `OBJECT_DECLARE_CPU_TYPE(CPUTwoCPU, CPUTwoCPUClass, CPUTWO_CPU)`

### 2.3 `target/cputwo/cpu.h` — CPU State & Constants

The main header. Define:

**`CPUArchState` (aka `CPUTwoState`):**
```c
typedef struct CPUArchState {
    uint32_t r[16];        /* r0–r12 GPR, r13=sp, r14=lr, r15=pc */
    uint32_t flags;        /* N(31) Z(30) C(29) V(28) */

    /* Supervisor registers (memory-mapped at 0x03FFF000 in guest) */
    uint32_t epc;
    uint32_t eflags;
    uint32_t evec;
    uint32_t cause;
    uint32_t status;       /* bit0=priv(1=sv), bit1=IE */
    uint32_t estatus;
    uint32_t satp;         /* bit31=EN, bits[19:0]=PPN */
    uint32_t badaddr;

    /* Internal state */
    uint32_t halted;
    uint32_t faulting_pc;  /* PC of currently executing insn */
} CPUTwoState;
```

**`ArchCPU` (aka `CPUTwoCPU`):**
```c
struct ArchCPU {
    CPUState parent_obj;
    CPUTwoState env;
};
```

**Constants:** Exception cause codes (EXCP_ILLEGAL=0x00, EXCP_MISALIGNED=0x01, EXCP_BUS=0x02, EXCP_SYSCALL=0x03, EXCP_DIVZERO=0x04, EXCP_HALT=0x05, EXCP_IRQ=0x06, EXCP_IFAULT=0x07, EXCP_LFAULT=0x08, EXCP_SFAULT=0x09).

**MMU index constants:** `MMU_IDX_USER=0`, `MMU_IDX_SUPERVISOR=1`.

### 2.4 `target/cputwo/cpu.c` — CPU Implementation

Implement the QEMU CPU lifecycle functions:

- **`cputwo_cpu_set_pc()`** / **`cputwo_cpu_get_pc()`** — read/write `env.r[15]`
- **`cputwo_cpu_has_work()`** — return true if pending interrupts are unmasked and `STATUS.IE=1`
- **`cputwo_cpu_mmu_index()`** — return `MMU_IDX_USER` or `MMU_IDX_SUPERVISOR` based on `status & 1`
- **`cputwo_get_tb_cpu_state()`** — pack PC + flags + status into translation block state
- **`cputwo_cpu_synchronize_from_tb()`** — restore PC from translation block
- **`cputwo_restore_state_to_opc()`** — restore PC from OPC data
- **`cputwo_cpu_reset_hold()`** — set `pc=0`, `status=0x01`, all other regs/flags to 0, `estatus=0x02`
- **`cputwo_cpu_realizefn()`** — call `qemu_init_vcpu()`
- **`cputwo_cpu_class_init()`** — register `TCGCPUOps` and `SysemuCPUOps`

**TCGCPUOps structure** — must provide:
- `.initialize` → `cputwo_tcg_init()`
- `.translate_code` → `cputwo_cpu_translate_code()`
- `.get_tb_cpu_state`, `.synchronize_from_tb`, `.restore_state_to_opc`
- `.mmu_index` → `cputwo_cpu_mmu_index()`
- `.cpu_exec_interrupt` → `cputwo_cpu_exec_interrupt()`
- `.do_interrupt` → `cputwo_cpu_do_interrupt()`
- `.tlb_fill` → `cputwo_cpu_tlb_fill()`

### 2.5 `target/cputwo/helper.h` — TCG Helper Declarations

Declare runtime helpers for operations too complex for inline TCG:
- `DEF_HELPER_1(cputwo_halt, noreturn, env)` — halt exception
- `DEF_HELPER_1(cputwo_syscall, noreturn, env)` — SYSCALL entry
- `DEF_HELPER_1(cputwo_sysret, void, env)` — SYSRET sequence
- `DEF_HELPER_1(cputwo_kret, void, env)` — KRET sequence
- `DEF_HELPER_1(cputwo_sfence, void, env)` — TLB flush
- `DEF_HELPER_2(cputwo_div, i32, i32, i32)` — signed divide (for divide-by-zero check)
- `DEF_HELPER_2(cputwo_divu, i32, i32, i32)` — unsigned divide
- `DEF_HELPER_2(cputwo_mod, i32, i32, i32)` — signed modulo
- `DEF_HELPER_2(cputwo_modu, i32, i32, i32)` — unsigned modulo
- `DEF_HELPER_FLAGS_3(cputwo_cas, TCG_CALL_NO_WG, void, env, i32, i32)` — atomic CAS
- `DEF_HELPER_1(cputwo_interrupt_check, void, env)` — check pending interrupts

### 2.6 `target/cputwo/helper.c` — Helper Implementations

Implement the helpers declared above. Key logic from `emulator.c` maps here:
- `raise_exception()` logic → `cputwo_cpu_do_interrupt()` in cpu.c
- SYSCALL/SYSRET/KRET entry sequences → helpers
- MMU page table walk → `cputwo_cpu_tlb_fill()`
- Divide-by-zero checks → div/mod helpers

### 2.7 `target/cputwo/insn.decode` — Instruction Decoder Specification

Use QEMU's decodetree DSL to define instruction formats and bit patterns. This generates the decoder automatically from the ISA encoding:

```
# Instruction formats
&r_type      rd rs1 rs2 shift
&i_type      rd rs1 imm16
&b_type      cond offset20
&m_type      rd rs1 imm16
&j_type      rd offset20

# Format definitions  (bit 31 = MSB)
@r_type  ........ rd:4 rs1:4 rs2:4 shift:5 .......   &r_type
@i_type  ........ rd:4 rs1:4 imm16:16                 &i_type
@b_type  ........ cond:4 offset20:20                   &b_type
@m_type  ........ rd:4 rs1:4 imm16:16                 &m_type
@j_type  ........ rd:4 offset20:20                     &j_type

# R-type ALU (opcode in bits 31-24)
ADD      00000000 .... .... .... ..... .......   @r_type
SUB      00000001 .... .... .... ..... .......   @r_type
# ... etc for all 64 opcodes
```

### 2.8 `target/cputwo/translate.c` — TCG Translation

The largest file. Translates CPUTwo instructions into TCG (Tiny Code Generator) intermediate representation. This is where the emulator.c `switch(op)` logic becomes QEMU's JIT-compiled translation blocks.

**Key structures:**
- `DisasContext` — per-translation-block context (PC, privilege mode, etc.)
- TCG temporaries for registers (`cpu_r[16]`, `cpu_flags`, `cpu_status`, etc.)

**For each instruction, implement a `trans_<MNEMONIC>()` function** that emits TCG ops:

- **ALU ops** (ADD, SUB, AND, etc.): straightforward TCG arithmetic ops + flag update helpers
- **Loads/Stores** (LW, SW, LB, etc.): `tcg_gen_qemu_ld_*` / `tcg_gen_qemu_st_*` with appropriate MMU index
- **Branches** (B*): compare flags, emit conditional branch TCG ops
- **JMP/CALLR**: emit direct/indirect jump TCG ops
- **SYSCALL/SYSRET/KRET/HALT**: call runtime helpers (too complex for inline TCG)
- **SFENCE**: call TLB flush helper
- **CAS**: call atomic CAS helper

**Flag updates**: Create helper functions for each flag-update pattern:
- `gen_update_flags_add()` — emit TCG ops to compute N, Z, C (carry), V (overflow) for addition
- `gen_update_flags_sub()` — same for subtraction
- `gen_update_flags_nz()` — N and Z only, clear C and V
- No flag update for MOV/MOVI/MOVHI/LUI/loads/stores

### 2.9 `target/cputwo/gdbstub.c` — GDB Register Access

Implement `cputwo_cpu_gdb_read_register()` and `cputwo_cpu_gdb_write_register()`:
- Registers 0–15: r0–r15 (r13=sp, r14=lr, r15=pc)
- Register 16: flags
- Register 17: status
- Registers 18–24: epc, eflags, evec, cause, estatus, satp, badaddr

### 2.10 `target/cputwo/machine.c` — Migration State

Define `VMStateDescription` for `CPUTwoState` to support VM migration/snapshots. List all fields: `r[16]`, `flags`, `epc`, `eflags`, `evec`, `cause`, `status`, `estatus`, `satp`, `badaddr`.

### 2.11 `target/cputwo/Kconfig`

```
config CPUTWO
    bool
```

### 2.12 `target/cputwo/meson.build`

```python
gen = [decodetree.process('insn.decode', extra_args: ['--decode=decode'])]

cputwo_ss = ss.source_set()
cputwo_ss.add(gen)
cputwo_ss.add(files(
    'cpu.c',
    'gdbstub.c',
    'helper.c',
    'translate.c',
))

cputwo_system_ss = ss.source_set()
cputwo_system_ss.add(files(
    'machine.c',
))

target_arch += {'cputwo': cputwo_ss}
target_system_arch += {'cputwo': cputwo_system_ss}
```

### 2.13 `gdb-xml/cputwo-cpu.xml` — GDB Target Description

XML file describing all registers for GDB:
```xml
<?xml version="1.0"?>
<!DOCTYPE target SYSTEM "gdb-target.dtd">
<target>
  <architecture>cputwo</architecture>
  <feature name="org.gnu.gdb.cputwo.core">
    <reg name="r0" bitsize="32" type="uint32"/>
    <!-- r1–r12 -->
    <reg name="sp" bitsize="32" type="data_ptr"/>
    <reg name="lr" bitsize="32" type="code_ptr"/>
    <reg name="pc" bitsize="32" type="code_ptr"/>
    <reg name="flags" bitsize="32" type="uint32"/>
    <reg name="status" bitsize="32" type="uint32"/>
  </feature>
</target>
```

---

## Phase 3: Hardware/Machine (`hw/cputwo/`)

### 3.1 `hw/cputwo/cputwo_board.c` — Board/Machine Definition

Define a single machine type `cputwo-board` that creates the complete system:

**Memory map (matching the architecture spec):**

| Region | Address | Size | Description |
|--------|---------|------|-------------|
| RAM | 0x00000000 | 63 MB (0x03F00000) | Main memory |
| UART | 0x03F00000 | 0x1000 | Serial console |
| Timer | 0x03F01000 | 0x1000 | Countdown timer |
| IC | 0x03F02000 | 0x1000 | Interrupt controller |
| Block | 0x03F03000 | 0x1000 | Block device |
| CPU Control | 0x03FFF000 | 0x1000 | Supervisor registers |

**Implementation:**
- Create `TYPE_CPUTWO_MACHINE` using `DEFINE_MACHINE()`
- In `cputwo_board_init()`:
  1. Create CPU: `object_new(TYPE_CPUTWO_CPU)`
  2. Create RAM: `memory_region_init_ram()` for 63 MB
  3. Create MMIO devices (UART, Timer, IC, Block) as SysBus devices
  4. Wire interrupt lines from devices → IC → CPU
  5. Load kernel binary (ELF or raw) using QEMU's loader infrastructure (`load_elf()` or `load_image_targphys()`)

**Command-line interface:**
- `-M cputwo-board` — select this machine
- `-kernel <binary>` — load the guest binary (ELF or raw)
- `-drive file=disk.img,format=raw` — attach block device
- `-serial stdio` — UART connected to host stdio

### 3.2 `hw/cputwo/cputwo_uart.c` — UART Device

Implement as a `TYPE_SYS_BUS_DEVICE` with a `MemoryRegionOps` struct:

**Registers (offset from 0x03F00000):**
- `+0x00` Status (read): bit 0 = TX ready (always 1), bit 1 = RX available
- `+0x04` TX (write): send byte via QEMU chardev backend
- `+0x08` RX (read): consume buffered byte
- `+0x0C` Control (r/w): bit 0 = RX IRQ enable, bit 1 = TX IRQ enable

**IRQ lines:** Connect to IC bits 1 (RX) and 2 (TX).

Wire to QEMU's `CharBackend` (`qemu_chr_*`) for host I/O (connects to `-serial` option).

### 3.3 `hw/cputwo/cputwo_timer.c` — Timer Device

**Registers (offset from 0x03F01000):**
- `+0x00` Period/Count: write sets period and starts; read returns remaining count
- `+0x04` Control: bit 0 = enable, bit 1 = IRQ enable

**Behavior:** Periodic countdown, one tick per instruction (in QEMU, use a QEMU timer with an appropriate period). On reaching zero, reload period and fire interrupt if enabled.

**IRQ line:** Connect to IC bit 0.

Implementation note: Since CPUTwo defines a tick as one instruction, and QEMU uses icount for deterministic timing, enable icount mode (`-icount shift=0`) for accurate timer behavior. Alternatively, use a QEMU virtual clock timer calibrated to approximate instruction rate.

### 3.4 `hw/cputwo/cputwo_ic.c` — Interrupt Controller

**Registers (offset from 0x03F02000):**
- `+0x00` Pending (read-only): latched interrupt bits
- `+0x04` Enable mask (r/w): 1 = source enabled
- `+0x08` Acknowledge (write): clear pending bits

**Bits:** 0=Timer, 1=UART RX, 2=UART TX, 3=Block device

**Input IRQ lines:** 4 lines from devices (directly set pending bits).
**Output IRQ line:** Single line to CPU, asserted when `(pending & mask) != 0`.

### 3.5 `hw/cputwo/cputwo_blk.c` — Block Device

**Registers (offset from 0x03F03000):**
- `+0x00` Sector (r/w)
- `+0x04` Buffer address (r/w) — must be 512-byte aligned
- `+0x08` Command (write): 1=read, 2=write
- `+0x0C` Status (read): 0=idle, 1=busy, 2=error
- `+0x10` Control (r/w): bit 0 = IRQ enable

Wire to QEMU's `BlockBackend` (`blk_*`) API for host disk I/O. DMA transfers between guest RAM and the block backend using `address_space_rw()`.

### 3.6 `hw/cputwo/Kconfig`

```
config CPUTWO_BOARD
    bool
    default y
    depends on CPUTWO
    select UNIMP
```

### 3.7 `hw/cputwo/meson.build`

```python
cputwo_ss = ss.source_set()
cputwo_ss.add(when: 'CONFIG_CPUTWO_BOARD', if_true: files(
    'cputwo_board.c',
    'cputwo_uart.c',
    'cputwo_timer.c',
    'cputwo_ic.c',
    'cputwo_blk.c',
))
hw_arch += {'cputwo': cputwo_ss}
```

---

## Phase 4: MMU / Address Translation

The MMU is implemented in `target/cputwo/helper.c` via the `cputwo_cpu_tlb_fill()` function, which QEMU calls on TLB misses:

1. If `SATP.EN == 0` → identity mapping (physical = virtual)
2. If VA >= `0x03F00000` → MMIO bypass (always physical)
3. Otherwise → two-level Sv32 page table walk:
   - L1 index = `VA[31:22]`, L2 index = `VA[21:12]`
   - Check V, R/W/X, U bits; raise page faults (cause 0x07/0x08/0x09) on violations
   - Support 4 MB superpages (leaf at L1 level)
   - Set D bit on stores
   - Write `BADADDR` on faults

The supervisor register region (0x03FFF000) must check privilege and raise bus error (cause 0x02) for user-mode access. This is handled either in the TLB fill or by the MMIO read/write callbacks for the CPU control memory region.

### SFENCE / SATP Write

- `SFENCE` instruction → call `tlb_flush()` (QEMU API) to invalidate all TLB entries
- Writing `SATP` → also calls `tlb_flush()`

QEMU's softmmu TLB handles the caching; the page table walk only runs on TLB misses.

---

## Phase 5: Interrupt & Exception Handling

### `cputwo_cpu_do_interrupt()` — Exception Entry (in `cpu.c` or `helper.c`)

Implements the hardware exception entry sequence for all causes:

```
ESTATUS = STATUS
EPC     = faulting_pc       (or PC+4 for SYSCALL, or next-PC for IRQ)
EFLAGS  = flags
CAUSE   = cause_code
STATUS  = 0x01              (supervisor, IE=0)
PC      = mem[EVEC + cause * 4]
```

### `cputwo_cpu_exec_interrupt()` — Interrupt Dispatch

Called by QEMU's main loop. Check `STATUS.IE` and IC `pending & mask`. If set, request exception with cause 0x06.

---

## Phase 6: Disassembler

### 6.1 `target/cputwo/disas.c`

Port the `disasm()` function from `emulator.c` to QEMU's disassembler API (`print_insn_cputwo()`). Register it so `qemu -d in_asm` shows decoded instructions.

---

## Phase 7: Testing & Validation

### 7.1 Build Verification

```bash
mkdir build && cd build
../configure --target-list=cputwo-softmmu
make -j$(nproc)
```

### 7.2 Basic Smoke Test

Run a minimal CPUTwo binary (e.g., one that writes to UART and halts):
```bash
./qemu-system-cputwo -M cputwo-board -kernel test.bin -serial stdio -nographic
```

### 7.3 Cross-Validation Against Reference Emulator

Run the same binaries on both `emulatortwo` (the reference C emulator) and `qemu-system-cputwo`. Compare:
- UART output (should be identical)
- Final register state (via GDB or `-d cpu` logging)
- Interrupt/timer behavior
- MMU page fault behavior
- Block device I/O

### 7.4 xv6 Boot Test

If an xv6 port exists for CPUTwo, boot it under QEMU as the ultimate integration test — exercises MMU, interrupts, syscalls, UART, timer, and block device.

---

## File Summary

| Directory | File | Purpose |
|-----------|------|---------|
| `configs/targets/` | `cputwo-softmmu.mak` | Target config |
| `configs/devices/cputwo-softmmu/` | `default.mak` | Default device config |
| `gdb-xml/` | `cputwo-cpu.xml` | GDB register description |
| `target/cputwo/` | `cpu-param.h` | Target parameters |
| | `cpu-qom.h` | QOM type declarations |
| | `cpu.h` | CPU state struct, constants |
| | `cpu.c` | CPU lifecycle, interrupt handling |
| | `helper.h` | TCG helper declarations |
| | `helper.c` | Helper impls, MMU page walk |
| | `insn.decode` | Decodetree instruction spec |
| | `translate.c` | TCG instruction translation |
| | `gdbstub.c` | GDB register read/write |
| | `machine.c` | VM migration state |
| | `disas.c` | Disassembler |
| | `Kconfig` | Build config |
| | `meson.build` | Build rules |
| `hw/cputwo/` | `cputwo_board.c` | Machine/board definition |
| | `cputwo_uart.c` | UART device |
| | `cputwo_timer.c` | Timer device |
| | `cputwo_ic.c` | Interrupt controller |
| | `cputwo_blk.c` | Block device |
| | `Kconfig` | HW build config |
| | `meson.build` | HW build rules |

**Total: ~23 new files**, roughly following the pattern of `target/rx/` and `hw/rx/`.

---

## Implementation Order

Recommended order to get to a bootable system incrementally:

1. **Build scaffolding** — Phase 1 + skeleton files so `configure && make` succeeds
2. **CPU + translate (no MMU)** — Phase 2 core: cpu.c, translate.c with all ALU/branch/load/store/jump instructions. Flat memory only (SATP.EN=0).
3. **Board + UART** — Phase 3.1 + 3.2: minimal board with RAM + UART. Goal: run a "Hello World" binary.
4. **Timer + IC** — Phase 3.3 + 3.4: enable interrupt-driven programs.
5. **SYSCALL/SYSRET/KRET** — complete privilege mode transitions.
6. **MMU** — Phase 4: page table walk in tlb_fill, SFENCE support.
7. **Block device** — Phase 3.5: disk I/O.
8. **GDB + disassembler** — Phase 2.9 + 6: debugging support.
9. **Testing** — Phase 7: cross-validate against reference emulator, boot xv6.

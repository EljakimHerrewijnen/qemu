# QEMU Hedgehog Python API

This document describes the Python API exposed by `qemu.hedgehog` and the
runtime behavior that matters when embedding QEMU as a library.

## Overview

The Python layer is a Hedgehog-compatible wrapper around QEMU's in-tree C
backend API. The main entry point is `qemu.hedgehog.Hedgehog`.

Each `Hedgehog` instance encapsulates a single CPU and its execution context.
The CPU model, machine type, and backend library are chosen at construction
time and are **fixed for the lifetime of the instance**. There is no concept
of switching backends or machine types on an existing instance.

The wrapper supports two execution models:

- **Board-backed mode** (default): create one CPU with a private address space
  and add RAM or MMIO callback regions yourself. Selected when `machine_type`
  is omitted or `None`.
- **Machine-backed mode**: create a real QEMU machine and use its existing
  memory map and device models. Selected by passing a `machine_type` such as
  `"raspi3b"`.

## Installation

For development from this source tree:

```bash
cd python
python3 -m venv .venv
source .venv/bin/activate
pip install -e .
```

If you install from a wheel that bundles native backend libraries, the package
auto-discovers them from `qemu/hedgehog/_native`.

You can always override library selection with:

```bash
export QEMU_HEDGEHOG_BACKEND_LIBRARY=/absolute/path/to/libqemu-hedgehog-backend.so
```

Use the `-aarch64` backend library for AArch64 CPU models such as
`cortex-a53` or `cortex-a57`.

## Constructor

```python
from qemu.hedgehog import Hedgehog

emu = Hedgehog(arch, mode)
```

For board-backed mode with an explicit CPU type:

```python
from qemu.hedgehog import Hedgehog, HEDGEHOG_ARCH_X86, HEDGEHOG_MODE_64

emu = Hedgehog(HEDGEHOG_ARCH_X86, HEDGEHOG_MODE_64, cpu_type="qemu64-x86_64-cpu")
```

For machine-backed mode with device endpoints:

```python
from qemu.hedgehog import Hedgehog, HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM

emu = Hedgehog(
    HEDGEHOG_ARCH_ARM64,
    HEDGEHOG_MODE_ARM,
    cpu_type="cortex-a53",
    machine_type="raspi3b",
    chardevs={"console": "pty"},
    property_bindings={
        "/machine/soc/peripherals/uart0": {"chardev": "console"},
    },
)
```

Arguments:

- `arch`: Hedgehog-compatible architecture constant (e.g. `HEDGEHOG_ARCH_X86`).
- `mode`: Hedgehog-compatible mode constant (e.g. `HEDGEHOG_MODE_64`).
- `cpu_type`: QEMU CPU type string. Required when no built-in default exists
  for the given `arch`/`mode` combination.
- `machine_type`: QEMU machine type string. Omit for board-backed mode. Pass a
  machine name such as `"raspi3b"` for machine-backed mode. This is fixed for
  the life of the instance.
- `chardevs`: optional mapping of chardev IDs to QEMU chardev URIs such as
  `"pty"`, `"stdio"`, or `"socket,..."`. Applied during construction before
  the board is realized.
- `property_bindings`: optional mapping from QOM object paths to
  string-valued property assignments. Applied during construction before the
  board is realized. Use this to bind pre-existing machine devices to named
  chardevs or other backends.
- `serial_backends`: optional mapping of legacy serial indices to chardev IDs.
  Applies only to machine-backed mode and must be configured before the board
  is realized. Prefer `property_bindings` when the target device exposes a
  `chardev` property.
- `cpu_properties`: optional mapping of CPU property names to values. These
  are applied before board realization to the machine-created CPU.
- `library_path`: optional explicit shared library path. If omitted, the
  library is discovered via `$QEMU_HEDGEHOG_BACKEND_LIBRARY`, the bundled
  `_native/` directory, or the system linker.
- `coverage`: enable coverage tracking. Accepts `False` (default, disabled),
  `True` (block-level), a mode string such as `"block"`, or an iterable of
  mode strings. See [Coverage tracking](#coverage-tracking) for details.

The `backend` parameter is reserved for unit-test injection of a custom
`BackendProtocol` implementation and is not intended for production use.

## Memory API

Board-backed mode provides manual memory mapping:

```python
emu.mem_map(0x1000, 0x1000)
emu.mem_write(0x1000, b"\x90\x90\xf4")
data = emu.mem_read(0x1000, 3)
emu.mem_unmap(0x1000, 0x1000)

# Unicorn-style region query: (begin, end_inclusive, perms)
regions = emu.mem_regions()
```

An optional `perms` argument accepts a bitwise combination of `HEDGEHOG_PROT_*`
constants. It defaults to `HEDGEHOG_PROT_ALL`:

```python
from qemu.hedgehog import HEDGEHOG_PROT_READ, HEDGEHOG_PROT_EXEC

emu.mem_map(0x1000, 0x1000, HEDGEHOG_PROT_READ | HEDGEHOG_PROT_EXEC)
```

Standalone mode can also map caller-owned host memory directly as guest RAM:

```python
import ctypes

backing = (ctypes.c_ubyte * 0x1000)()
emu.mem_map_ptr(
    0x2000,
    len(backing),
    HEDGEHOG_PROT_ALL,
    ctypes.addressof(backing),
)
```

Guest and host accesses then share the same bytes. The caller must retain the
complete backing allocation until the mapping is removed or the Hedgehog
instance is closed. Host-backed RAM is rejected in machine-backed mode, just
like ordinary `mem_map()` RAM.

MMIO callbacks are available in board-backed mode:

```python
def mmio_read(offset: int, size: int) -> int:
    return 0

def mmio_write(offset: int, value: int, size: int) -> None:
    print(hex(offset), hex(value), size)

emu.mem_map_mmio(0x40000000, 0x1000, mmio_read, mmio_write)
```

In machine-backed mode the selected board's real device tree is used instead.
`mem_map()` is not available there, but `mem_map_mmio()` can overlay a
callback-backed MMIO region onto the board system memory map.

## Register API

The wrapper exposes raw and integer register helpers:

```python
value = emu.reg_read(0)                  # returns int (little-endian)
raw   = emu.reg_read_bytes(0, size=8)    # returns bytes
emu.reg_write(0, 0x1234)                 # write int
emu.reg_write(1, b"\x01\x00\x00\x00")   # write bytes
```

Register numbering follows the target's existing QEMU backend encoding.

Integer writes are padded to known target register widths for Arm and AArch64
core registers. For AArch64 gdbstub numbering, `x0..x30`, `sp`, and `pc` are
8-byte writes, while `pstate` is a 4-byte write.

## AArch64 reset state, system registers, and diagnostics

Standalone AArch64 sessions can select their architectural entry state before
guest execution:

```python
emu = Hedgehog(HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM, cpu_type="max")

emu.qemu_set_aarch64_reset_state(current_el=3, secure=True)
```

The helper is available only for standalone AArch64 backends. CPU-model and
machine properties should be configured through QEMU's normal CPU selection
and `property_bindings`, rather than by adding machine-specific register
profiles to the Hedgehog API.

Before the first run, callers may add a target-neutral stored AArch64 system
register by its architectural encoding. This example uses a deliberately
synthetic implementation-defined encoding rather than a machine profile:

```python
emu.qemu_add_aarch64_cp_reg(
    3, 7, 15, 15, 7,
    min_el=1,
    readable=True,
    writable=True,
    reset_value=0,
)
```

An existing QEMU system register can instead receive explicit live and reset
values without replacing its architectural access checks:

```python
emu.qemu_set_aarch64_cp_reg_value(
    3, 0, 0, 0, 5,
    reset_value=0,
    value=0,
)
```

Both operations are available only for standalone AArch64 sessions and must
complete before execution begins. Adding a duplicate encoding is rejected;
the value helper requires an encoding already supplied by QEMU. A reset
restores every configured reset value. Unknown encodings retain QEMU's normal
undefined-instruction behavior. Keep board- and product-specific register
profiles in the embedding application rather than in this generic API.

`qemu_arm_diagnostics()` returns read-only ARM CPU state useful for firmware
frontier debugging. The dictionary includes `pc`, `pstate`, `current_el`,
`scr_el3`, `hcr_el2`, `hcr_el2_eff`, `elr_el1..3`, `spsr_el1..3`,
`esr_el1..3`, `far_el1..3`, `exception_syndrome`, `exception_vaddress`,
`exception_target_el`, `exception_index`, and CPU run/stop flags. It also
includes a read-only generic-timer record: `generic_timer_counter`, plus seven
index-aligned `generic_timer_cval`, `generic_timer_ctl`,
`generic_timer_enabled`, `generic_timer_imask`, and
`generic_timer_istatus` arrays. The index order is QEMU's architectural order:
physical EL1, virtual EL1, EL2 physical, EL3 physical, EL2 virtual, secure-EL2
physical, secure-EL2 virtual. Reading the record does not service a timer,
modify a deadline, drive an output, or inject an interrupt.

### Invalid-memory diagnostic capture

Before execution, an embedding application can enable one bounded,
host-observational record for the next failed memory transaction:

```python
emu.qemu_set_invalid_memory_diagnostic_capture()
# Run normally, then inspect after an invalid-memory terminal result.
diagnostic = emu.qemu_last_invalid_memory_diagnostic()
```

When available, `diagnostic` is an `InvalidMemoryDiagnostic` with the fault
virtual address, translated physical address, MMU index, access size/type,
`MemTxResult`, and the resolved `MemoryRegion` base/name. A failed page-table
walk has no completed TLB or region result, so its translated-address and
region fields are `None` and the respective validity flags are false. A
MemoryRegion without a QOM name likewise returns `region_name=None`.

The record is cleared at reset and at the start of each native run. Capture
does not install a guest hook, add a memory mapping, alter the memory
transaction, or change normal behavior while disabled.

## Execution API

The Hedgehog-compatible entry points are:

```python
emu.emu_start(begin=entry, until=0, count=1000)
emu.emu_stop()
```

`emu_start` sets the PC to `begin`, then runs:

- For `count > 0`: runs at most `count` instructions and returns.
- For `until != 0`: installs a stop-on-PC hook and runs until the PC matches
  `until`, or the budget is exhausted.
- When exec hooks or coverage are active and `count == 0`: runs in chunks of
  `0x1000` instructions until a stop condition is met.
- Otherwise: runs with no instruction budget.

`emu_start` raises a `HedgehogError` for CPU exceptions and invalid-memory
faults. It does not expose backend-specific stop reasons such as
`QEMU_HEDGEHOG_RUN_HALTED`.

Standalone device models can drive the CPU's level-triggered hard interrupt
input with `qemu_set_hard_interrupt(asserted)`.  Asserting the input wakes an
architectural `WFI`/`WFE` halt and leaves delivery, masking, exception routing,
and vector entry to the target CPU model.  The caller must deassert the input
when its interrupt controller no longer has a deliverable interrupt; this API
models the CPU input line and does not acknowledge or synthesize an interrupt.
Board-backed machines must drive their board interrupt controller instead;
calling this standalone input API for such a machine is rejected.

The standalone run loop also consumes QEMU's internal `EXCP_YIELD` scheduler
exit and resumes the same CPU.  A guest `YIELD` hint is therefore an ordinary
executed instruction, as it is in QEMU's system CPU loop; it is not reported as
a guest exception merely because the embedding backend has no second vCPU to
schedule.

QEMU-specific helpers are also exposed:

```python
emu.qemu_set_pc(entry)
pc = emu.qemu_get_pc()
run_result, cpu_exit = emu.qemu_run(max_instructions=1000)
```

`qemu_run` returns the raw backend status as a `(run_result, cpu_exit)` tuple.
The run result is one of:

| Constant | Meaning |
|---|---|
| `QEMU_HEDGEHOG_RUN_BUDGET_EXHAUSTED` | Instruction budget reached |
| `QEMU_HEDGEHOG_RUN_STOP_REQUESTED` | `emu_stop()` was called |
| `QEMU_HEDGEHOG_RUN_HALTED` | CPU entered architectural wait state |
| `QEMU_HEDGEHOG_RUN_EXCEPTION` | Unhandled CPU exception |
| `QEMU_HEDGEHOG_RUN_INVALID_MEMORY` | Guest accessed unmapped or protected memory |

In standalone mode, Hedgehog owns the selected CPU while `qemu_run` is active:
QEMU's normal vCPU scheduler remains parked, and the caller thread drains
queued safe CPU work between execution intervals. This is required for guest
operations such as broadcast TLB maintenance, which queue CPU work even when
only one standalone CPU is present.

Standalone execution also owns QEMU's shared virtual clock. The clock starts
when a direct run begins and freezes when the last active standalone run ends,
including a halt, instruction budget, exception, or asynchronous stop. Creating
a standalone backend does not start guest time. Host delays between runs and
read-only inspection therefore do not advance architectural counter registers
when no other standalone run is active. Board-backed sessions retain their
existing VM-clock lifecycle. Python permits only one native backend creation
per process; the C lifecycle accounts for shared clock ownership across direct
runs rather than stopping another active run's clock.

This is pause semantics, not deterministic virtual-time scheduling. During an
active run the clock remains host-paced, including time inside callbacks.
Fixed-rate instruction-counted time, deterministic idle advancement, and clock
state capture/restore require separate APIs and are not provided by this change.
External host wall-clock timeouts remain independent of guest time.

The direct runner does not currently pump QEMU virtual timer lists. The
`qemu_events_poll()` helper iterates GLib sources and is not a replacement for
that timer dispatch: a future AArch64 physical-timer deadline can pass during
direct execution without updating `CNTP_CTL_EL0.ISTATUS`. This existing timer
integration gap is separate from pause-clock ownership and from the explicit
standalone hard-interrupt input API. Freezing host pauses does not repair it.

Use `qemu_run` directly when you need to inspect or react to
`QEMU_HEDGEHOG_RUN_HALTED`. For example, Arm64 firmware that idles with `WFI`
while waiting for a device interrupt:

```python
from qemu.hedgehog.constants import (
    QEMU_HEDGEHOG_RUN_BUDGET_EXHAUSTED,
    QEMU_HEDGEHOG_RUN_HALTED,
)

emu.qemu_set_pc(entry)

while True:
    run_result, cpu_exit = emu.qemu_run(100_000)

    if run_result == QEMU_HEDGEHOG_RUN_HALTED:
        pc = emu.qemu_get_pc()
        print(f"guest halted/waiting at pc=0x{pc:x}")
        break

    if run_result != QEMU_HEDGEHOG_RUN_BUDGET_EXHAUSTED:
        raise RuntimeError(f"unexpected run result={run_result}, cpu_exit={cpu_exit}")
```

## Host-connected backends

QEMU-specific methods expose the chardev and QOM property binding APIs for
machine-backed sessions. All bindings are applied before the board is realized,
either through the constructor arguments or through explicit calls before the
first `emu_start`.

Constructor-time binding is the reliable path for devices that wire their
properties during board creation:

```python
emu = Hedgehog(
    HEDGEHOG_ARCH_ARM64,
    HEDGEHOG_MODE_ARM,
    cpu_type="cortex-a53",
    machine_type="raspi3b",
    chardevs={"console": "pty"},
    property_bindings={
        "/machine/soc/peripherals/uart0": {"chardev": "console"},
    },
)
pty_path = emu.qemu_chardev_get_endpoint("console")
print(pty_path)
```

Equivalent explicit calls:

```python
emu.qemu_chardev_add("console", "pty")
emu.qemu_property_bind("/machine/soc/peripherals/uart0", "chardev", "console")
pty_path = emu.qemu_chardev_get_endpoint("console")
```

For convenience, `qemu_chardev_bind()` is a thin wrapper around
`qemu_property_bind()` when the target property expects a chardev ID.

The current implementation is intentionally narrow:

- chardev creation is supported;
- generic string-valued device property binding is supported;
- legacy serial slot binding is supported for existing board models;
- endpoint discovery is supported for backends such as PTY;
- event processing is explicit via `qemu_events_poll()`.

Legacy serial slot binding is available via `qemu_chardev_attach_serial()`:

```python
emu.qemu_chardev_attach_serial(0, "console")
```

Prefer `property_bindings` or `qemu_property_bind()` when the target device
exposes an explicit `chardev` property.

Event pumping matters for host-driven backends:

```python
emu.qemu_events_poll()              # non-blocking: process pending events
emu.qemu_events_poll(block=True)    # blocking: wait for host-side activity
```

Use the blocking form when you want to wait for host-side activity. Use the
non-blocking form when integrating into an external event loop.

The current host-backend implementation covers:

- chardev creation (`pty`, `stdio`, `socket,...`)
- generic string-valued QOM property binding
- legacy serial slot binding
- PTY/socket endpoint discovery
- explicit event processing

Other backend families (block, net, USB) are not exposed yet.

## Hooks

Supported hook families:

| Constant | Fires on |
|---|---|
| `HEDGEHOG_HOOK_BLOCK` | Start of each translated basic block |
| `HEDGEHOG_HOOK_CODE` | Each instruction |
| `HEDGEHOG_HOOK_INSN_INVALID` | An instruction rejected by the CPU model |
| `HEDGEHOG_HOOK_MEM_READ` | Python API `mem_read()` operations |
| `HEDGEHOG_HOOK_MEM_WRITE` | Python API `mem_write()` operations |
| `HEDGEHOG_HOOK_MEM_INVALID` | Any unmapped or protected memory access |
| `HEDGEHOG_HOOK_MEM_READ_UNMAPPED` | Unmapped read |
| `HEDGEHOG_HOOK_MEM_WRITE_UNMAPPED` | Unmapped write |
| `HEDGEHOG_HOOK_MEM_FETCH_UNMAPPED` | Unmapped fetch (instruction) |

Example:

```python
from qemu.hedgehog import HEDGEHOG_HOOK_CODE

def on_code(emu, address, size, user_data):
    print(hex(address))
    return False

handle = emu.hook_add(HEDGEHOG_HOOK_CODE, on_code)
emu.hook_del(handle)
```

Unicorn-style convenience helpers are also available:

```python
code_handle = emu.hook_code(0x1000, 0x1fff, on_code)
block_handle = emu.hook_block(0x1000, 0x1fff, on_code)
emu.hook_del(code_handle)
emu.hook_del(block_handle)
```

Unsupported instructions can be handled in Python and execution can continue:

```python
def emulate_instruction(emu, instruction, user_data):
    if instruction.data != bytes.fromhex("00000000"):
        return False  # deliver the original exception to the guest

    emu.reg_write(0, 0)
    return True       # handled; resume at pc + instruction.size

handle = emu.hook_invalid_instruction(emulate_instruction)
```

The `InvalidInstruction` object contains `pc`, `data`, `size`, `syndrome`, and
`exception_index`. Returning `None` or `False` preserves normal QEMU exception
delivery. Returning `True` advances by the instruction size. Returning a
non-boolean integer uses that value as the next PC, which is useful when the
emulation replaces a control-flow instruction. Callback exceptions stop the
run and are re-raised as a `HedgehogError` with the original exception chained.

The callback runs only after the selected CPU model has raised an undefined-
instruction exception; it adds no Python callback to successfully decoded
instructions. This makes it suitable for rare compatibility gaps while CPU
features with general value still belong in QEMU's target implementation.

Explicit helpers are available for region-scoped memory hooks:

```python
def on_mem(emu, access, addr, size, value, user_data):
  print(access, hex(addr), size, hex(value))

read_handle = emu.hook_mem_read(0x1000, 0x1fff, on_mem)
write_handle = emu.hook_mem_write(0x1000, 0x1fff, on_mem)

emu.hook_del(read_handle)
emu.hook_del(write_handle)
```

Optional `begin` and `end` arguments restrict the hook to a guest address
range. Both default to the entire address space.

Hook return behavior:

- Code and block hooks: return `True` to request a stop.
- Mem read/write hooks: return value is ignored.
- Invalid-memory hooks: return `True` to continue execution after the invalid
  access, or `False` to let the backend stop with an invalid-memory result.
- Invalid-instruction hooks: return `True` to advance, an integer to choose the
  next PC, or `False`/`None` to preserve normal exception delivery.

For this phase, mem read/write hooks are emitted for explicit Python API memory
operations (`mem_read` / `mem_write`). Full guest-memory-access tracing across
all CPU accesses is not part of this slice.

## Coverage tracking

Coverage collection is enabled at construction time and is active for the
lifetime of the instance. It cannot be enabled or disabled after construction.

```python
emu = Hedgehog(HEDGEHOG_ARCH_X86, HEDGEHOG_MODE_64, coverage='block')
```

Available modes:

| Mode | Tracks | Output keys in `get_coverage()` |
|---|---|---|
| `'block'` | Unique basic blocks | `blocks`, `unique_blocks` |
| `'insn'` | Unique instructions | `insn`, `unique_insn` |
| `'digest'` | BLAKE2b hash of covered blocks | `coverage_digest` |
| `'edge_digest'` | BLAKE2b hash of block transitions | `edge_digest`, `unique_edges` |

Pass `coverage=True` to enable block-level tracking. Pass a string or an
iterable of strings to enable multiple modes simultaneously.

```python
emu = Hedgehog(
    HEDGEHOG_ARCH_X86, HEDGEHOG_MODE_64,
    coverage=('block', 'edge_digest'),
)

emu.mem_map(0x1000, 0x1000)
emu.mem_write(0x1000, code)
emu.emu_start(0x1000, until=0, count=10000)

cov = emu.get_coverage()
print(f"unique blocks : {cov['unique_blocks']}")
print(f"edge digest   : {cov['edge_digest']}")
```

Coverage accumulates across multiple `emu_start` calls. Reset between runs:

```python
emu.clear_coverage()     # or emu.reset_coverage() (alias)
```

Standalone digest methods are also available:

```python
digest = emu.get_coverage_digest()  # BLAKE2b of block PCs
edges  = emu.get_edge_digest()      # BLAKE2b of (src, dst) edge pairs
```

See `coverage.md` for full mode details and fuzzing integration guidance.

## Machine-backed device behavior

In machine-backed mode, reads and writes to real device models do not use the
Python MMIO callback API. They go through the board's actual QEMU device model.

That means:

- valid device accesses normally continue execution automatically;
- invalid or faulting accesses can still surface as invalid-memory or exception
  results;
- if firmware polls a device status bit waiting for external input, execution
  can appear to "run indefinitely" even though the backend is still alive and
  simply revisiting the same poll loop.

A blocking UART receive routine, for example, loops until input becomes
available. If no input source is attached or injected, the guest will stay in
that loop until the instruction budget expires.

Another common embedded firmware pattern:

1. Program a device and enable its interrupt.
2. Execute `WFI`/`WFE` in an idle loop.
3. Resume only when the device signals completion.

In that case `qemu_run()` repeatedly returns `QEMU_HEDGEHOG_RUN_HALTED` until
your host-side code injects the expected event:

```python
from qemu.hedgehog.constants import QEMU_HEDGEHOG_RUN_HALTED

while True:
    run_result, _cpu_exit = emu.qemu_run(50_000)

    if run_result == QEMU_HEDGEHOG_RUN_HALTED:
        if device_model.has_pending_rx_data():
            device_model.inject_rx_byte(0x41)
            continue
        # No external event to deliver yet — pump I/O and loop.
        emu.qemu_events_poll(block=True)
        continue

    # Handle other run results as needed.
```

## Lifecycle

Create one native `Hedgehog` instance per process. Release it when done:

```python
emu.close()
```

Context-manager usage is also supported and calls `close()` automatically:

```python
with Hedgehog(HEDGEHOG_ARCH_X86, HEDGEHOG_MODE_64) as emu:
    emu.mem_map(0x1000, 0x1000)
    emu.emu_start(0x1000, until=0, count=100)
```

`close()` first unmaps every RAM, host-backed RAM, and MMIO region, waiting for
QEMU's readers to stop referencing them, and only then detaches Python
callbacks. It does not tear down QEMU's process-global TCG runtime. Keep every
buffer passed to `mem_map_ptr()` alive until `mem_unmap()` or `close()` returns.
If you need multiple independent native emulator sessions, run each one in a
separate subprocess.

## Current limitations

- Only a subset of Hedgehog hooks is implemented (`BLOCK`, `CODE`,
  `INSN_INVALID`, `MEM_READ`, `MEM_WRITE`, `MEM_INVALID` family).
- Native backends can only be initialized once per process.
- Host-connected device support covers chardev-backed connections and
  string-valued property binding. Block, net, and USB backend families are
  not exposed yet.

### `HedgehogError(... only be initialized once per process ...)`

The native backend is a process-local singleton because embedded QEMU TCG
teardown is not currently safe for repeated create/close cycles. Use a fresh
subprocess for each native emulator session.

## Troubleshooting

### `failed to create backend for cpu type ...`

This now includes backend-side detail when available, for example unknown or
abstract CPU model errors returned by QEMU.

Common cause: loading the wrong backend library for the requested CPU
architecture. For ARM and AArch64 CPU models (for example `cortex-a9`), use
the `libqemu-hedgehog-backend-aarch64.so` variant.

If needed, pin the library explicitly:

```bash
QEMU_HEDGEHOG_BACKEND_LIBRARY=/path/to/libqemu-hedgehog-backend-aarch64.so
```

### `HedgehogError(HEDGEHOG_ERR_*_UNMAPPED)`

The guest touched unmapped memory and no invalid-memory hook chose to continue.
In board-backed mode, ensure all guest memory ranges have been mapped with
`mem_map()` before starting execution.

### `property_bindings require a board-backed machine_type`

`property_bindings` and `serial_backends` are only valid when a non-`None`
`machine_type` is provided to the constructor.

### Execution appears to stop without progress

Use `qemu_run()` and `qemu_get_pc()` to inspect the raw backend status:

```python
emu.qemu_reset_stop()
run_result, cpu_exit = emu.qemu_run(2000)
print(run_result, cpu_exit, hex(emu.qemu_get_pc()))
```

For a new asynchronously stopped run, call `qemu_reset_stop()` before arming
the timer or worker that may call `emu_stop()`. `qemu_run()` never clears a
pending stop on entry, so a timeout that fires immediately is still observed.

If the result is `QEMU_HEDGEHOG_RUN_BUDGET_EXHAUSTED` and the PC stays in the
same small range, the guest is still running but polling a device. That is not
the same as a backend stop caused by MMIO.

If the result is `QEMU_HEDGEHOG_RUN_HALTED`, the CPU entered an architectural
wait state (`WFI`/`WFE`). Use the `qemu_run` loop pattern above to drive
execution while delivering host-side events.

### Standalone timer dispatch and idle execution

Standalone direct runs own one process-wide event dispatcher while at least
one run is active. It uses QEMU's main loop and timer queues, with the normal
BQL/replay locking, so virtual timer deadlines run during native execution and
idle. The final run stops and joins the dispatcher before freezing virtual
time. Board-backed execution keeps its existing event-loop ownership.
Explicit event polling does not compete with an active standalone dispatcher.

`qemu_virtual_clock_ns()` observes the native virtual clock without running
instructions or changing counter registers. Host pauses leave this clock
frozen. Active execution remains host-paced; this is not an instruction-count
clock or a deterministic snapshot-clock implementation.

Unbounded standalone execution waits at WFI for native interrupt work or an
external stop instead of returning merely because the CPU is idle. Finite
single-step execution retains its HALTED result. The external stop wakes idle
and dispatcher waits. Timer status expiration alone does not demonstrate IRQ
delivery: standalone CPU output lines must also be connected to the embedding
interrupt-controller model.

### System-call observer

Standalone AArch64 callers may register one read-only observer before the
first execution:

```python
emu.qemu_connect_system_call_observer(on_system_call)
```

The callback receives an immutable `SystemCallEvent` on the direct-run caller
thread, outside QEMU's BQL. A `request` record identifies the decoded `svc`,
`hvc`, or `smc` instruction and includes its PC, source EL, immediate, return
PC, and `x0..x7`. A `complete` record preserves that request metadata and
carries its actual target EL, return EL, and `x0..x7` after an ERET from that
target. Calls handled by the host, blocked by routing, or not returned through
the matching target ERET retain a request row only; the observer never
synthesizes a response. Native pending and delivery queues are bounded to
4,096 records. If the observer falls behind, their oldest records can be
dropped, so consumers must treat sequence gaps as lost data. The observer is
diagnostic only: it does not handle system calls, alter exception routing, or
modify guest state.

`qemu_connect_cpu_output(index, callback)` connects an anonymous native CPU
GPIO output during setup. The target CPU defines the output index; for ARM,
these are the native generic-timer outputs. Native callbacks capture ordered
assert/deassert events and wake the direct runner. Host callbacks execute only
on that runner, outside BQL, so they can safely update an embedding interrupt
controller without a dispatcher-thread BQL/Python-GIL inversion. The embedding
controller remains responsible for level retention, masks, acknowledgment, and
interrupt routing. Connecting new outputs after execution is rejected.

### CPU-wait observer and external reset

Standalone ARM callers may register one CPU-wait observer before the first
execution:

```python
emu.qemu_connect_cpu_wait_observer(on_cpu_wait)
```

The callback receives an immutable `CpuWaitEvent` on the direct-run caller
thread, outside QEMU's BQL, only after an architectural `WFI` was accepted
rather than trapped or bypassed by already-pending CPU work. The event contains
its kind and current exception level and is observational only. An embedding
power controller that has independently established a complete power-collapse
and wake contract may call `qemu_request_cpu_reset()` from a later observer;
the queued reset resumes from the configured reset vector. It never assigns a
guest PC, register, or memory value on behalf of the callback.

This is intended for an embedding power controller that has independently
established a full power-collapse and wake contract. It is not an interrupt
injector, a generic PSCI implementation, or a way to turn arbitrary waits into
resets. Native delivery is bounded to 4,096 queued records; a queue overflow
drops oldest unread observations and callers must not infer a missing wait.
Connecting or replacing the observer after execution begins is rejected.

### Post-reset observer

Standalone ARM callers may register one notification before the first
execution:

```python
emu.qemu_connect_cpu_reset_observer(on_cpu_reset)
```

The zero-argument callback runs on the direct-run caller outside QEMU's BQL,
but only after a prior `qemu_request_cpu_reset()` has crossed its direct-run
boundary, completed QEMU's normal CPU reset, and applied the configured reset
profile. It is a notification, not a reset implementation: it does not expose
or change guest PC, registers, memory, reset cause, timers, or interrupts.
It lets an embedding replay an already-retained host input signal that QEMU's
CPU reset cleared. Connecting or replacing it after execution begins is
rejected.

### Standalone device deadlines

`qemu_connect_virtual_timer(callback)` registers one callback before execution.
`qemu_arm_virtual_timer(deadline_ns)` arms an absolute native virtual-clock
deadline; `None` cancels both queued and scheduled delivery. The embedding
application can multiplex device deadlines onto this one-shot timer. Native
expiry requests a CPU exit or wakes its halted wait; only the direct-run caller
invokes the callback, outside BQL. The callback can service device state and
rearm its next deadline. Expiry alone does not assert a CPU interrupt.
Host pauses freeze this clock; external execution stops remain wall-clock
controlled. Closing disconnects the callback and frees its native timer.
Board-backed sessions do not expose this standalone timer facility.

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

MMIO callbacks are available in board-backed mode:

```python
def mmio_read(offset: int, size: int) -> int:
    return 0

def mmio_write(offset: int, value: int, size: int) -> None:
    print(hex(offset), hex(value), size)

emu.mem_map_mmio(0x40000000, 0x1000, mmio_read, mmio_write)
```

In machine-backed mode the selected board's real device tree is used instead.
`mem_map()` and `mem_map_mmio()` are not available in machine-backed mode.

## Register API

The wrapper exposes raw and integer register helpers:

```python
value = emu.reg_read(0)                  # returns int (little-endian)
raw   = emu.reg_read_bytes(0, size=8)    # returns bytes
emu.reg_write(0, 0x1234)                 # write int
emu.reg_write(1, b"\x01\x00\x00\x00")   # write bytes
```

Register numbering follows the target's existing QEMU backend encoding.

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

`close()` detaches Python callbacks, but it does not tear down QEMU's
process-global TCG runtime. If you need multiple independent native emulator
sessions, run each one in a separate subprocess.

## Current limitations

- Only a subset of Hedgehog hooks is implemented (`BLOCK`, `CODE`,
  `MEM_READ`, `MEM_WRITE`, `MEM_INVALID` family).
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
run_result, cpu_exit = emu.qemu_run(2000)
print(run_result, cpu_exit, hex(emu.qemu_get_pc()))
```

If the result is `QEMU_HEDGEHOG_RUN_BUDGET_EXHAUSTED` and the PC stays in the
same small range, the guest is still running but polling a device. That is not
the same as a backend stop caused by MMIO.

If the result is `QEMU_HEDGEHOG_RUN_HALTED`, the CPU entered an architectural
wait state (`WFI`/`WFE`). Use the `qemu_run` loop pattern above to drive
execution while delivering host-side events.
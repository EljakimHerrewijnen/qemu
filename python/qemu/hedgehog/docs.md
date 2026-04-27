# QEMU Hedgehog Python API

This document describes the Python API exposed by `qemu.hedgehog` and the
runtime behavior that matters when embedding QEMU as a library.

## Overview

The Python layer is a Hedgehog-compatible wrapper around QEMU's in-tree C
backend API. The main entry point is `qemu.hedgehog.Hedgehog`.

The wrapper supports two execution models:

- Board-backed mode: create one CPU with a private address space and add RAM
  or MMIO callback regions yourself.
- Machine-backed mode: create a real QEMU machine and use its existing memory
  map and device models.

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

emu = Hedgehog(
    arch,
    mode,
    cpu_type="qemu64-x86_64-cpu",
    machine_type="none",
)
```

For a machine-backed configuration that attaches a host endpoint to an
existing board device, pass `chardevs` and `property_bindings` at
construction time:

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
```

Arguments:

- `arch`, `mode`: Hedgehog-compatible architecture and mode constants.
- `cpu_type`: optional explicit QEMU CPU type. Required when no default exists.
- `machine_type`: optional QEMU machine type. If omitted, the backend uses the
  private board-backed mode.
- `chardevs`: optional mapping of chardev IDs to QEMU chardev URIs such as
  `pty`, `stdio`, or `socket,...`.
- `property_bindings`: optional mapping from QOM object paths to string-valued
  property assignments. This is the generic path for binding pre-existing
  machine devices to named backends such as chardevs.
- `serial_backends`: optional mapping of legacy serial indices to chardev IDs.
  This currently applies only to machine-backed mode and must be configured
  before the board is realized.
- `library_path`: optional explicit shared library path.
- `backend`: test-only injection point for a custom backend implementation.

## Memory API

Board-backed mode provides manual memory mapping:

```python
emu.mem_map(0x1000, 0x1000)
emu.mem_write(0x1000, b"\x90\x90\xf4")
data = emu.mem_read(0x1000, 3)
```

MMIO callbacks are available in board-backed mode:

```python
def mmio_read(offset: int, size: int) -> int:
    return 0

def mmio_write(offset: int, value: int, size: int) -> None:
    print(hex(offset), hex(value), size)

emu.mem_map_mmio(0x40000000, 0x1000, mmio_read, mmio_write)
```

Machine-backed mode uses the selected board's real device tree instead.
`mem_map()` and `mem_map_mmio()` are not available as overlays there.

## Register API

The wrapper exposes raw and integer register helpers:

```python
value = emu.reg_read(0)
raw = emu.reg_read_bytes(0, size=8)
emu.reg_write(0, 0x1234)
emu.reg_write(1, b"\x01\x00\x00\x00")
```

Register numbering follows the target's existing QEMU backend encoding.

## Execution API

The high-level Hedgehog-compatible calls are:

```python
emu.emu_start(begin=entry, until=0, count=1000)
emu.emu_stop()
```

QEMU-specific helpers are also exposed:

```python
emu.qemu_set_pc(entry)
pc = emu.qemu_get_pc()
run_result, cpu_exit = emu.qemu_run(1000)
```

The native run result matches the C enum:

- `QEMU_HEDGEHOG_RUN_BUDGET_EXHAUSTED`
- `QEMU_HEDGEHOG_RUN_STOP_REQUESTED`
- `QEMU_HEDGEHOG_RUN_HALTED`
- `QEMU_HEDGEHOG_RUN_EXCEPTION`
- `QEMU_HEDGEHOG_RUN_INVALID_MEMORY`

`emu_start()` converts exception and invalid-memory results into
`HedgehogError` exceptions, but it intentionally does not expose
backend-specific stop reasons such as `HALTED`.

If you need to stop when the CPU enters an architectural wait state
(for example Arm64 `WFI`, or `WFE` on profiles where it halts), use
`qemu_run()` directly and inspect the run result:

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
        print(f"guest halted/waiting at pc=0x{pc:x}, cpu_exit={cpu_exit}")
        break

    if run_result != QEMU_HEDGEHOG_RUN_BUDGET_EXHAUSTED:
        raise RuntimeError(f"unexpected run result={run_result}, cpu_exit={cpu_exit}")
```

For Arm64 specifically, this pattern is useful when guest firmware idles with
`WFI` while waiting for a device interrupt.

## Host-connected backends

The wrapper now exposes a first QEMU-specific host backend API for chardevs.
This is the path to connect machine-backed guest UARTs to host endpoints such
as PTYs.

Constructor-time configuration:

```python
emu = Hedgehog(
    HEDGEHOG_ARCH_ARM64,
    HEDGEHOG_MODE_ARM,
    cpu_type="cortex-a53",
    machine_type="raspi3b",
    chardevs={"console": "pty"},
    property_bindings={
        "/machine/soc/peripherals/uart0": {
            "chardev": "console",
        },
    },
)

pty_path = emu.qemu_chardev_get_endpoint("console")
print(pty_path)
```

You can also manage chardevs explicitly:

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

`serial_backends` remains available as a compatibility path for board code
that still consumes QEMU's legacy serial slot array. Prefer
`property_bindings` when the target device exposes a `chardev` or similar
string property.

For machine models that wire device properties during board creation,
constructor-time `property_bindings` are the reliable path because the
property must be set before the board code falls back to its own default.

Event pumping matters for host-driven backends:

```python
emu.qemu_events_poll()
emu.qemu_events_poll(block=True)
```

Use the blocking form when you want to wait for host-side activity. Use the
non-blocking form when integrating Hedgehog into an external event loop.

## Hooks

Currently supported hook families:

- `HEDGEHOG_HOOK_BLOCK`
- `HEDGEHOG_HOOK_CODE`
- `HEDGEHOG_HOOK_MEM_INVALID`
- `HEDGEHOG_HOOK_MEM_READ_UNMAPPED`
- `HEDGEHOG_HOOK_MEM_WRITE_UNMAPPED`
- `HEDGEHOG_HOOK_MEM_FETCH_UNMAPPED`

Example:

```python
from qemu.hedgehog import HEDGEHOG_HOOK_CODE

def on_code(emu, address, size, user_data):
    print(hex(address))
    return False

handle = emu.hook_add(HEDGEHOG_HOOK_CODE, on_code)
emu.hook_del(handle)
```

Hook return behavior:

- Code and block hooks: return `True` to request a stop.
- Invalid-memory hooks: return `True` to continue execution after the invalid
  access, or `False` to let the backend stop with an invalid-memory result.

## Machine-backed device behavior

In machine-backed mode, reads and writes to real device models do not use the
Python MMIO callback API. They go through the board's actual QEMU device model.

That means:

- valid device accesses normally continue execution automatically;
- invalid or faulting accesses can still surface as invalid-memory or exception
  results;
- if firmware polls a device status bit waiting for external input, execution
  can appear to "hang" even though the backend is still running and simply
  revisiting the same loop.

For example, a blocking UART receive routine may loop until input becomes
available. If no input source is attached or injected, the guest will stay in
that poll loop until your instruction budget expires.

Another common pattern in embedded firmware is:

1. Program a device and enable its interrupt.
2. Execute `WFI`/`WFE` in an idle loop.
3. Resume only when the device signals completion.

In that case, the emulator can repeatedly return `QEMU_HEDGEHOG_RUN_HALTED`
from `qemu_run()` until your host-side emulation injects the expected event.
A practical control flow is:

```python
from qemu.hedgehog.constants import QEMU_HEDGEHOG_RUN_HALTED

while True:
    run_result, _cpu_exit = emu.qemu_run(50_000)

    if run_result == QEMU_HEDGEHOG_RUN_HALTED:
        if device_model.has_pending_rx_data():
            device_model.inject_rx_byte(0x41)
            continue

        # No external event to deliver yet.
        # Sleep/poll host I/O, then run again.
        continue

    # Handle other run results as needed.
```

This keeps control in Python while the guest waits in architectural idle
states, and lets your device model decide when execution should wake up.

## Current limitations

- Only a subset of Hedgehog hooks is implemented.
- Repeated create/close cycles in one process are not fully reliable yet.
- Switching machine types within one process is not supported reliably.
- Host-connected device support currently starts with chardev-backed
  connections and string-valued property binding. Other backend families such
  as block, net, or USB plumbing are not exposed yet.

## Troubleshooting

### `failed to create backend for cpu type ...`

You likely loaded the wrong backend library for the CPU architecture.

### `HedgehogError(HEDGEHOG_ERR_*_UNMAPPED)`

The guest touched unmapped memory and no invalid-memory hook chose to continue.

### Execution appears to stop in a device access loop

Use `qemu_run()` and `qemu_get_pc()` to inspect the raw backend status:

```python
run_result, cpu_exit = emu.qemu_run(2000)
print(run_result, cpu_exit, hex(emu.qemu_get_pc()))
```

If the result is `QEMU_HEDGEHOG_RUN_BUDGET_EXHAUSTED` and the PC stays in the
same small range, the guest is still executing and likely polling a device.
That is not the same as a backend stop caused by MMIO.
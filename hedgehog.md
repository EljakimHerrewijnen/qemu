# Hedgehog Backend Overview

This document summarizes the current in-tree Hedgehog implementation in QEMU,
and shows a basic usage flow.

## What It Is

Hedgehog is an optional embedding backend that lets you use QEMU target CPUs and
memory machinery through a compact emulator-style API.

The implementation is split across:

- C backend API and runtime in `accel/hedgehog/` and `include/system/hedgehog-backend.h`
- TCG hook integration in `accel/tcg/hedgehog-exec-hooks.c`
- Python wrapper in `python/qemu/hedgehog/`

## Implementation Changes (Summary)

The following major changes are now in-tree for this implementation:

- Added a dedicated `hedgehog` build feature (`--enable-hedgehog`) and replaced
    previous Unicorn-oriented option wiring.
- Added a new backend implementation under `accel/hedgehog/` with lifecycle,
    memory mapping, MMIO callback mapping, execution control, and register access.
- Added hook integration through TCG execution and MMU paths:
    - TB/instruction callbacks in `accel/tcg/cpu-exec.c`
    - invalid-memory callbacks in `accel/tcg/cputlb.c`
- Added public headers:
    - `include/system/hedgehog-backend.h`
    - `include/system/hedgehog-exec-hooks.h`
- Added a Python compatibility package under `python/qemu/hedgehog/` and tests.
- Added architecture-specific backend library output support:
    - `build/libqemu-hedgehog-backend.so`
    - `build/libqemu-hedgehog-backend-aarch64.so` (when `aarch64-softmmu` exists)
- Added CPU model alias fallback for backend CPU selection (for model names such
    as `cortex-a57`).
- Fixed teardown ordering in backend free path to unrealize CPU/MMIO objects
    before final release, avoiding post-close assertion failures during process
    exit in AArch64 runs.

## Implementation Summary

### 1. Build and Feature Gating

The backend is controlled by a feature option:

- Configure flag: `--enable-hedgehog` / `--disable-hedgehog`
- Meson option: `hedgehog`

When disabled, the backend compiles out.

### 2. Backend Core Object

The C surface centers on `HedgehogBackend` with APIs for:

- lifecycle: `hedgehog_backend_new`, `hedgehog_backend_free`
- memory: `hedgehog_backend_map_ram`, `hedgehog_backend_map_mmio`,
  `hedgehog_backend_mem_read`, `hedgehog_backend_mem_write`
- registers: `hedgehog_backend_reg_read`, `hedgehog_backend_reg_write`
- execution: `hedgehog_backend_set_pc`, `hedgehog_backend_get_pc`,
  `hedgehog_backend_run`, `hedgehog_backend_stop`
- hooks: `hedgehog_backend_set_tb_hook`, `hedgehog_backend_set_insn_hook`,
  `hedgehog_backend_set_invalid_mem_hook`

### 3. Memory Model

The backend reuses standard QEMU memory objects:

- RAM mappings are real `MemoryRegion` RAM subregions.
- MMIO mappings are callback-backed device regions.

This keeps behavior aligned with normal QEMU memory and TLB/MMU handling.

### 4. Execution Hooks and Stop Behavior

Hook dispatch is integrated via TCG execution and MMU paths:

- translation-block and instruction hooks
- invalid-memory hook path (unmapped/protection/fill failure scenarios)

Run results include explicit statuses:

- `HEDGEHOG_RUN_BUDGET_EXHAUSTED`
- `HEDGEHOG_RUN_STOP_REQUESTED`
- `HEDGEHOG_RUN_HALTED`
- `HEDGEHOG_RUN_EXCEPTION`
- `HEDGEHOG_RUN_INVALID_MEMORY`

### 5. Python Wrapper

The Python package `qemu.hedgehog` provides:

- class: `Hedgehog`
- error type: `HedgehogError`
- constants namespace with `HEDGEHOG_*` names

The native wrapper path uses `ctypes` and expects a shared object exposing the
`hedgehog_backend_*` symbols.

You can point to that shared object via:

- `QEMU_HEDGEHOG_BACKEND_LIBRARY`

## Build Example

```bash
./configure --enable-hedgehog
make -j"$(nproc)"
```

### Shared Library Output

The build now emits a loadable Hedgehog backend library at:

- `build/libqemu-hedgehog-backend.so`

When `aarch64-softmmu` is configured, it also emits:

- `build/libqemu-hedgehog-backend-aarch64.so`

Target selection for this library is:

- `x86_64-softmmu` if that target is present
- otherwise the first available `*-softmmu` target

The `-aarch64` variant is built from `aarch64-softmmu` and is intended for
ARM64 CPU models such as `cortex-a57`.

Important:

- For x86 CPU models, use `build/libqemu-hedgehog-backend.so`.
- For AArch64 CPU models (for example `cortex-a57`), use
    `build/libqemu-hedgehog-backend-aarch64.so`.

Quick check:

```bash
find build -maxdepth 1 -name 'libqemu-hedgehog-backend*.so'
```

## Install the Python Module

Use a virtual environment and install the package from `python/`.

### Editable install (recommended for development)

```bash
cd python
python3 -m venv .venv
source .venv/bin/activate
pip install -e .
```

### Standard install

```bash
cd python
python3 -m venv .venv
source .venv/bin/activate
pip install .
```

### Verify installation

```bash
python -c "import qemu.hedgehog as h; print(h.Hedgehog.__name__)"
```

Notes:

- The package name is `qemu`, with the Hedgehog wrapper under `qemu.hedgehog`.
- The native wrapper expects a shared library path in
    `QEMU_HEDGEHOG_BACKEND_LIBRARY`.
- Wheel builds can bundle the backend library automatically by setting
    `QEMU_HEDGEHOG_BACKEND_BUILD_DIR` to a build directory containing
    `libqemu-hedgehog-backend*.so`.
- Bundled libraries are loaded automatically from `qemu/hedgehog/_native`
    before falling back to system linker lookup.
- If you are running from the source tree, use
    `QEMU_HEDGEHOG_BACKEND_LIBRARY=$PWD/build/libqemu-hedgehog-backend.so`.

## Python Usage Example

```python
import os

from qemu.hedgehog import (
    Hedgehog,
    HedgehogError,
    HEDGEHOG_ARCH_X86,
    HEDGEHOG_MODE_64,
    HEDGEHOG_HOOK_CODE,
)

# Point this to a shared library that exports hedgehog_backend_* symbols.
os.environ["QEMU_HEDGEHOG_BACKEND_LIBRARY"] = "/home/me/qemu/build/libqemu-hedgehog-backend.so"

BASE = 0x1000
CODE = bytes([
    0x90,  # nop
    0x90,  # nop
    0xF4,  # hlt
])


def on_code(emu, address, size, user_data):
    print(f"executing @ 0x{address:x}")
    return False  # return True to request stop


try:
    emu = Hedgehog(HEDGEHOG_ARCH_X86, HEDGEHOG_MODE_64, cpu_type="qemu64-x86_64-cpu")

    emu.mem_map(BASE, 0x1000)
    emu.mem_write(BASE, CODE)

    hook = emu.hook_add(
        HEDGEHOG_HOOK_CODE,
        on_code,
        begin=BASE,
        end=BASE + len(CODE) - 1,
    )

    emu.emu_start(BASE, 0, count=16)
    emu.hook_del(hook)
    emu.close()

except HedgehogError as err:
    print(f"Hedgehog failed: {err}")
```

## Python Machine Type Example

The Python API now accepts `machine_type` and forwards it to the native
backend creation path:

```python
import os

from qemu.hedgehog import (
    Hedgehog,
    HedgehogError,
    HEDGEHOG_ARCH_X86,
    HEDGEHOG_MODE_64,
)

os.environ["QEMU_HEDGEHOG_BACKEND_LIBRARY"] = "/home/me/qemu/build/libqemu-hedgehog-backend.so"

try:
    emu = Hedgehog(
        HEDGEHOG_ARCH_X86,
        HEDGEHOG_MODE_64,
        cpu_type="qemu64-x86_64-cpu",
        machine_type="none",  # explicit machine selection
    )

    emu.mem_map(0x1000, 0x1000)
    emu.close()

except HedgehogError as err:
    print(f"Hedgehog machine-type setup failed: {err}")
```

Raspberry Pi 3B machine type example:

```python
import os

from qemu.hedgehog import (
    Hedgehog,
    HedgehogError,
    HEDGEHOG_ARCH_ARM64,
    HEDGEHOG_MODE_ARM,
)

os.environ["QEMU_HEDGEHOG_BACKEND_LIBRARY"] = "/home/me/qemu/build/libqemu-hedgehog-backend-aarch64.so"

try:
    emu = Hedgehog(
        HEDGEHOG_ARCH_ARM64,
        HEDGEHOG_MODE_ARM,
        cpu_type="cortex-a53",
        machine_type="raspi3b",
    )

    # Run a short bounded execution window, then close.
    emu.emu_start(begin=0, until=0, count=16)
    emu.close()

except HedgehogError as err:
    print(f"Hedgehog raspi3b setup failed: {err}")
```

Current behavior:

- If `machine_type` is omitted, Hedgehog defaults to `none`.
- A process is locked to a single machine type after first Hedgehog
    initialization; attempting to switch machine type later in the same process
    returns an error.
- Board-backed machine types (for example `raspi3b`) run through machine
    realization and use the board-created CPU/memory model.
- In board-backed mode, `mem_map` and `mem_map_mmio` are not supported.

## AArch64 Python Usage Example

```python
import os

from qemu.hedgehog import (
    Hedgehog,
    HedgehogError,
    HEDGEHOG_ARCH_ARM64,
    HEDGEHOG_MODE_ARM,
    HEDGEHOG_HOOK_CODE,
)

# Point this to a shared library that exports hedgehog_backend_* symbols.
os.environ["QEMU_HEDGEHOG_BACKEND_LIBRARY"] = "/home/me/qemu/build/libqemu-hedgehog-backend-aarch64.so"

BASE = 0x400000

# AArch64 NOP is 0xD503201F (little-endian bytes shown below).
CODE = bytes([
    0x1F, 0x20, 0x03, 0xD5,
    0x1F, 0x20, 0x03, 0xD5,
    0x1F, 0x20, 0x03, 0xD5,
    0x1F, 0x20, 0x03, 0xD5,
])


def on_code(emu, address, size, user_data):
    print(f"AArch64 executing @ 0x{address:x}")
    return False


try:
    # For non-x86 architectures, pass an explicit CPU type.
    # If "cortex-a57" is unavailable in your build, choose another AArch64 CPU.
    emu = Hedgehog(HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM, cpu_type="cortex-a57")

    emu.mem_map(BASE, 0x1000)
    emu.mem_write(BASE, CODE)

    hook = emu.hook_add(
        HEDGEHOG_HOOK_CODE,
        on_code,
        begin=BASE,
        end=BASE + len(CODE) - 1,
    )

    # Run exactly 4 instructions (budgeted run).
    emu.emu_start(BASE, 0, count=4)
    emu.hook_del(hook)
    emu.close()

except HedgehogError as err:
    print(f"Hedgehog AArch64 failed: {err}")
```

## Notes

- Register IDs for `reg_read`/`reg_write` follow target-specific GDB register
  numbering in the current implementation.
- The Python wrapper currently focuses on low-level parity with the in-tree C
  backend API and a subset of hook families.
- Current limitation: repeated create/close cycles within the same Python
    process are not yet supported reliably (`tcg_register_thread` assertion may
    occur on the second instance). Prefer one Hedgehog lifecycle per process.

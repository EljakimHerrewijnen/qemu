# Hedgehog Quick Start (Using QEMU from Your Python Project)

This guide shows how to use QEMU Hedgehog from a separate Python project.

## 1. Prerequisites

You need:

- A QEMU source checkout with Hedgehog enabled
- Python 3.10+ (venv recommended)
- A target backend library built for your architecture

## 2. Build QEMU Hedgehog Backend Libraries

From your QEMU source root:

```bash
mkdir -p build-hedgehog
./configure --enable-hedgehog --target-list=x86_64-softmmu,aarch64-softmmu
ninja -C build-hedgehog libqemu-hedgehog-backend.so libqemu-hedgehog-backend-aarch64.so
```

Verify output:

```bash
find build-hedgehog -maxdepth 1 -name 'libqemu-hedgehog-backend*.so'
```

Typical outputs:

- `build-hedgehog/libqemu-hedgehog-backend.so` (x86)
- `build-hedgehog/libqemu-hedgehog-backend-aarch64.so` (ARM64)

## 3. Install the Python Package in Your External Project

In your external project folder:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -U pip
pip install /absolute/path/to/qemu/python
```

For active QEMU development, editable install is convenient:

```bash
pip install -e /absolute/path/to/qemu/python
```

## 3a. Install Directly from a GitHub Release Wheel

For consumer installs (no local QEMU build), install directly from release assets.

Linux x86_64:

```bash
pip install \
  https://github.com/<owner>/<repo>/releases/download/v0.6.1a1/qemu-0.6.1a1-<python-tag>-<abi-tag>-manylinux_<glibc>_x86_64.whl
```

Linux aarch64:

```bash
pip install \
  https://github.com/<owner>/<repo>/releases/download/v0.6.1a1/qemu-0.6.1a1-<python-tag>-<abi-tag>-manylinux_<glibc>_aarch64.whl
```

Tip: open the release page first and copy the exact wheel filename for your
Python/ABI tag.

## 4. Minimal x86 Script

Create `run_hedgehog_x86.py`:

```python
import os

from qemu.hedgehog import (
    Hedgehog,
    HEDGEHOG_ARCH_X86,
    HEDGEHOG_MODE_64,
)

# Point to the x86 backend library built from your QEMU tree.
os.environ["QEMU_HEDGEHOG_BACKEND_LIBRARY"] = \
    "/absolute/path/to/qemu/build-hedgehog/libqemu-hedgehog-backend.so"

BASE = 0x1000
CODE = bytes([
    0x90,  # nop
    0x90,  # nop
    0xF4,  # hlt
])

with Hedgehog(HEDGEHOG_ARCH_X86, HEDGEHOG_MODE_64,
              cpu_type="qemu64-x86_64-cpu") as emu:
    emu.mem_map(BASE, 0x1000)
    emu.mem_write(BASE, CODE)
    emu.emu_start(begin=BASE, until=0, count=16)

print("x86 run complete")
```

Run:

```bash
python run_hedgehog_x86.py
```

## 5. Minimal raspi3b Board-Backed Script (ARM64)

Create `run_hedgehog_raspi3b.py`:

```python
import os

from qemu.hedgehog import (
    Hedgehog,
    HEDGEHOG_ARCH_ARM64,
    HEDGEHOG_MODE_ARM,
)

# Use the ARM64 backend library.
os.environ["QEMU_HEDGEHOG_BACKEND_LIBRARY"] = \
    "/absolute/path/to/qemu/build-hedgehog/libqemu-hedgehog-backend-aarch64.so"

with Hedgehog(HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM,
              cpu_type="cortex-a53", machine_type="raspi3b") as emu:
    emu.emu_start(begin=0, until=0, count=16)

print("raspi3b run complete")
```

Run:

```bash
python run_hedgehog_raspi3b.py
```

Notes for board-backed mode:

- `machine_type` selects a real board model.
- `mem_map` and `mem_map_mmio` are not supported in board-backed mode.
- For now, treat board-backed usage as single-active-instance per process.

## 6. Optional: Build a Wheel That Bundles Backend Libraries

If you want to ship one wheel with native Hedgehog libs included:

```bash
cd /absolute/path/to/qemu/python
QEMU_HEDGEHOG_BACKEND_BUILD_DIR=/absolute/path/to/qemu/build-hedgehog \
  QEMU_HEDGEHOG_REQUIRE_NATIVE=1 \
  pip wheel . -w dist
```

Then install in your project:

```bash
pip install /absolute/path/to/qemu/python/dist/*.whl
```

The package auto-discovers bundled libs under `qemu/hedgehog/_native`.

## 7. Troubleshooting

- `ImportError: No module named qemu.hedgehog`
  - Install from `/path/to/qemu/python` into your active venv.
- `failed to create backend for cpu type ...`
  - Usually wrong backend `.so` for the CPU architecture.
  - Example: AArch64 CPU models need `libqemu-hedgehog-backend-aarch64.so`.
- `unknown cpu type ...`
  - Use a valid QEMU CPU model for that target.
- Machine type switch errors in one process
  - Create a fresh process if changing board machine types.

## 8. Recommended Next Step

Once this works, wrap Hedgehog creation into a helper in your project so only
one module owns:

- backend library selection
- machine/cpu defaults
- error handling and retries

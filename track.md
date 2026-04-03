# Hedgehog Backend Progress Tracker

This file tracks implementation status for docs/devel/hedgehog-backend.rst and
provides concrete pointers for future LLM-assisted phases.

## Current Status

- Phase 1: implemented
  - Optional build feature (`--enable-hedgehog` / meson `hedgehog` option)
  - Backend bootstrap and lifecycle API
  - CPU creation, RAM mapping, MMIO mapping entry point, run/stop API

- Phase 2: implemented
  - Added a callback-backed MMIO base device (`TYPE_HEDGEHOG_MMIO_DEVICE`)
  - Backend MMIO mapping now instantiates and realizes that device
  - MMIO callbacks are forwarded through standard `MemoryRegionOps`
  - Backend teardown now explicitly unmaps and destroys MMIO/RAM mappings

- Phase 3: implemented
  - Added backend execution hook API for translation-block and instruction
    callbacks (`hedgehog_backend_set_tb_hook`,
    `hedgehog_backend_set_insn_hook`)
  - Added a narrow TCG integration path in `accel/tcg/cpu-exec.c` for TB-enter
    callbacks and single-step instruction callbacks
  - Added an internal CPU-to-backend hook registry for dispatch and stop
    propagation without target-specific translator edits

- Phase 6: implemented (initial)
  - Added a new Python package under `python/qemu/hedgehog/`
  - Added Hedgehog-style constants and exception shims (`HedgehogError`, errno values)
  - Added a Hedgehog-compatible `Hedgehog` API surface with:
    - emulator construction
    - RAM/MMIO mapping
    - memory read/write
    - register read/write
    - bounded execution (`emu_start`/`emu_stop`)
    - hook registration (`hook_add`/`hook_del`) for block/code/invalid-memory
  - Added an optional ctypes-based native backend loader for the in-tree C API
    through `QEMU_HEDGEHOG_BACKEND_LIBRARY`
  - Added Python unit tests for the compatibility wrapper using a fake backend

## Recent Fixes (2026-04-03)

- Added CPU model alias fallback in backend CPU class resolution so model names
  such as `cortex-a57` work when available for the selected target.
- Added AArch64-targeted backend shared library output:
  - `build/libqemu-hedgehog-backend-aarch64.so`
- Fixed backend teardown sequencing to unrealize and release CPU/MMIO devices
  before final address-space/object cleanup.
- Corrected usage docs so architecture-specific examples select the right
  backend library path.

## Files Changed For Phase 2

- accel/hedgehog/hedgehog-mmio-device.c
- accel/hedgehog/hedgehog-mmio-device.h
- accel/hedgehog/hedgehog.c
- accel/hedgehog/meson.build

## Files Added/Changed For Phase 3

- accel/tcg/hedgehog-exec-hooks.c
- accel/tcg/cpu-exec.c
- accel/tcg/meson.build
- include/system/hedgehog-exec-hooks.h
- include/system/hedgehog-backend.h
- accel/hedgehog/hedgehog.c
- tests/unit/test-hedgehog-backend-api.c

## Files Added/Changed For Phase 6

- python/setup.cfg
- python/qemu/hedgehog/__init__.py
- python/qemu/hedgehog/api.py
- python/qemu/hedgehog/backend.py
- python/qemu/hedgehog/constants.py
- python/qemu/hedgehog/errors.py
- python/qemu/hedgehog/README.rst
- python/qemu/hedgehog/py.typed
- python/tests/test_hedgehog_api.py

## Where Future LLMs Should Look Next

- Phase 3 (TCG execution hooks)
  - Status: done for initial TB/instruction dispatch
  - Refinements still possible:
    - instruction callback behavior for non-single-step execution
    - richer callback metadata (size/end address/flags)
    - callback filtering by address range

- Phase 4 (invalid memory and exception hooks)
  - Status: done for initial cputlb callback path
  - Refinements still possible:
    - capture additional MMU fault metadata (translation stage / permission bits)
    - add explicit exception-class mapping table for backend-visible status
    - extend callbacks to include faulting physical address when available

- Phase 5: implemented (initial)
  - Added architecture-polish register helpers on top of existing CPUClass
    GDB register callbacks:
    - `hedgehog_backend_reg_read()`
    - `hedgehog_backend_reg_write()`
  - These provide a target-agnostic register access surface while still using
    each target's existing register encoding and semantics.
  - Added unit coverage for expanded API constants/types and signature sanity
    checks.

- API surface and tests to extend
  - include/system/hedgehog-backend.h
  - tests/unit/test-hedgehog-backend-api.c
  - python/qemu/hedgehog/*
  - python/tests/test_hedgehog_api.py
  - Add runtime integration coverage with a real shared backend library

## Open Follow-Ups

- Add backend-visible fault/exception translation coverage tests
- Investigate multi-instance lifecycle support in one process
  (current limitation: second create/close may hit `tcg_register_thread` assert)
- Keep target-specific changes out of `target/*` unless absolutely required
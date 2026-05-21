# Hedgehog Feature Research: Memory Hook + Unmap Parity

## Problem Statement

The requested phase-4 Hedgehog parity items from `Chokmah/plans/hedgehog_features_plan.md` are not fully present in this QEMU fork.

Missing items in current qemu.hedgehog stack:
- `mem_unmap(addr, size)` public API and native backend support.
- Memory read hook registration with begin/end filters.
- Memory write hook registration with begin/end filters.
- Hook lifecycle parity for these hook families (`hook_add` + `hook_del`).

## Touched Subsystems / Files

Likely touch points:
- `include/system/hedgehog-backend.h`
- `accel/hedgehog/hedgehog.c`
- `include/system/hedgehog-exec-hooks.h`
- `accel/tcg/hedgehog-exec-hooks.c`
- `accel/tcg/cpu-exec.c` and/or `accel/tcg/cputlb.c` (if full guest-access callbacks are needed)
- `python/qemu/hedgehog/backend.py`
- `python/qemu/hedgehog/api.py`
- `python/qemu/hedgehog/docs.md`

## Current Control/Data Flow Notes

- Python hook registration in `api.py` currently supports:
  - exec block/code hooks
  - invalid memory hooks only
- Native backend (`backend.py`) exposes C callbacks for:
  - TB hook
  - instruction hook
  - invalid memory hook
- C hook registry (`hedgehog-exec-hooks.c`) stores/dispatched hooks for:
  - TB
  - instruction
  - invalid memory
- No backend unmap call exists in C API.

## Alternatives and Tradeoffs

1. Full Unicorn-like guest memory read/write hook parity in TCG (all guest accesses)
- Pros: closest semantic parity.
- Cons: invasive changes in hot TCG paths; higher merge-risk against upstream.

2. Minimal parity using API-level read/write observation hooks (on `mem_read`/`mem_write` calls) plus `mem_unmap`
- Pros: low-risk, additive, quick to land, keeps merge-friendliness.
- Cons: does not observe every guest CPU memory access.

Given upstream-sync goals and requested merge-safe workflow, option 2 is preferred for this phase.

## Merge-Risk Assessment

- Low risk: Python API/backend additions and C `mem_unmap` API.
- Medium risk: C region lifecycle tracking changes in `accel/hedgehog/hedgehog.c`.
- High risk avoided in this phase: deep TCG instrumentation for full guest memory hooks.

## Test Strategy Draft

- Build verification:
  - configure + ninja hedgehog libs.
- Python smoke:
  - map, write, read, unmap, verify access failure after unmap.
- Hook lifecycle:
  - register read/write hooks with address filters.
  - verify callbacks run for API `mem_read` / `mem_write` operations in deterministic registration order.
  - delete hook and verify no callback.

## Assumptions

- For this phase, `hook_mem_read`/`hook_mem_write` parity means API-visible hook family and lifecycle compatibility, not full TCG-wide guest memory trace parity.
- If strict full Unicorn guest-access parity is required, a separate TCG-focused phase should be planned.

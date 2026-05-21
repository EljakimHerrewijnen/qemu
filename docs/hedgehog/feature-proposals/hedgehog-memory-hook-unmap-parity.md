# Proposal: Hedgehog Memory Hook + Unmap Parity (Merge-Safe)

## Scope

Implement a merge-safe parity slice for:
- `mem_unmap(addr, size)` support.
- `hook_mem_read(begin, end, hook)` and `hook_mem_write(begin, end, hook)` behavior through existing `hook_add`/`hook_del` API surface.
- Deterministic callback ordering and address-range filtering.

## Non-Goals

- Full TCG instrumentation for every guest memory access event.
- Bridge/FUSE/link work from Chokmah plan (outside this repository).

## Files and Changes

1. `include/system/hedgehog-backend.h`
- Add `hedgehog_backend_mem_unmap(...)` declaration.

2. `accel/hedgehog/hedgehog.c`
- Add `hedgehog_backend_mem_unmap(...)` implementation for RAM/MMIO mappings.
- Update region bookkeeping to remove entries on unmap and keep authoritative mapping state.

3. `python/qemu/hedgehog/backend.py`
- Extend `BackendProtocol` with `unmap(...)`.
- Implement `NativeBackend.unmap(...)` and wire ctypes symbol configuration.

4. `python/qemu/hedgehog/api.py`
- Add `mem_unmap(...)` public method.
- Extend supported hook mask to include `HEDGEHOG_HOOK_MEM_READ` and `HEDGEHOG_HOOK_MEM_WRITE`.
- Dispatch mem-read/mem-write callbacks on `mem_read`/`mem_write` API operations with begin/end filtering and deterministic registration order.

5. `python/qemu/hedgehog/docs.md`
- Document `mem_unmap` and clarify mem read/write hook semantics for this phase.

## Patch Set Boundaries

### Patch Set 1: C backend unmap API + authoritative region updates
- Header declaration + C implementation.
- Acceptance: mapped region can be unmapped and subsequent accesses fail as expected.

### Patch Set 2: Python backend bridge for unmap
- Protocol + NativeBackend + ctypes signatures.
- Acceptance: Python can invoke unmap successfully.

### Patch Set 3: Python API mem hook parity surface
- Hook mask expansion + read/write callback dispatch.
- Acceptance: callbacks fire in deterministic order; `hook_del` stops callbacks.

### Patch Set 4: Docs
- API docs and notes.

## Invariants to Preserve

- Existing TB/code/invalid-memory hooks remain behaviorally unchanged.
- Existing coverage fallback behavior remains unchanged.
- Hook callback exception handling remains consistent (`HEDGEHOG_ERR_EXCEPTION` wrapping).

## Verification Commands

- `./configure --enable-hedgehog --target-list=x86_64-softmmu,aarch64-softmmu`
- `ninja -C build-hedgehog libqemu-hedgehog-backend.so libqemu-hedgehog-backend-aarch64.so`
- Python smoke script for map/read/write/unmap + hooks.

## Rollback Path

- Revert patch sets independently in reverse order.
- If unmap logic causes instability, keep API stubs disabled while preserving additive declarations for follow-up fixes.

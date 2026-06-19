# Proposal: Hedgehog Backend CPU Diagnostics and Arch Fallback (Merge-Safe)

## 1) Scope and Non-Goals

### Scope
- Improve backend creation diagnostics for CPU/model selection failures (example: `cpu_type='cortex-a9'`) in the Python Hedgehog bridge.
- Stop discarding C-side error objects by passing a writable `Error **` instead of `NULL` in backend creation/initialization call paths.
- Add a bounded, deterministic architecture-aware fallback attempt when initial backend creation fails due to likely library/CPU mismatch.
- Document new behavior and troubleshooting guidance in Python API docs.

### Non-Goals
- No changes to C backend ABI or signatures in `include/system/hedgehog-backend.h`.
- No behavioral changes to run loop, hooks, memory mapping, coverage, or machine realization semantics.
- No broad refactor of library discovery logic beyond narrowly scoped fallback ordering/retry behavior.
- No changes to upstream hot files in `accel/tcg/*`.

## 2) File-by-File Edits

### `python/qemu/hedgehog/backend.py`

Planned edits (single file, additive helpers + narrow call-site rewiring):
- Add a tiny internal error-capture helper for C calls that accept `Error **`:
  - Allocate local `ctypes.c_void_p` and pass `ctypes.byref(...)` as `Error **`.
  - Convert resulting error object to text (best effort) using exported error utilities if available.
  - Always free captured error objects when owned by Python-side caller.
  - Return tuple-like result to callers: `(ok_or_value, diagnostic_text_or_none)`.
- Apply helper to these creation-time call sites first (highest user impact):
  - `hedgehog_backend_initialize(...)`
  - `hedgehog_backend_initialize_for_machine(...)`
  - `hedgehog_backend_new(...)`
  - `hedgehog_backend_new_with_machine(...)`
- Keep existing `HedgehogError(errno=HEDGEHOG_ERR_RESOURCE, ...)` contract, but upgrade message detail to include:
  - failing API (`new`, `new_with_machine`, `initialize*`)
  - requested `cpu_type` and optional `machine_type`
  - selected library path/candidate basename when known
  - captured C-side detail string when available
  - fallback attempts summary (attempted/skipped/succeeded/failed)
- Add architecture-aware fallback strategy for backend creation:
  - Trigger only in `NativeBackend.create(...)` when first creation attempt fails.
  - Retry against a small ordered candidate set (for example: explicitly requested path first, env path, packaged candidates, linker candidates) without changing global behavior elsewhere.
  - Guard retry count (max 1 extra candidate by default) to avoid slow nondeterministic probing.
  - Use conservative heuristics for mismatch hinting (for example, ARM/AArch64 CPU names) only to improve candidate ordering and diagnostics, not to silently mask unrelated failures.
- Preserve existing API surface and return types; no new public classes or exported symbols.

### `python/qemu/hedgehog/docs.md`

Planned edits (documentation-only, localized additions):
- Add a short subsection under constructor/runtime troubleshooting that explains:
  - backend creation now surfaces C-side details when available
  - how architecture/library mismatch is diagnosed
  - what fallback is attempted and its limits
  - how to pin library selection explicitly with `QEMU_HEDGEHOG_BACKEND_LIBRARY`
- Add concrete troubleshooting examples for failures like `cpu_type='cortex-a9'`:
  - expected enriched error format
  - next-step checks (CPU model support in selected library, backend library variant, machine_type compatibility)
- Clarify that fallback is best-effort and does not alter successful existing flows.

## 3) Patch Boundaries Minimizing Conflicts

### Patch Set 1: Python Error-Capture Plumbing for Create Path
Files:
- `python/qemu/hedgehog/backend.py`

Changes:
- Introduce private helpers for `Error **` capture/cleanup and diagnostics formatting.
- Rewire only creation/initialization call sites to use helper.

Why merge-safe:
- Localized to Python shim.
- No C ABI or cross-directory changes.
- No edits in high-churn upstream core execution paths.

### Patch Set 2: Arch-Aware Fallback Attempt + Structured Failure Reporting
Files:
- `python/qemu/hedgehog/backend.py`

Changes:
- Add bounded retry path on backend creation failure.
- Add deterministic attempt metadata in final raised `HedgehogError` message.

Why merge-safe:
- Single-purpose change in same file as patch set 1.
- Additive control flow around existing creation calls.
- Easy to isolate/revert independently if retry behavior is undesirable.

### Patch Set 3: API Documentation Update
Files:
- `python/qemu/hedgehog/docs.md`

Changes:
- Document enriched diagnostics, fallback behavior, and troubleshooting.

Why merge-safe:
- Docs-only patch, no runtime risk.

## 4) Invariants to Preserve

- `NativeBackend.create(...)` success path remains functionally identical for currently working CPU/library combinations.
- Existing exception type and errno contract remains: still `HedgehogError` with `HEDGEHOG_ERR_RESOURCE` on resource/creation failure.
- Existing library resolution precedence remains stable for first attempt; fallback is explicit, bounded, and diagnostic-driven.
- No changes to method signatures in `BackendProtocol` or `NativeBackend` public call sites.
- No changes to machine-backed vs board-backed mode semantics.
- No changes to hook, memory, register, run-loop, or chardev behavior outside creation diagnostics.

## 5) Acceptance Criteria

### Patch Set 1 Acceptance
Build condition:
- Python package imports cleanly after edit (`qemu.hedgehog` import smoke).

Runtime condition:
- Forced creation failure (invalid CPU type or intentionally incompatible combo) raises `HedgehogError` containing API-stage context and captured C-side detail when available.

API condition:
- `NativeBackend.create(...)` signature and return type unchanged.

Failure signals and triage hints:
- If message still only shows generic `failed to create backend...`, verify `Error **` plumbing is used at all create/initialize call sites.
- If crashes or invalid pointers appear, verify error object ownership and free path.

### Patch Set 2 Acceptance
Build condition:
- No static/type errors in edited Python module.

Runtime condition:
- For mismatch-like scenario (example `cortex-a9` with incompatible selected library), failure message includes retry summary and candidate info.
- For already-working setup, no behavior regression and no extra fallback attempt on success.

API condition:
- Raised exception class/errno unchanged; only message detail expanded.

Failure signals and triage hints:
- If startup latency increases materially, ensure retry count is bounded to configured maximum.
- If fallback causes confusing silent changes, ensure message always reports chosen candidate and attempt order.

### Patch Set 3 Acceptance
Build condition:
- Markdown renders cleanly and keeps existing docs structure.

Runtime condition:
- Documentation examples map to actual raised message format from patch sets 1-2.

API condition:
- No code changes in this patch set.

Failure signals and triage hints:
- If users still cannot diagnose failure, docs must include explicit env-var override and candidate verification steps.

## Compatibility / Merge Risk Notes

- Risk level: Low.
- Reasons:
  - Python-only functional changes, docs-only follow-up.
  - No change to upstream C interfaces or accelerator internals.
  - Patch sets are narrow and independently revertible.
- Primary conflict surface:
  - Concurrent edits in `python/qemu/hedgehog/backend.py` around `NativeBackend.create(...)` and library loading helpers.
- Conflict mitigation:
  - Keep helper names private and cohesive.
  - Keep fallback logic adjacent to creation flow, not spread through unrelated backend methods.

## Validation Commands

From repository root:

```bash
python3 -m venv /tmp/hh-proposal-venv
source /tmp/hh-proposal-venv/bin/activate
pip install -e python
python -c "from qemu.hedgehog import Hedgehog; print('import-ok')"
```

Suggested runtime smoke (diagnostic failure path):

```bash
QEMU_HEDGEHOG_BACKEND_LIBRARY=/path/to/libqemu-hedgehog-backend.so \
python - <<'PY'
from qemu.hedgehog import Hedgehog
from qemu.hedgehog.constants import HEDGEHOG_ARCH_ARM, HEDGEHOG_MODE_ARM

try:
    Hedgehog(HEDGEHOG_ARCH_ARM, HEDGEHOG_MODE_ARM, cpu_type='cortex-a9')
except Exception as e:
    print(type(e).__name__)
    print(str(e))
PY
```

If backend libraries need rebuilding for local validation:

```bash
./configure --enable-hedgehog --target-list=x86_64-softmmu,aarch64-softmmu
ninja -C build-hedgehog libqemu-hedgehog-backend.so libqemu-hedgehog-backend-aarch64.so
```

## 6) Rollback Strategy

- Revert in reverse patch-set order:
  1. Revert docs update (`python/qemu/hedgehog/docs.md`).
  2. Revert fallback/retry logic in `python/qemu/hedgehog/backend.py`.
  3. Revert `Error **` capture helpers/call-site rewiring in `python/qemu/hedgehog/backend.py`.
- Safe rollback invariant:
  - Restores previous generic error behavior without affecting successful backend creation flows.
- Emergency mitigation option:
  - Keep diagnostics helper but disable fallback retry behind a local constant/flag if retry behavior is implicated.

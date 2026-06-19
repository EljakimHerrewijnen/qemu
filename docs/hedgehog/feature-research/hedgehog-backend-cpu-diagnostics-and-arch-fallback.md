# Feature Research: Hedgehog backend CPU diagnostics and architecture fallback

## Problem statement

A downstream consumer (BrotherPrinter) fails to create a Hedgehog backend with `cpu_type="cortex-a9"` and receives only:

- `HedgehogError: failed to create backend for cpu type cortex-a9 (errno=20)`

The backend likely returns a precise reason (for example unknown CPU type for current target), but the Python binding currently drops this detail by always passing `Error **errp = NULL` to C APIs.

## Touched subsystems/files

- `python/qemu/hedgehog/backend.py`
- `python/qemu/hedgehog/docs.md`
- (potentially) `python/qemu/hedgehog/api.py` if API-level messaging needs adjustment

No C ABI changes are required for the minimal fix.

## Control/data flow notes

1. `Hedgehog.__init__()` calls `NativeBackend.create(...)`.
2. `NativeBackend.create(...)` initializes backend and calls:
   - `hedgehog_backend_new_with_machine(...)` or
   - `hedgehog_backend_new(...)`
3. Python currently passes `None` for `Error **errp` in all these calls.
4. On failure, C computes a detailed `Error` string (for example in `hedgehog_backend_lookup_cpu_class()`), but Python only raises a generic `HedgehogError(HEDGEHOG_ERR_RESOURCE, ...)`.

## Alternatives and tradeoffs

1. Python-side Error bridge (preferred)
- Pass a real `Error **` pointer from ctypes.
- Extract detail via `error_get_pretty()` and free with `error_free()`.
- Pros: minimal and low-risk, no ABI changes, immediate diagnostics improvement.
- Cons: depends on symbols being available in loaded library.

2. Add new C helper APIs returning last error string
- Pros: explicit stable API for bindings.
- Cons: larger C surface and maintenance overhead.

3. Add CPU alias mapping in Python (`cortex-a9` -> `cortex-a9-arm-cpu`)
- Pros: may mask some user misconfiguration.
- Cons: does not solve mismatched target library root issue, can hide real architecture mismatch.

## Merge-risk assessment

- Low risk for Python-only Error bridge updates.
- Moderate risk for behavioral fallback changes in library selection order.
- No expected impact on non-hedgehog code paths.

Potential conflict hotspots:
- `python/qemu/hedgehog/backend.py` around `NativeBackend.create()` and `_configure_library_api()`.

## Test strategy draft

1. Unit-ish smoke in Python:
- Force backend creation failure with invalid cpu type and verify raised message includes backend detail text.

2. Architecture mismatch smoke:
- Use x86 backend library with ARM cpu string and confirm error message surfaces target mismatch details.

3. Regression smoke:
- Existing x86 default backend creation remains unchanged.
- Existing machine-backed creation path still works when valid.

## Assumptions

- `error_get_pretty()` and `error_free()` are available in the loaded backend shared object dependencies.
- Downstream users benefit primarily from diagnosability; auto-retry/fallback across library variants is optional and should remain conservative.

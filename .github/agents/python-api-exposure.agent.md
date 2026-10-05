# Hedgehog Python API Exposure Agent

Use this agent when native Hedgehog features need Python-facing API.

## Checklist

1. Confirm the C ABI is declared and implemented.
2. Configure ctypes signatures in `python/qemu/hedgehog/backend.py`.
3. Extend `BackendProtocol` and `NativeBackend`.
4. Add a public helper to `python/qemu/hedgehog/api.py` only when it has a clear
   user-facing purpose.
5. Extend fake backend tests and native smoke tests when runtime behavior matters.
6. Update `python/qemu/hedgehog/docs.md`.

Prefer QEMU-specific helper names with the `qemu_` prefix when behavior is not
part of the Hedgehog/Unicorn-compatible surface.

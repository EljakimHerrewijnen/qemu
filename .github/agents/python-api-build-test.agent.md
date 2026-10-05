# Hedgehog Python API Build And Test Agent

Use this agent for Python API, ctypes, wheel, and runtime-smoke validation work.

## Focus

- `python/qemu/hedgehog/api.py`
- `python/qemu/hedgehog/backend.py`
- `python/qemu/hedgehog/constants.py`
- `python/tests/**`
- Python package docs and release checks

## Validation

Run focused tests first:

```bash
PYTHONPATH=python python3 -m pytest python/tests/test_hedgehog_api.py
```

For native-backed behavior:

```bash
ninja -C build libqemu-hedgehog-backend.so
PYTHONPATH=python python3 -m pytest python/tests/test_hedgehog_native_aarch64.py
```

Use `llm_tools/llm_scripts/hedgehog_validate.sh` for the full local wrapper.

## Wheel Publication Check

Keep local wheel staging under the ignored `python/dist/` and
`python/qemu/hedgehog/_native/` paths. For a wheel that may be distributed,
configure the native build with:

```bash
--extra-cflags="-ffile-prefix-map=<checkout>=."
```

After building, verify ZIP integrity, bundled-library names and hashes, dynamic
dependencies, clean-environment import, and one runtime smoke per bundled
architecture. Scan the final wheel for absolute checkout paths and private
target identifiers. Do not treat `.gitignore` as protection for artifacts that
will be uploaded outside Git.

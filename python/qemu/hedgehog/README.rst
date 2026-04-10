QEMU Hedgehog Compatibility Layer
================================

This package provides a Hedgehog-like Python API on top of QEMU's
in-tree Hedgehog backend C API.

Current goals:

- provide Hedgehog-compatible constants and exceptions;
- expose a familiar ``Hedgehog`` object with map/read/write/register/hook APIs;
- keep a low-level path close to the backend C surface;
- allow incremental porting from Hedgehog Python code.

For the full Python API reference and usage notes in Markdown form, see
``docs.md`` in this package directory.

Status notes:

- the default backend implementation uses ctypes and requires a shared
  library exposing the in-tree backend symbols;
- set ``QEMU_HEDGEHOG_BACKEND_LIBRARY`` to the shared object path;
- when building/installing wheels from this source tree, you can set
  ``QEMU_HEDGEHOG_BACKEND_BUILD_DIR`` so matching
  ``libqemu-hedgehog-backend*`` libraries are bundled into the wheel under
  ``qemu/hedgehog/_native``;
- bundled libraries are auto-discovered at runtime before falling back to the
  system linker path;
- after building QEMU with ``--enable-hedgehog``, the in-tree path is typically
  ``build/libqemu-hedgehog-backend.so``;
- if ``aarch64-softmmu`` is configured, an ARM64-targeted backend is also
  emitted at ``build/libqemu-hedgehog-backend-aarch64.so``;
- machine-backed configurations can pre-create named chardevs, bind them to
  device properties via ``property_bindings``, and query endpoints such as PTY
  paths through ``qemu_chardev_get_endpoint()``;
- host-connected backends are serviced explicitly with ``qemu_events_poll()``,
  which is useful when a guest is waiting on UART or similar device input;
- only a subset of Hedgehog hooks is currently implemented.

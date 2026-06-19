"""
Low-level backend interface for qemu.hedgehog.

This module provides:
- a typed backend protocol used by the Hedgehog-compatible wrapper;
- a ctypes-based backend implementation for the in-tree C API.
"""

# Copyright (C) 2026 Red Hat Inc.
#
# This work is licensed under the terms of the GNU GPL, version 2.  See
# the COPYING file in the top-level directory.

from __future__ import annotations

import ctypes
import ctypes.util
import glob
import os
import threading
from typing import Any, Callable, Dict, List, Optional, Protocol, Tuple, cast
from typing import runtime_checkable

from .constants import HEDGEHOG_ERR_ARG, HEDGEHOG_ERR_RESOURCE
from .errors import HedgehogError

ExecHookCallback = Callable[[int], bool]
InvalidHookCallback = Callable[[int, int, int, int], bool]
MMIOReadCallback = Callable[[int, int], int]
MMIOWriteCallback = Callable[[int, int, int], None]


_EXEC_HOOK_BRIDGE = ctypes.CFUNCTYPE(
    ctypes.c_bool,
    ctypes.c_void_p,
    ctypes.c_uint64,
    ctypes.c_void_p,
)

_INVALID_HOOK_BRIDGE = ctypes.CFUNCTYPE(
    ctypes.c_bool,
    ctypes.c_void_p,
    ctypes.c_uint64,
    ctypes.c_uint,
    ctypes.c_uint,
    ctypes.c_uint,
    ctypes.c_void_p,
)

_MMIO_READ_BRIDGE = ctypes.CFUNCTYPE(
    ctypes.c_uint64,
    ctypes.c_void_p,
    ctypes.c_uint64,
    ctypes.c_uint,
)

_MMIO_WRITE_BRIDGE = ctypes.CFUNCTYPE(
    None,
    ctypes.c_void_p,
    ctypes.c_uint64,
    ctypes.c_uint64,
    ctypes.c_uint,
)


_NATIVE_BACKEND_PROCESS_LOCK = threading.Lock()
_NATIVE_BACKEND_PROCESS_SINGLETON: Optional['NativeBackend'] = None

_NATIVE_BACKEND_SINGLETON_ERROR = (
    'qemu.hedgehog native backends can only be initialized once per process; '
    'QEMU TCG teardown is not currently safe for repeated create/close cycles. '
    'Run each native Hedgehog emulator in a separate process.'
)

_NATIVE_BACKEND_CLOSED_ERROR = (
    'this qemu.hedgehog backend has been closed and cannot be reused'
)


@runtime_checkable
class BackendProtocol(Protocol):
    """
    Common backend protocol consumed by the Hedgehog compatibility wrapper.
    """

    def close(self) -> None:
        """Release backend resources."""
        ...

    def map_ram(self, name: str, addr: int, size: int) -> bool:
        """Map RAM in guest address space."""
        ...

    def map_mmio(
        self,
        name: str,
        addr: int,
        size: int,
        read_fn: MMIOReadCallback,
        write_fn: MMIOWriteCallback,
    ) -> bool:
        """Map MMIO callbacks in guest address space."""
        ...

    def mem_read(self, addr: int, size: int) -> Tuple[int, bytes]:
        """Read guest memory, returning (MemTxResult, data)."""
        ...

    def mem_write(self, addr: int, data: bytes) -> int:
        """Write guest memory, returning MemTxResult."""
        ...

    def unmap(self, addr: int, size: int) -> bool:
        """Unmap guest memory region, returning success."""
        ...

    def reg_read(self, regno: int, buf_size: int) -> Optional[bytes]:
        """Read register bytes, or None on failure."""
        ...

    def reg_write(self, regno: int, data: bytes) -> bool:
        """Write register bytes, returning success."""
        ...

    def set_tb_hook(self, callback: Optional[ExecHookCallback]) -> None:
        """Set or clear translation-block callback."""
        ...

    def set_insn_hook(self, callback: Optional[ExecHookCallback]) -> None:
        """Set or clear instruction callback."""
        ...

    def set_invalid_mem_hook(
        self,
        callback: Optional[InvalidHookCallback],
    ) -> None:
        """Set or clear invalid-memory callback."""
        ...

    def reset(self) -> None:
        """Reset CPU state."""
        ...

    def set_pc(self, addr: int) -> None:
        """Set guest PC."""
        ...

    def get_pc(self) -> int:
        """Get guest PC."""
        ...

    def run(self, max_instructions: int) -> Tuple[int, int]:
        """Run backend, returning (run_result, cpu_exit)."""
        ...

    def stop(self) -> None:
        """Request stop for the current run."""
        ...

    def add_chardev(self, chardev_id: str, uri: str) -> bool:
        """Create a named host chardev backend."""
        ...

    def bind_property(self, object_path: str, property_name: str, value: str) -> bool:
        """Bind a string-valued QOM property to a backend or other named object."""
        ...

    def attach_serial_chardev(self, index: int, chardev_id: str) -> bool:
        """Attach a named chardev to a legacy serial slot."""
        ...

    def get_chardev_endpoint(self, chardev_id: str) -> Optional[str]:
        """Return endpoint metadata such as PTY path for a named chardev."""
        ...

    def poll_events(self, block: bool) -> int:
        """Pump the backend event sources and return the number of iterations."""
        ...


class NativeBackend:
    """
    ctypes-backed implementation of the in-tree Hedgehog backend API.

    The shared object path can be provided explicitly, or discovered using:
    - $QEMU_HEDGEHOG_BACKEND_LIBRARY
    - the dynamic linker default search path.
    """

    def __init__(self, lib: ctypes.CDLL, backend_handle: int):
        self._lib = lib
        self._handle = ctypes.c_void_p(backend_handle)
        self._closed = False

        self._tb_hook_bridge: Optional[object] = None
        self._insn_hook_bridge: Optional[object] = None
        self._invalid_hook_bridge: Optional[object] = None
        self._mmio_bridges: List[Tuple[object, object]] = []

    @classmethod
    def create(
        cls,
        cpu_type: str,
        machine_type: Optional[str] = None,
        library_path: Optional[str] = None,
        chardevs: Optional[Dict[str, str]] = None,
        property_bindings: Optional[Dict[str, Dict[str, str]]] = None,
        serial_backends: Optional[Dict[int, str]] = None,
    ) -> 'NativeBackend':
        """
        Create and initialize a backend instance.
        """
        global _NATIVE_BACKEND_PROCESS_SINGLETON

        if not cpu_type:
            raise HedgehogError(HEDGEHOG_ERR_ARG, 'cpu_type is required')

        if serial_backends and (machine_type is None or machine_type == 'none'):
            raise HedgehogError(
                HEDGEHOG_ERR_ARG,
                'serial_backends require a board-backed machine_type',
            )
        if property_bindings and (machine_type is None or machine_type == 'none'):
            raise HedgehogError(
                HEDGEHOG_ERR_ARG,
                'property_bindings require a board-backed machine_type',
            )

        with _NATIVE_BACKEND_PROCESS_LOCK:
            if _NATIVE_BACKEND_PROCESS_SINGLETON is not None:
                raise HedgehogError(
                    HEDGEHOG_ERR_RESOURCE,
                    _NATIVE_BACKEND_SINGLETON_ERROR,
                )

            lib = _load_native_library(library_path)
            _configure_library_api(lib)

            for chardev_id, uri in (chardevs or {}).items():
                ok, detail = _call_bool_with_error(
                    lib,
                    lib.hedgehog_backend_chardev_add,
                    chardev_id.encode('ascii'),
                    uri.encode('ascii'),
                )
                if not ok:
                    raise HedgehogError(
                        HEDGEHOG_ERR_RESOURCE,
                        _format_creation_error(
                            f'failed to create chardev {chardev_id}',
                            cpu_type,
                            machine_type,
                            detail,
                        ),
                    )

            for object_path, bindings in (property_bindings or {}).items():
                for property_name, value in bindings.items():
                    ok, detail = _call_bool_with_error(
                        lib,
                        lib.hedgehog_backend_bind_property,
                        object_path.encode('ascii'),
                        property_name.encode('ascii'),
                        value.encode('ascii'),
                    )
                    if not ok:
                        raise HedgehogError(
                            HEDGEHOG_ERR_RESOURCE,
                            _format_creation_error(
                                f'failed to bind {object_path}:{property_name}={value}',
                                cpu_type,
                                machine_type,
                                detail,
                            ),
                        )

            for index, chardev_id in sorted((serial_backends or {}).items()):
                ok, detail = _call_bool_with_error(
                    lib,
                    lib.hedgehog_backend_chardev_attach_serial,
                    ctypes.c_int(index),
                    chardev_id.encode('ascii'),
                )
                if not ok:
                    raise HedgehogError(
                        HEDGEHOG_ERR_RESOURCE,
                        _format_creation_error(
                            f'failed to attach chardev {chardev_id} to serial{index}',
                            cpu_type,
                            machine_type,
                            detail,
                        ),
                    )

            if machine_type and hasattr(lib, 'hedgehog_backend_initialize_for_machine'):
                machine_arg = machine_type.encode('ascii')
                initialized, detail = _call_bool_with_error(
                    lib,
                    lib.hedgehog_backend_initialize_for_machine,
                    machine_arg,
                )
            else:
                initialized, detail = _call_bool_with_error(
                    lib,
                    lib.hedgehog_backend_initialize,
                )

            if not initialized:
                raise HedgehogError(
                    HEDGEHOG_ERR_RESOURCE,
                    _format_creation_error(
                        'failed to initialize qemu hedgehog backend',
                        cpu_type,
                        machine_type,
                        detail,
                    ),
                )

            if machine_type and not hasattr(lib, 'hedgehog_backend_new_with_machine'):
                raise HedgehogError(
                    HEDGEHOG_ERR_RESOURCE,
                    'loaded backend library does not support machine_type selection',
                )

            if hasattr(lib, 'hedgehog_backend_new_with_machine'):
                machine_arg = machine_type.encode('ascii') if machine_type else None
                backend, detail = _call_pointer_with_error(
                    lib,
                    lib.hedgehog_backend_new_with_machine,
                    cpu_type.encode('ascii'),
                    machine_arg,
                )
            else:
                backend, detail = _call_pointer_with_error(
                    lib,
                    lib.hedgehog_backend_new,
                    cpu_type.encode('ascii'),
                )
            if backend is None or int(backend) == 0:
                raise HedgehogError(
                    HEDGEHOG_ERR_RESOURCE,
                    _format_creation_error(
                        f'failed to create backend for cpu type {cpu_type}',
                        cpu_type,
                        machine_type,
                        detail,
                        library_name=_library_name(lib),
                    ),
                )

            instance = cls(lib, int(backend))
            _NATIVE_BACKEND_PROCESS_SINGLETON = instance
            return instance

    def close(self) -> None:
        if self._closed:
            return

        # The embedded QEMU/TCG runtime is process-global and does not currently
        # provide a safe teardown path for repeated create/close cycles. Clear
        # host-side callbacks so the Python objects can be collected, but keep
        # the native backend alive until process exit.
        self._clear_host_callbacks()
        self._closed = True

    def _clear_host_callbacks(self) -> None:
        self._lib.hedgehog_backend_set_tb_hook(self._handle, None, None)
        self._lib.hedgehog_backend_set_insn_hook(self._handle, None, None)
        self._lib.hedgehog_backend_set_invalid_mem_hook(self._handle, None, None)
        self._tb_hook_bridge = None
        self._insn_hook_bridge = None
        self._invalid_hook_bridge = None
        self._mmio_bridges.clear()

    def _ensure_open(self) -> None:
        if self._closed:
            raise HedgehogError(HEDGEHOG_ERR_RESOURCE, _NATIVE_BACKEND_CLOSED_ERROR)

    def map_ram(self, name: str, addr: int, size: int) -> bool:
        self._ensure_open()
        return bool(
            self._lib.hedgehog_backend_map_ram(
                self._handle,
                name.encode('ascii', 'replace'),
                ctypes.c_uint64(addr),
                ctypes.c_uint64(size),
                None,
            )
        )

    def map_mmio(
        self,
        name: str,
        addr: int,
        size: int,
        read_fn: MMIOReadCallback,
        write_fn: MMIOWriteCallback,
    ) -> bool:
        self._ensure_open()

        def read_bridge(_opaque: int, io_addr: int, io_size: int) -> int:
            return int(read_fn(int(io_addr), int(io_size))) & 0xFFFFFFFFFFFFFFFF

        def write_bridge(
            _opaque: int,
            io_addr: int,
            io_value: int,
            io_size: int,
        ) -> None:
            write_fn(int(io_addr), int(io_value), int(io_size))

        read_cb = _MMIO_READ_BRIDGE(read_bridge)
        write_cb = _MMIO_WRITE_BRIDGE(write_bridge)

        ok = bool(
            self._lib.hedgehog_backend_map_mmio(
                self._handle,
                name.encode('ascii', 'replace'),
                ctypes.c_uint64(addr),
                ctypes.c_uint64(size),
                ctypes.cast(read_cb, ctypes.c_void_p),
                ctypes.cast(write_cb, ctypes.c_void_p),
                None,
                None,
            )
        )

        if ok:
            self._mmio_bridges.append((read_cb, write_cb))
        return ok

    def mem_read(self, addr: int, size: int) -> Tuple[int, bytes]:
        self._ensure_open()
        buf = ctypes.create_string_buffer(size)
        result = int(
            self._lib.hedgehog_backend_mem_read(
                self._handle,
                ctypes.c_uint64(addr),
                ctypes.cast(buf, ctypes.c_void_p),
                ctypes.c_uint64(size),
            )
        )
        return result, bytes(buf.raw)

    def mem_write(self, addr: int, data: bytes) -> int:
        self._ensure_open()
        buf = ctypes.create_string_buffer(data, len(data))
        return int(
            self._lib.hedgehog_backend_mem_write(
                self._handle,
                ctypes.c_uint64(addr),
                ctypes.cast(buf, ctypes.c_void_p),
                ctypes.c_uint64(len(data)),
            )
        )

    def unmap(self, addr: int, size: int) -> bool:
        self._ensure_open()
        return bool(
            self._lib.hedgehog_backend_mem_unmap(
                self._handle,
                ctypes.c_uint64(addr),
                ctypes.c_uint64(size),
                None,
            )
        )

    def reg_read(self, regno: int, buf_size: int) -> Optional[bytes]:
        self._ensure_open()
        buf = ctypes.create_string_buffer(buf_size)
        nread = int(
            self._lib.hedgehog_backend_reg_read(
                self._handle,
                ctypes.c_int(regno),
                ctypes.cast(buf, ctypes.c_void_p),
                ctypes.c_size_t(buf_size),
                None,
            )
        )
        if nread < 0:
            return None
        return bytes(buf.raw[:nread])

    def reg_write(self, regno: int, data: bytes) -> bool:
        self._ensure_open()
        buf = ctypes.create_string_buffer(data, len(data))
        nwritten = int(
            self._lib.hedgehog_backend_reg_write(
                self._handle,
                ctypes.c_int(regno),
                ctypes.cast(buf, ctypes.c_void_p),
                ctypes.c_size_t(len(data)),
                None,
            )
        )
        return nwritten >= 0

    def set_tb_hook(self, callback: Optional[ExecHookCallback]) -> None:
        self._ensure_open()
        self._tb_hook_bridge = _maybe_wrap_exec_hook(callback)
        self._lib.hedgehog_backend_set_tb_hook(
            self._handle,
            _callback_pointer(self._tb_hook_bridge),
            None,
        )

    def set_insn_hook(self, callback: Optional[ExecHookCallback]) -> None:
        self._ensure_open()
        self._insn_hook_bridge = _maybe_wrap_exec_hook(callback)
        self._lib.hedgehog_backend_set_insn_hook(
            self._handle,
            _callback_pointer(self._insn_hook_bridge),
            None,
        )

    def set_invalid_mem_hook(
        self,
        callback: Optional[InvalidHookCallback],
    ) -> None:
        self._ensure_open()
        self._invalid_hook_bridge = _maybe_wrap_invalid_hook(callback)
        self._lib.hedgehog_backend_set_invalid_mem_hook(
            self._handle,
            _callback_pointer(self._invalid_hook_bridge),
            None,
        )

    def reset(self) -> None:
        self._ensure_open()
        self._lib.hedgehog_backend_reset(self._handle)

    def set_pc(self, addr: int) -> None:
        self._ensure_open()
        self._lib.hedgehog_backend_set_pc(self._handle, ctypes.c_uint64(addr))

    def get_pc(self) -> int:
        self._ensure_open()
        return int(self._lib.hedgehog_backend_get_pc(self._handle))

    def run(self, max_instructions: int) -> Tuple[int, int]:
        self._ensure_open()
        cpu_exit = ctypes.c_int(0)
        run_result = int(
            self._lib.hedgehog_backend_run(
                self._handle,
                ctypes.c_uint64(max_instructions),
                ctypes.byref(cpu_exit),
            )
        )
        return run_result, int(cpu_exit.value)

    def stop(self) -> None:
        self._ensure_open()
        self._lib.hedgehog_backend_stop(self._handle)

    def add_chardev(self, chardev_id: str, uri: str) -> bool:
        self._ensure_open()
        return bool(
            self._lib.hedgehog_backend_chardev_add(
                chardev_id.encode('ascii', 'replace'),
                uri.encode('ascii', 'replace'),
                None,
            )
        )

    def bind_property(self, object_path: str, property_name: str, value: str) -> bool:
        self._ensure_open()
        return bool(
            self._lib.hedgehog_backend_bind_property(
                object_path.encode('ascii', 'replace'),
                property_name.encode('ascii', 'replace'),
                value.encode('ascii', 'replace'),
                None,
            )
        )

    def attach_serial_chardev(self, index: int, chardev_id: str) -> bool:
        self._ensure_open()
        return bool(
            self._lib.hedgehog_backend_chardev_attach_serial(
                ctypes.c_int(index),
                chardev_id.encode('ascii', 'replace'),
                None,
            )
        )

    def get_chardev_endpoint(self, chardev_id: str) -> Optional[str]:
        self._ensure_open()
        required = int(
            self._lib.hedgehog_backend_chardev_get_endpoint(
                chardev_id.encode('ascii', 'replace'),
                None,
                ctypes.c_size_t(0),
                None,
            )
        )
        if required <= 0:
            return None

        buf = ctypes.create_string_buffer(required)
        final_size = int(
            self._lib.hedgehog_backend_chardev_get_endpoint(
                chardev_id.encode('ascii', 'replace'),
                ctypes.cast(buf, ctypes.c_char_p),
                ctypes.c_size_t(len(buf)),
                None,
            )
        )
        if final_size <= 0:
            return None
        return buf.value.decode('utf-8', errors='replace')

    def poll_events(self, block: bool) -> int:
        self._ensure_open()
        return int(
            self._lib.hedgehog_backend_poll_events(
                ctypes.c_bool(block),
                None,
            )
        )

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass


def _callback_pointer(callback: Optional[Any]) -> Optional[ctypes.c_void_p]:
    if callback is None:
        return None
    return ctypes.cast(cast(Any, callback), ctypes.c_void_p)


def _maybe_wrap_exec_hook(callback: Optional[ExecHookCallback]) -> Optional[object]:
    if callback is None:
        return None

    def hook_bridge(_uc_ptr: int, pc: int, _opaque: int) -> bool:
        return bool(callback(int(pc)))

    return _EXEC_HOOK_BRIDGE(hook_bridge)


def _maybe_wrap_invalid_hook(
    callback: Optional[InvalidHookCallback],
) -> Optional[object]:
    if callback is None:
        return None

    def hook_bridge(
        _uc_ptr: int,
        addr: int,
        size: int,
        access_type: int,
        response: int,
        _opaque: int,
    ) -> bool:
        return bool(
            callback(
                int(addr),
                int(size),
                int(access_type),
                int(response),
            )
        )

    return _INVALID_HOOK_BRIDGE(hook_bridge)


def _load_native_library(library_path: Optional[str]) -> ctypes.CDLL:
    candidates: List[str] = []
    if library_path:
        candidates.append(library_path)

    env_path = os.getenv('QEMU_HEDGEHOG_BACKEND_LIBRARY')
    if env_path:
        candidates.append(env_path)

    candidates.extend(_packaged_library_candidates())

    for libname in ('qemu-hedgehog-backend', 'qemu-hedgehog-backend-aarch64'):
        found = ctypes.util.find_library(libname)
        if found:
            candidates.append(found)

    for candidate in candidates:
        try:
            return ctypes.CDLL(candidate)
        except OSError:
            continue

    raise HedgehogError(
        HEDGEHOG_ERR_RESOURCE,
        'unable to locate hedgehog backend library; set '
        'QEMU_HEDGEHOG_BACKEND_LIBRARY to a shared object path',
    )


def _packaged_library_candidates() -> List[str]:
    native_dir = os.path.join(os.path.dirname(__file__), '_native')
    if not os.path.isdir(native_dir):
        return []

    matches: List[str] = []
    patterns = (
        'libqemu-hedgehog-backend*.so*',
        'libqemu-hedgehog-backend*.dylib',
        '*qemu-hedgehog-backend*.dll',
    )
    for pattern in patterns:
        matches.extend(sorted(glob.glob(os.path.join(native_dir, pattern))))
    return matches


def _configure_library_api(lib: ctypes.CDLL) -> None:
    error_ptr_t = ctypes.POINTER(ctypes.c_void_p)

    lib.hedgehog_backend_initialize.argtypes = [error_ptr_t]
    lib.hedgehog_backend_initialize.restype = ctypes.c_bool

    if hasattr(lib, 'hedgehog_backend_initialize_for_machine'):
        lib.hedgehog_backend_initialize_for_machine.argtypes = [
            ctypes.c_char_p,
            error_ptr_t,
        ]
        lib.hedgehog_backend_initialize_for_machine.restype = ctypes.c_bool

    lib.hedgehog_backend_new.argtypes = [ctypes.c_char_p, error_ptr_t]
    lib.hedgehog_backend_new.restype = ctypes.c_void_p

    lib.hedgehog_backend_chardev_add.argtypes = [
        ctypes.c_char_p,
        ctypes.c_char_p,
        error_ptr_t,
    ]
    lib.hedgehog_backend_chardev_add.restype = ctypes.c_bool

    lib.hedgehog_backend_bind_property.argtypes = [
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        error_ptr_t,
    ]
    lib.hedgehog_backend_bind_property.restype = ctypes.c_bool

    lib.hedgehog_backend_chardev_attach_serial.argtypes = [
        ctypes.c_int,
        ctypes.c_char_p,
        error_ptr_t,
    ]
    lib.hedgehog_backend_chardev_attach_serial.restype = ctypes.c_bool

    lib.hedgehog_backend_chardev_get_endpoint.argtypes = [
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_size_t,
        error_ptr_t,
    ]
    lib.hedgehog_backend_chardev_get_endpoint.restype = ctypes.c_int

    lib.hedgehog_backend_poll_events.argtypes = [
        ctypes.c_bool,
        error_ptr_t,
    ]
    lib.hedgehog_backend_poll_events.restype = ctypes.c_int

    if hasattr(lib, 'hedgehog_backend_new_with_machine'):
        lib.hedgehog_backend_new_with_machine.argtypes = [
            ctypes.c_char_p,
            ctypes.c_char_p,
            error_ptr_t,
        ]
        lib.hedgehog_backend_new_with_machine.restype = ctypes.c_void_p

    lib.hedgehog_backend_free.argtypes = [ctypes.c_void_p]
    lib.hedgehog_backend_free.restype = None

    lib.hedgehog_backend_map_ram.argtypes = [
        ctypes.c_void_p,
        ctypes.c_char_p,
        ctypes.c_uint64,
        ctypes.c_uint64,
        error_ptr_t,
    ]
    lib.hedgehog_backend_map_ram.restype = ctypes.c_bool

    lib.hedgehog_backend_map_mmio.argtypes = [
        ctypes.c_void_p,
        ctypes.c_char_p,
        ctypes.c_uint64,
        ctypes.c_uint64,
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
        error_ptr_t,
    ]
    lib.hedgehog_backend_map_mmio.restype = ctypes.c_bool

    lib.hedgehog_backend_mem_read.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint64,
        ctypes.c_void_p,
        ctypes.c_uint64,
    ]
    lib.hedgehog_backend_mem_read.restype = ctypes.c_uint32

    lib.hedgehog_backend_mem_write.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint64,
        ctypes.c_void_p,
        ctypes.c_uint64,
    ]
    lib.hedgehog_backend_mem_write.restype = ctypes.c_uint32

    lib.hedgehog_backend_mem_unmap.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint64,
        ctypes.c_uint64,
        error_ptr_t,
    ]
    lib.hedgehog_backend_mem_unmap.restype = ctypes.c_bool

    lib.hedgehog_backend_reg_read.argtypes = [
        ctypes.c_void_p,
        ctypes.c_int,
        ctypes.c_void_p,
        ctypes.c_size_t,
        error_ptr_t,
    ]
    lib.hedgehog_backend_reg_read.restype = ctypes.c_int

    lib.hedgehog_backend_reg_write.argtypes = [
        ctypes.c_void_p,
        ctypes.c_int,
        ctypes.c_void_p,
        ctypes.c_size_t,
        error_ptr_t,
    ]
    lib.hedgehog_backend_reg_write.restype = ctypes.c_int

    lib.hedgehog_backend_set_tb_hook.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
    ]
    lib.hedgehog_backend_set_tb_hook.restype = None

    lib.hedgehog_backend_set_insn_hook.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
    ]
    lib.hedgehog_backend_set_insn_hook.restype = None

    lib.hedgehog_backend_set_invalid_mem_hook.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
    ]
    lib.hedgehog_backend_set_invalid_mem_hook.restype = None

    lib.hedgehog_backend_reset.argtypes = [ctypes.c_void_p]
    lib.hedgehog_backend_reset.restype = None

    lib.hedgehog_backend_set_pc.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
    lib.hedgehog_backend_set_pc.restype = None

    lib.hedgehog_backend_get_pc.argtypes = [ctypes.c_void_p]
    lib.hedgehog_backend_get_pc.restype = ctypes.c_uint64

    lib.hedgehog_backend_run.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint64,
        ctypes.POINTER(ctypes.c_int),
    ]
    lib.hedgehog_backend_run.restype = ctypes.c_int

    if hasattr(lib, 'error_get_pretty'):
        lib.error_get_pretty.argtypes = [ctypes.c_void_p]
        lib.error_get_pretty.restype = ctypes.c_char_p

    if hasattr(lib, 'error_free'):
        lib.error_free.argtypes = [ctypes.c_void_p]
        lib.error_free.restype = None


def _call_bool_with_error(
    lib: ctypes.CDLL,
    func: Any,
    *args: Any,
) -> Tuple[bool, Optional[str]]:
    err = ctypes.c_void_p()
    ok = bool(func(*args, ctypes.byref(err)))
    return ok, _consume_error_detail(lib, err)


def _call_pointer_with_error(
    lib: ctypes.CDLL,
    func: Any,
    *args: Any,
) -> Tuple[Optional[int], Optional[str]]:
    err = ctypes.c_void_p()
    result = func(*args, ctypes.byref(err))
    detail = _consume_error_detail(lib, err)
    if result is None:
        return None, detail
    value = int(result)
    if value == 0:
        return None, detail
    return value, detail


def _consume_error_detail(lib: ctypes.CDLL, err: ctypes.c_void_p) -> Optional[str]:
    err_value = int(err.value or 0)
    if err_value == 0:
        return None

    message: Optional[str] = None
    if hasattr(lib, 'error_get_pretty'):
        try:
            pretty = lib.error_get_pretty(ctypes.c_void_p(err_value))
            if pretty:
                message = cast(bytes, pretty).decode('utf-8', errors='replace')
        except Exception:
            message = None

    if hasattr(lib, 'error_free'):
        try:
            lib.error_free(ctypes.c_void_p(err_value))
        except Exception:
            pass

    return message


def _library_name(lib: ctypes.CDLL) -> Optional[str]:
    name = getattr(lib, '_name', None)
    if not name:
        return None
    return os.fspath(name)


def _cpu_library_hint(cpu_type: str) -> Optional[str]:
    cpu = cpu_type.lower()
    arm_markers = ('arm', 'cortex-', 'cpsr', 'v7', 'v8')
    if any(marker in cpu for marker in arm_markers):
        return (
            'for ARM/AArch64 CPU models, use the aarch64 backend library '
            '(for example libqemu-hedgehog-backend-aarch64.so)'
        )
    return None


def _format_creation_error(
    summary: str,
    cpu_type: str,
    machine_type: Optional[str],
    detail: Optional[str],
    library_name: Optional[str] = None,
) -> str:
    pieces = [summary]
    if machine_type:
        pieces.append(f'machine_type={machine_type}')
    if library_name:
        pieces.append(f'library={library_name}')
    if detail:
        pieces.append(f'backend detail: {detail}')
    hint = _cpu_library_hint(cpu_type)
    if hint:
        pieces.append(f'hint: {hint}')
    return '; '.join(pieces)

    lib.hedgehog_backend_stop.argtypes = [ctypes.c_void_p]
    lib.hedgehog_backend_stop.restype = None


__all__ = (
    'BackendProtocol',
    'ExecHookCallback',
    'InvalidHookCallback',
    'MMIOReadCallback',
    'MMIOWriteCallback',
    'NativeBackend',
)

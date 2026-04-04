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
from typing import Any, Callable, List, Optional, Protocol, Tuple, cast
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
    ) -> 'NativeBackend':
        """
        Create and initialize a backend instance.
        """
        if not cpu_type:
            raise HedgehogError(HEDGEHOG_ERR_ARG, 'cpu_type is required')

        lib = _load_native_library(library_path)
        _configure_library_api(lib)

        if not bool(lib.hedgehog_backend_initialize(None)):
            raise HedgehogError(
                HEDGEHOG_ERR_RESOURCE,
                'failed to initialize qemu hedgehog backend',
            )

        if machine_type and not hasattr(lib, 'hedgehog_backend_new_with_machine'):
            raise HedgehogError(
                HEDGEHOG_ERR_RESOURCE,
                'loaded backend library does not support machine_type selection',
            )

        if hasattr(lib, 'hedgehog_backend_new_with_machine'):
            machine_arg = machine_type.encode('ascii') if machine_type else None
            backend = lib.hedgehog_backend_new_with_machine(
                cpu_type.encode('ascii'),
                machine_arg,
                None,
            )
        else:
            backend = lib.hedgehog_backend_new(cpu_type.encode('ascii'), None)
        if backend is None or int(backend) == 0:
            raise HedgehogError(
                HEDGEHOG_ERR_RESOURCE,
                f'failed to create backend for cpu type {cpu_type}',
            )

        return cls(lib, int(backend))

    def close(self) -> None:
        if self._closed:
            return
        self._lib.hedgehog_backend_free(self._handle)
        self._closed = True

    def map_ram(self, name: str, addr: int, size: int) -> bool:
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
        buf = ctypes.create_string_buffer(data, len(data))
        return int(
            self._lib.hedgehog_backend_mem_write(
                self._handle,
                ctypes.c_uint64(addr),
                ctypes.cast(buf, ctypes.c_void_p),
                ctypes.c_uint64(len(data)),
            )
        )

    def reg_read(self, regno: int, buf_size: int) -> Optional[bytes]:
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
        self._tb_hook_bridge = _maybe_wrap_exec_hook(callback)
        self._lib.hedgehog_backend_set_tb_hook(
            self._handle,
            _callback_pointer(self._tb_hook_bridge),
            None,
        )

    def set_insn_hook(self, callback: Optional[ExecHookCallback]) -> None:
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
        self._invalid_hook_bridge = _maybe_wrap_invalid_hook(callback)
        self._lib.hedgehog_backend_set_invalid_mem_hook(
            self._handle,
            _callback_pointer(self._invalid_hook_bridge),
            None,
        )

    def reset(self) -> None:
        self._lib.hedgehog_backend_reset(self._handle)

    def set_pc(self, addr: int) -> None:
        self._lib.hedgehog_backend_set_pc(self._handle, ctypes.c_uint64(addr))

    def get_pc(self) -> int:
        return int(self._lib.hedgehog_backend_get_pc(self._handle))

    def run(self, max_instructions: int) -> Tuple[int, int]:
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
        self._lib.hedgehog_backend_stop(self._handle)

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

    lib.hedgehog_backend_new.argtypes = [ctypes.c_char_p, error_ptr_t]
    lib.hedgehog_backend_new.restype = ctypes.c_void_p

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

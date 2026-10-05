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
from dataclasses import dataclass
from typing import Any, Callable, Dict, List, Optional, Protocol, Tuple, cast
from typing import runtime_checkable

from .constants import HEDGEHOG_ERR_ARG, HEDGEHOG_ERR_RESOURCE
from .errors import HedgehogError

ExecHookCallback = Callable[[int], bool]
InvalidHookCallback = Callable[[int, int, int, int], bool]
InvalidInsnHookCallback = Callable[[int, bytes, int, int, int], Tuple[int, int]]
MMIOReadCallback = Callable[[int, int], int]
MMIOWriteCallback = Callable[[int, int, int], None]

_INVALID_INSN_PASS = 0
_INVALID_INSN_CONTINUE = 1
_INVALID_INSN_STOP = 2


_VIRTUAL_TIMER_BRIDGE = ctypes.CFUNCTYPE(None, ctypes.c_void_p)

_SYSTEM_CALL_EVENT_REQUEST = 1
_SYSTEM_CALL_EVENT_COMPLETE = 2
_SYSTEM_CALL_SVC = 1
_SYSTEM_CALL_HVC = 2
_SYSTEM_CALL_SMC = 3


class _SystemCallEvent(ctypes.Structure):
    _fields_ = [
        ('sequence', ctypes.c_uint64),
        ('pc', ctypes.c_uint64),
        ('return_pc', ctypes.c_uint64),
        ('x', ctypes.c_uint64 * 8),
        ('source_el', ctypes.c_uint32),
        ('target_el', ctypes.c_uint32),
        ('return_el', ctypes.c_uint32),
        ('immediate', ctypes.c_uint32),
        ('kind', ctypes.c_uint32),
        ('phase', ctypes.c_uint32),
    ]


class _CPUWaitEvent(ctypes.Structure):
    _fields_ = [
        ('pc', ctypes.c_uint64),
        ('current_el', ctypes.c_uint32),
        ('kind', ctypes.c_uint32),
    ]


@dataclass(frozen=True)
class SystemCallEvent:
    """A native AArch64 SVC, HVC, or SMC request or return record."""

    sequence: int
    phase: str
    instruction: str
    pc: int
    return_pc: int
    source_el: int
    target_el: Optional[int]
    return_el: Optional[int]
    immediate: int
    registers: Tuple[int, int, int, int, int, int, int, int]


@dataclass(frozen=True)
class CPUWaitEvent:
    """An accepted native architectural CPU wait observation."""

    pc: int
    current_el: int
    kind: str


@dataclass(frozen=True)
class InvalidMemoryDiagnostic:
    """Bounded native information from the most recent failed memory access."""

    fault_vaddr: int
    physical_address: Optional[int]
    region_base: Optional[int]
    region_name: Optional[str]
    mmu_idx: int
    size: int
    access_type: int
    memtx_result: int
    translation_valid: bool
    region_valid: bool


SystemCallObserverCallback = Callable[[SystemCallEvent], None]
CPUWaitObserverCallback = Callable[[CPUWaitEvent], None]
CPUResetObserverCallback = Callable[[], None]

_SYSTEM_CALL_OBSERVER_BRIDGE = ctypes.CFUNCTYPE(
    None, ctypes.c_void_p, ctypes.POINTER(_SystemCallEvent),
)

_CPU_WAIT_OBSERVER_BRIDGE = ctypes.CFUNCTYPE(
    None, ctypes.c_void_p, ctypes.POINTER(_CPUWaitEvent),
)

_CPU_RESET_OBSERVER_BRIDGE = ctypes.CFUNCTYPE(None, ctypes.c_void_p)

_CPU_OUTPUT_BRIDGE = ctypes.CFUNCTYPE(
    None, ctypes.c_void_p, ctypes.c_uint, ctypes.c_bool,
)

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


class _InvalidInstructionInfo(ctypes.Structure):
    _fields_ = [
        ('pc', ctypes.c_uint64),
        ('syndrome', ctypes.c_uint64),
        ('exception_index', ctypes.c_int32),
        ('size', ctypes.c_uint8),
        ('bytes_len', ctypes.c_uint8),
        ('bytes', ctypes.c_uint8 * 16),
    ]


class _InvalidMemoryDiagnostic(ctypes.Structure):
    _fields_ = [
        ('addr', ctypes.c_uint64),
        ('physical_address', ctypes.c_uint64),
        ('region_base', ctypes.c_uint64),
        ('size', ctypes.c_uint),
        ('mmu_idx', ctypes.c_int32),
        ('access_type', ctypes.c_int),
        ('response', ctypes.c_uint32),
        ('translation_valid', ctypes.c_bool),
        ('region_valid', ctypes.c_bool),
        ('region_name', ctypes.c_char * 96),
    ]


_INVALID_INSN_HOOK_BRIDGE = ctypes.CFUNCTYPE(
    ctypes.c_int,
    ctypes.c_void_p,
    ctypes.POINTER(_InvalidInstructionInfo),
    ctypes.POINTER(ctypes.c_uint64),
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


class _ArmDiagnostics(ctypes.Structure):
    _fields_ = [
        ('pc', ctypes.c_uint64),
        ('pstate', ctypes.c_uint64),
        ('scr_el3', ctypes.c_uint64),
        ('hcr_el2', ctypes.c_uint64),
        ('hcr_el2_eff', ctypes.c_uint64),
        ('elr_el1', ctypes.c_uint64),
        ('elr_el2', ctypes.c_uint64),
        ('elr_el3', ctypes.c_uint64),
        ('spsr_el1', ctypes.c_uint64),
        ('spsr_el2', ctypes.c_uint64),
        ('spsr_el3', ctypes.c_uint64),
        ('esr_el1', ctypes.c_uint64),
        ('esr_el2', ctypes.c_uint64),
        ('esr_el3', ctypes.c_uint64),
        ('far_el1', ctypes.c_uint64),
        ('far_el2', ctypes.c_uint64),
        ('far_el3', ctypes.c_uint64),
        ('exception_syndrome', ctypes.c_uint64),
        ('exception_vaddress', ctypes.c_uint64),
        ('current_el', ctypes.c_uint32),
        ('cpsr', ctypes.c_uint32),
        ('interrupt_request', ctypes.c_uint32),
        ('halted', ctypes.c_uint32),
        ('exit_request', ctypes.c_uint32),
        ('exception_target_el', ctypes.c_uint32),
        ('exception_index', ctypes.c_int32),
        ('generic_timer_count', ctypes.c_uint32),
        ('generic_timer_counter', ctypes.c_uint64),
        ('generic_timer_cval', ctypes.c_uint64 * 7),
        ('generic_timer_ctl', ctypes.c_uint64 * 7),
        ('generic_timer_enabled', ctypes.c_uint32 * 7),
        ('generic_timer_imask', ctypes.c_uint32 * 7),
        ('generic_timer_istatus', ctypes.c_uint32 * 7),
    ]


class _ArmGenericTimerState(ctypes.Structure):
    _fields_ = [
        ('virtual_clock_ns', ctypes.c_int64),
        ('generic_timer_cval', ctypes.c_uint64 * 7),
        ('generic_timer_ctl', ctypes.c_uint64 * 7),
    ]


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

    def map_ram_ptr(self, name: str, addr: int, size: int, ptr: int) -> bool:
        """Map RAM backed by caller-owned host memory."""
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

    def set_invalid_insn_hook(
        self,
        callback: Optional[InvalidInsnHookCallback],
    ) -> None:
        """Set or clear undefined-instruction callback."""
        ...

    def set_invalid_mem_hook(
        self,
        callback: Optional[InvalidHookCallback],
    ) -> None:
        """Set or clear invalid-memory callback."""
        ...

    def set_invalid_memory_diagnostic_capture(self, enabled: bool) -> bool:
        """Enable bounded native invalid-memory diagnostics before execution."""
        ...

    def last_invalid_memory_diagnostic(self) -> Optional[InvalidMemoryDiagnostic]:
        """Return the latest native invalid-memory diagnostic, if available."""
        ...

    def reset(self) -> None:
        """Reset CPU state."""
        ...

    def reset_stop(self) -> None:
        """Clear stop state before starting a new run."""
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

    def set_hard_interrupt(self, asserted: bool) -> bool:
        """Drive the CPU's level-triggered hard interrupt input."""
        ...

    def set_aarch64_reset_state(self, current_el: int, secure: bool) -> bool:
        """Set the standalone AArch64 reset/current CPU state."""
        ...

    def add_aarch64_cp_reg(
        self,
        opc0: int,
        opc1: int,
        crn: int,
        crm: int,
        opc2: int,
        min_el: int,
        readable: bool,
        writable: bool,
        reset_value: int,
    ) -> bool:
        """Add one standalone AArch64 system-register overlay."""
        ...

    def set_aarch64_cp_reg_value(
        self,
        opc0: int,
        opc1: int,
        crn: int,
        crm: int,
        opc2: int,
        reset_value: int,
        value: int,
    ) -> bool:
        """Override one existing AArch64 system register's live/reset value."""
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

    def connect_cpu_output(self, index: int, callback: Callable[[int, bool], None]) -> bool:
        """Connect a standalone CPU output before execution."""
        ...

    def connect_system_call_observer(
        self, callback: SystemCallObserverCallback,
    ) -> bool:
        """Observe standalone AArch64 SVC/HVC/SMC entries and returns."""
        ...

    def connect_cpu_wait_observer(
        self, callback: CPUWaitObserverCallback,
    ) -> bool:
        """Observe accepted standalone architectural CPU waits."""
        ...

    def connect_cpu_reset_observer(
        self, callback: CPUResetObserverCallback,
    ) -> bool:
        """Observe a completed standalone queued architectural CPU reset."""
        ...

    def request_cpu_reset(self) -> bool:
        """Queue a standalone architectural reset at a run boundary."""
        ...

    def connect_virtual_timer(self, callback: Callable[[], None]) -> bool:
        """Register the standalone device deadline callback before execution."""
        ...

    def arm_virtual_timer(self, deadline_ns: int) -> bool:
        """Arm a native virtual deadline, or cancel with a negative value."""
        ...

    def virtual_clock_ns(self) -> int:
        """Observe the native virtual clock without executing instructions."""
        ...

    def poll_events(self, block: bool) -> int:
        """Pump the backend event sources and return the number of iterations."""
        ...

    def arm_diagnostics(self) -> Dict[str, object]:
        """Return read-only ARM CPU diagnostic state."""
        ...

    def restore_arm_generic_timers(
        self,
        virtual_clock_ns: int,
        cvals: Iterable[int],
        controls: Iterable[int],
    ) -> bool:
        """Restore the stopped native AArch64 generic-timer state."""
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
        self._invalid_insn_hook_bridge: Optional[object] = None
        self._invalid_hook_bridge: Optional[object] = None
        self._mmio_bridges: List[Tuple[object, object]] = []
        self._cpu_output_bridges: Dict[int, object] = {}
        self._virtual_timer_bridge = None
        self._system_call_observer_bridge: Optional[object] = None
        self._cpu_wait_observer_bridge: Optional[object] = None
        self._cpu_reset_observer_bridge: Optional[object] = None

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
        self._lib.hedgehog_backend_set_invalid_insn_hook(self._handle, None, None)
        self._lib.hedgehog_backend_set_invalid_mem_hook(self._handle, None, None)
        self._tb_hook_bridge = None
        self._insn_hook_bridge = None
        self._invalid_insn_hook_bridge = None
        self._invalid_hook_bridge = None
        self._mmio_bridges.clear()
        if self._virtual_timer_bridge is not None:
            self._lib.hedgehog_backend_connect_virtual_timer(self._handle, None, None, None)
            self._virtual_timer_bridge = None
        for index in self._cpu_output_bridges:
            self._lib.hedgehog_backend_connect_cpu_output(
                self._handle, ctypes.c_uint(index), None, None, None,
            )
        if self._system_call_observer_bridge is not None:
            self._lib.hedgehog_backend_connect_system_call_observer(
                self._handle, None, None, None,
            )
            self._system_call_observer_bridge = None
        if self._cpu_wait_observer_bridge is not None:
            self._lib.hedgehog_backend_connect_cpu_wait_observer(
                self._handle, None, None, None,
            )
            self._cpu_wait_observer_bridge = None
        if self._cpu_reset_observer_bridge is not None:
            self._lib.hedgehog_backend_connect_cpu_reset_observer(
                self._handle, None, None, None,
            )
            self._cpu_reset_observer_bridge = None
        self._cpu_output_bridges.clear()

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

    def map_ram_ptr(self, name: str, addr: int, size: int, ptr: int) -> bool:
        self._ensure_open()
        ok, _detail = _call_bool_with_error(
            self._lib,
            self._lib.hedgehog_backend_map_ram_ptr,
            self._handle,
            name.encode('utf-8'),
            ctypes.c_uint64(addr),
            ctypes.c_uint64(size),
            ctypes.c_void_p(ptr),
        )
        return ok

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

    def set_invalid_insn_hook(
        self,
        callback: Optional[InvalidInsnHookCallback],
    ) -> None:
        self._ensure_open()
        self._invalid_insn_hook_bridge = _maybe_wrap_invalid_insn_hook(callback)
        self._lib.hedgehog_backend_set_invalid_insn_hook(
            self._handle,
            _callback_pointer(self._invalid_insn_hook_bridge),
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

    def set_invalid_memory_diagnostic_capture(self, enabled: bool) -> bool:
        self._ensure_open()
        if not hasattr(self._lib, 'hedgehog_backend_set_invalid_mem_diagnostic_capture'):
            return False
        ok, _detail = _call_bool_with_error(
            self._lib,
            self._lib.hedgehog_backend_set_invalid_mem_diagnostic_capture,
            self._handle,
            ctypes.c_bool(enabled),
        )
        return ok

    def last_invalid_memory_diagnostic(self) -> Optional[InvalidMemoryDiagnostic]:
        self._ensure_open()
        if not hasattr(self._lib, 'hedgehog_backend_get_last_invalid_mem_diagnostic'):
            return None
        diagnostic = _InvalidMemoryDiagnostic()
        if not self._lib.hedgehog_backend_get_last_invalid_mem_diagnostic(
            self._handle,
            ctypes.byref(diagnostic),
        ):
            return None
        raw_name = bytes(diagnostic.region_name).split(b'\0', 1)[0]
        region_name = raw_name.decode('ascii', errors='replace') or None
        translation_valid = bool(diagnostic.translation_valid)
        region_valid = bool(diagnostic.region_valid)
        return InvalidMemoryDiagnostic(
            fault_vaddr=int(diagnostic.addr),
            physical_address=(
                int(diagnostic.physical_address) if translation_valid else None
            ),
            region_base=int(diagnostic.region_base) if region_valid else None,
            region_name=region_name if region_valid else None,
            mmu_idx=int(diagnostic.mmu_idx),
            size=int(diagnostic.size),
            access_type=int(diagnostic.access_type),
            memtx_result=int(diagnostic.response),
            translation_valid=translation_valid,
            region_valid=region_valid,
        )

    def reset(self) -> None:
        self._ensure_open()
        self._lib.hedgehog_backend_reset(self._handle)

    def reset_stop(self) -> None:
        self._ensure_open()
        self._lib.hedgehog_backend_reset_stop(self._handle)

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

    def set_hard_interrupt(self, asserted: bool) -> bool:
        """Drive the CPU's level-triggered hard interrupt input."""
        self._ensure_open()
        ok, _detail = _call_bool_with_error(
            self._lib,
            self._lib.hedgehog_backend_set_hard_interrupt,
            self._handle,
            ctypes.c_bool(asserted),
        )
        return ok

    def set_aarch64_reset_state(self, current_el: int, secure: bool) -> bool:
        """Set the standalone AArch64 reset/current CPU state."""
        self._ensure_open()
        ok, detail = _call_bool_with_error(
            self._lib,
            self._lib.hedgehog_backend_set_aarch64_reset_state,
            self._handle,
            ctypes.c_uint(current_el),
            ctypes.c_bool(secure),
        )
        return ok

    def add_aarch64_cp_reg(
        self,
        opc0: int,
        opc1: int,
        crn: int,
        crm: int,
        opc2: int,
        min_el: int,
        readable: bool,
        writable: bool,
        reset_value: int,
    ) -> bool:
        """Add one standalone AArch64 system-register overlay."""
        self._ensure_open()
        ok, _detail = _call_bool_with_error(
            self._lib,
            self._lib.hedgehog_backend_add_aarch64_cp_reg,
            self._handle,
            ctypes.c_uint(opc0),
            ctypes.c_uint(opc1),
            ctypes.c_uint(crn),
            ctypes.c_uint(crm),
            ctypes.c_uint(opc2),
            ctypes.c_uint(min_el),
            ctypes.c_bool(readable),
            ctypes.c_bool(writable),
            ctypes.c_uint64(reset_value),
        )
        return ok

    def set_aarch64_cp_reg_value(
        self,
        opc0: int,
        opc1: int,
        crn: int,
        crm: int,
        opc2: int,
        reset_value: int,
        value: int,
    ) -> bool:
        """Override one existing AArch64 system register's live/reset value."""
        self._ensure_open()
        ok, _detail = _call_bool_with_error(
            self._lib,
            self._lib.hedgehog_backend_set_aarch64_cp_reg_value,
            self._handle,
            ctypes.c_uint(opc0),
            ctypes.c_uint(opc1),
            ctypes.c_uint(crn),
            ctypes.c_uint(crm),
            ctypes.c_uint(opc2),
            ctypes.c_uint64(reset_value),
            ctypes.c_uint64(value),
        )
        return ok

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

    def connect_cpu_output(self, index: int, callback: Callable[[int, bool], None]) -> bool:
        self._ensure_open()
        if not hasattr(self._lib, 'hedgehog_backend_connect_cpu_output'):
            raise RuntimeError('native backend does not support CPU output connections')
        def output_bridge(_opaque: int, output: int, level: bool) -> None:
            callback(int(output), bool(level))
        bridge = _CPU_OUTPUT_BRIDGE(output_bridge)
        ok = bool(self._lib.hedgehog_backend_connect_cpu_output(
            self._handle, ctypes.c_uint(index),
            ctypes.cast(bridge, ctypes.c_void_p), None, None,
        ))
        if ok:
            self._cpu_output_bridges[index] = bridge
        return ok

    def connect_system_call_observer(
        self, callback: SystemCallObserverCallback,
    ) -> bool:
        self._ensure_open()
        if not hasattr(self._lib, 'hedgehog_backend_connect_system_call_observer'):
            raise RuntimeError('native backend does not support system-call observation')

        def observer_bridge(
            _opaque: int, raw: ctypes.POINTER(_SystemCallEvent),
        ) -> None:
            event = raw.contents
            if event.phase == _SYSTEM_CALL_EVENT_REQUEST:
                phase = 'request'
                target_el: Optional[int] = None
                return_el: Optional[int] = None
            elif event.phase == _SYSTEM_CALL_EVENT_COMPLETE:
                phase = 'complete'
                target_el = int(event.target_el)
                return_el = int(event.return_el)
            else:
                raise RuntimeError(
                    f'unknown native system-call observer phase {event.phase}'
                )
            instruction = {
                _SYSTEM_CALL_SVC: 'svc',
                _SYSTEM_CALL_HVC: 'hvc',
                _SYSTEM_CALL_SMC: 'smc',
            }.get(int(event.kind))
            if instruction is None:
                raise RuntimeError(
                    f'unknown native system-call instruction {event.kind}'
                )
            registers = tuple(int(event.x[index]) for index in range(8))
            callback(SystemCallEvent(
                sequence=int(event.sequence),
                phase=phase,
                instruction=instruction,
                pc=int(event.pc),
                return_pc=int(event.return_pc),
                source_el=int(event.source_el),
                target_el=target_el,
                return_el=return_el,
                immediate=int(event.immediate),
                registers=registers,
            ))

        bridge = _SYSTEM_CALL_OBSERVER_BRIDGE(observer_bridge)
        ok = bool(self._lib.hedgehog_backend_connect_system_call_observer(
            self._handle, ctypes.cast(bridge, ctypes.c_void_p), None, None,
        ))
        if ok:
            self._system_call_observer_bridge = bridge
        return ok

    def connect_cpu_wait_observer(
        self, callback: CPUWaitObserverCallback,
    ) -> bool:
        self._ensure_open()
        if not hasattr(self._lib, 'hedgehog_backend_connect_cpu_wait_observer'):
            raise RuntimeError('native backend does not support CPU-wait observation')

        def observer_bridge(
            _opaque: int, raw: ctypes.POINTER(_CPUWaitEvent),
        ) -> None:
            event = raw.contents
            kind = {1: 'wfi'}.get(int(event.kind))
            if kind is None:
                raise RuntimeError(f'unknown native CPU-wait kind {event.kind}')
            callback(CPUWaitEvent(
                pc=int(event.pc),
                current_el=int(event.current_el),
                kind=kind,
            ))

        bridge = _CPU_WAIT_OBSERVER_BRIDGE(observer_bridge)
        ok = bool(self._lib.hedgehog_backend_connect_cpu_wait_observer(
            self._handle, ctypes.cast(bridge, ctypes.c_void_p), None, None,
        ))
        if ok:
            self._cpu_wait_observer_bridge = bridge
        return ok

    def connect_cpu_reset_observer(
        self, callback: CPUResetObserverCallback,
    ) -> bool:
        self._ensure_open()
        if not hasattr(self._lib, 'hedgehog_backend_connect_cpu_reset_observer'):
            raise RuntimeError('native backend does not support CPU-reset observation')

        bridge = _CPU_RESET_OBSERVER_BRIDGE(lambda _opaque: callback())
        ok = bool(self._lib.hedgehog_backend_connect_cpu_reset_observer(
            self._handle, ctypes.cast(bridge, ctypes.c_void_p), None, None,
        ))
        if ok:
            self._cpu_reset_observer_bridge = bridge
        return ok

    def request_cpu_reset(self) -> bool:
        self._ensure_open()
        if not hasattr(self._lib, 'hedgehog_backend_request_cpu_reset'):
            raise RuntimeError('native backend does not support queued CPU reset')
        return bool(self._lib.hedgehog_backend_request_cpu_reset(
            self._handle, None,
        ))

    def connect_virtual_timer(self, callback: Callable[[], None]) -> bool:
        self._ensure_open()
        if not hasattr(self._lib, 'hedgehog_backend_connect_virtual_timer'):
            raise RuntimeError('native backend does not support virtual timer callbacks')
        bridge = _VIRTUAL_TIMER_BRIDGE(lambda _opaque: callback())
        ok = bool(self._lib.hedgehog_backend_connect_virtual_timer(
            self._handle, ctypes.cast(bridge, ctypes.c_void_p), None, None,
        ))
        if ok:
            self._virtual_timer_bridge = bridge
        return ok

    def arm_virtual_timer(self, deadline_ns: int) -> bool:
        self._ensure_open()
        return bool(self._lib.hedgehog_backend_arm_virtual_timer(
            self._handle, ctypes.c_int64(deadline_ns),
        ))

    def virtual_clock_ns(self) -> int:
        self._ensure_open()
        if not hasattr(self._lib, 'hedgehog_backend_virtual_clock_ns'):
            raise RuntimeError('native backend does not expose its virtual clock')
        return int(self._lib.hedgehog_backend_virtual_clock_ns())

    def poll_events(self, block: bool) -> int:
        self._ensure_open()
        return int(
            self._lib.hedgehog_backend_poll_events(
                ctypes.c_bool(block),
                None,
            )
        )

    def arm_diagnostics(self) -> Dict[str, object]:
        """Return read-only ARM CPU diagnostic state."""
        self._ensure_open()
        if not hasattr(self._lib, 'hedgehog_backend_arm_diagnostics'):
            return {}
        diagnostics = _ArmDiagnostics()
        ok, detail = _call_bool_with_error(
            self._lib,
            self._lib.hedgehog_backend_arm_diagnostics,
            self._handle,
            ctypes.byref(diagnostics),
        )
        if not ok:
            return {}
        return {
            'pc': int(diagnostics.pc),
            'pstate': int(diagnostics.pstate),
            'scr_el3': int(diagnostics.scr_el3),
            'hcr_el2': int(diagnostics.hcr_el2),
            'hcr_el2_eff': int(diagnostics.hcr_el2_eff),
            'elr_el1': int(diagnostics.elr_el1),
            'elr_el2': int(diagnostics.elr_el2),
            'elr_el3': int(diagnostics.elr_el3),
            'spsr_el1': int(diagnostics.spsr_el1),
            'spsr_el2': int(diagnostics.spsr_el2),
            'spsr_el3': int(diagnostics.spsr_el3),
            'esr_el1': int(diagnostics.esr_el1),
            'esr_el2': int(diagnostics.esr_el2),
            'esr_el3': int(diagnostics.esr_el3),
            'far_el1': int(diagnostics.far_el1),
            'far_el2': int(diagnostics.far_el2),
            'far_el3': int(diagnostics.far_el3),
            'exception_syndrome': int(diagnostics.exception_syndrome),
            'exception_vaddress': int(diagnostics.exception_vaddress),
            'current_el': int(diagnostics.current_el),
            'cpsr': int(diagnostics.cpsr),
            'interrupt_request': int(diagnostics.interrupt_request),
            'halted': int(diagnostics.halted),
            'exit_request': int(diagnostics.exit_request),
            'exception_target_el': int(diagnostics.exception_target_el),
            'exception_index': int(diagnostics.exception_index),
            'generic_timer_count': int(diagnostics.generic_timer_count),
            'generic_timer_counter': int(diagnostics.generic_timer_counter),
            'generic_timer_cval': [int(value) for value in diagnostics.generic_timer_cval],
            'generic_timer_ctl': [int(value) for value in diagnostics.generic_timer_ctl],
            'generic_timer_enabled': [int(value) for value in diagnostics.generic_timer_enabled],
            'generic_timer_imask': [int(value) for value in diagnostics.generic_timer_imask],
            'generic_timer_istatus': [int(value) for value in diagnostics.generic_timer_istatus],
        }

    def restore_arm_generic_timers(
        self,
        virtual_clock_ns: int,
        cvals: Iterable[int],
        controls: Iterable[int],
    ) -> bool:
        """Restore native timer deadline/control state before direct running."""
        self._ensure_open()
        if not hasattr(self._lib, 'hedgehog_backend_restore_arm_generic_timers'):
            return False
        cval_values = tuple(int(value) for value in cvals)
        control_values = tuple(int(value) for value in controls)
        if len(cval_values) != 7 or len(control_values) != 7:
            raise ValueError('AArch64 generic-timer restore requires seven timer views')
        state = _ArmGenericTimerState()
        state.virtual_clock_ns = int(virtual_clock_ns)
        for index, value in enumerate(cval_values):
            state.generic_timer_cval[index] = value
        for index, value in enumerate(control_values):
            state.generic_timer_ctl[index] = value
        ok, _detail = _call_bool_with_error(
            self._lib,
            self._lib.hedgehog_backend_restore_arm_generic_timers,
            self._handle,
            ctypes.byref(state),
        )
        return ok

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


def _maybe_wrap_invalid_insn_hook(
    callback: Optional[InvalidInsnHookCallback],
) -> Optional[object]:
    if callback is None:
        return None

    def hook_bridge(
        _uc_ptr: int,
        info_ptr: ctypes.POINTER(_InvalidInstructionInfo),
        next_pc_ptr: ctypes.POINTER(ctypes.c_uint64),
        _opaque: int,
    ) -> int:
        try:
            info = info_ptr.contents
            instruction = bytes(info.bytes[:int(info.bytes_len)])
            disposition, next_pc = callback(
                int(info.pc),
                instruction,
                int(info.size),
                int(info.syndrome),
                int(info.exception_index),
            )
            disposition = int(disposition)
            if disposition == _INVALID_INSN_CONTINUE:
                next_pc_ptr[0] = int(next_pc)
            if disposition not in (
                _INVALID_INSN_PASS,
                _INVALID_INSN_CONTINUE,
                _INVALID_INSN_STOP,
            ):
                return _INVALID_INSN_STOP
            return disposition
        except BaseException:
            # ctypes callbacks must never let an exception cross the C ABI.
            return _INVALID_INSN_STOP

    return _INVALID_INSN_HOOK_BRIDGE(hook_bridge)


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

    if hasattr(lib, 'hedgehog_backend_connect_cpu_output'):
        lib.hedgehog_backend_connect_cpu_output.argtypes = [
            ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p,
            ctypes.c_void_p, error_ptr_t,
        ]
        lib.hedgehog_backend_connect_cpu_output.restype = ctypes.c_bool

    if hasattr(lib, 'hedgehog_backend_connect_system_call_observer'):
        lib.hedgehog_backend_connect_system_call_observer.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, error_ptr_t,
        ]
        lib.hedgehog_backend_connect_system_call_observer.restype = ctypes.c_bool

    if hasattr(lib, 'hedgehog_backend_connect_cpu_wait_observer'):
        lib.hedgehog_backend_connect_cpu_wait_observer.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, error_ptr_t,
        ]
        lib.hedgehog_backend_connect_cpu_wait_observer.restype = ctypes.c_bool

    if hasattr(lib, 'hedgehog_backend_connect_cpu_reset_observer'):
        lib.hedgehog_backend_connect_cpu_reset_observer.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, error_ptr_t,
        ]
        lib.hedgehog_backend_connect_cpu_reset_observer.restype = ctypes.c_bool

    if hasattr(lib, 'hedgehog_backend_request_cpu_reset'):
        lib.hedgehog_backend_request_cpu_reset.argtypes = [ctypes.c_void_p, error_ptr_t]
        lib.hedgehog_backend_request_cpu_reset.restype = ctypes.c_bool

    if hasattr(lib, 'hedgehog_backend_connect_virtual_timer'):
        lib.hedgehog_backend_connect_virtual_timer.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, error_ptr_t,
        ]
        lib.hedgehog_backend_connect_virtual_timer.restype = ctypes.c_bool
        lib.hedgehog_backend_arm_virtual_timer.argtypes = [ctypes.c_void_p, ctypes.c_int64]
        lib.hedgehog_backend_arm_virtual_timer.restype = ctypes.c_bool

    if hasattr(lib, 'hedgehog_backend_virtual_clock_ns'):
        lib.hedgehog_backend_virtual_clock_ns.argtypes = []
        lib.hedgehog_backend_virtual_clock_ns.restype = ctypes.c_int64

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

    lib.hedgehog_backend_map_ram_ptr.argtypes = [
        ctypes.c_void_p,
        ctypes.c_char_p,
        ctypes.c_uint64,
        ctypes.c_uint64,
        ctypes.c_void_p,
        error_ptr_t,
    ]
    lib.hedgehog_backend_map_ram_ptr.restype = ctypes.c_bool

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

    lib.hedgehog_backend_set_invalid_insn_hook.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
    ]
    lib.hedgehog_backend_set_invalid_insn_hook.restype = None

    lib.hedgehog_backend_set_invalid_mem_hook.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
    ]
    lib.hedgehog_backend_set_invalid_mem_hook.restype = None

    if hasattr(lib, 'hedgehog_backend_set_invalid_mem_diagnostic_capture'):
        lib.hedgehog_backend_set_invalid_mem_diagnostic_capture.argtypes = [
            ctypes.c_void_p,
            ctypes.c_bool,
            error_ptr_t,
        ]
        lib.hedgehog_backend_set_invalid_mem_diagnostic_capture.restype = ctypes.c_bool

    if hasattr(lib, 'hedgehog_backend_get_last_invalid_mem_diagnostic'):
        lib.hedgehog_backend_get_last_invalid_mem_diagnostic.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(_InvalidMemoryDiagnostic),
        ]
        lib.hedgehog_backend_get_last_invalid_mem_diagnostic.restype = ctypes.c_bool

    lib.hedgehog_backend_reset.argtypes = [ctypes.c_void_p]
    lib.hedgehog_backend_reset.restype = None

    lib.hedgehog_backend_reset_stop.argtypes = [ctypes.c_void_p]
    lib.hedgehog_backend_reset_stop.restype = None

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

    lib.hedgehog_backend_set_hard_interrupt.argtypes = [
        ctypes.c_void_p,
        ctypes.c_bool,
        error_ptr_t,
    ]
    lib.hedgehog_backend_set_hard_interrupt.restype = ctypes.c_bool

    if hasattr(lib, 'hedgehog_backend_set_aarch64_reset_state'):
        lib.hedgehog_backend_set_aarch64_reset_state.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint,
            ctypes.c_bool,
            error_ptr_t,
        ]
        lib.hedgehog_backend_set_aarch64_reset_state.restype = ctypes.c_bool

    if hasattr(lib, 'hedgehog_backend_add_aarch64_cp_reg'):
        lib.hedgehog_backend_add_aarch64_cp_reg.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint,
            ctypes.c_uint,
            ctypes.c_uint,
            ctypes.c_uint,
            ctypes.c_uint,
            ctypes.c_uint,
            ctypes.c_bool,
            ctypes.c_bool,
            ctypes.c_uint64,
            error_ptr_t,
        ]
        lib.hedgehog_backend_add_aarch64_cp_reg.restype = ctypes.c_bool

    if hasattr(lib, 'hedgehog_backend_set_aarch64_cp_reg_value'):
        lib.hedgehog_backend_set_aarch64_cp_reg_value.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint,
            ctypes.c_uint,
            ctypes.c_uint,
            ctypes.c_uint,
            ctypes.c_uint,
            ctypes.c_uint64,
            ctypes.c_uint64,
            error_ptr_t,
        ]
        lib.hedgehog_backend_set_aarch64_cp_reg_value.restype = ctypes.c_bool

    if hasattr(lib, 'hedgehog_backend_arm_diagnostics'):
        lib.hedgehog_backend_arm_diagnostics.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(_ArmDiagnostics),
            error_ptr_t,
        ]
        lib.hedgehog_backend_arm_diagnostics.restype = ctypes.c_bool

    if hasattr(lib, 'hedgehog_backend_restore_arm_generic_timers'):
        lib.hedgehog_backend_restore_arm_generic_timers.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(_ArmGenericTimerState),
            error_ptr_t,
        ]
        lib.hedgehog_backend_restore_arm_generic_timers.restype = ctypes.c_bool

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


__all__ = (
    'BackendProtocol',
    'ExecHookCallback',
    'InvalidInsnHookCallback',
    'InvalidHookCallback',
    'InvalidMemoryDiagnostic',
    'MMIOReadCallback',
    'MMIOWriteCallback',
    'NativeBackend',
)

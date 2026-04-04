"""
Hedgehog-style Python API on top of QEMU's in-tree backend API.
"""

# Copyright (C) 2026 Red Hat Inc.
#
# This work is licensed under the terms of the GNU GPL, version 2.  See
# the COPYING file in the top-level directory.

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Callable, Dict, Optional, Tuple, Union

from .backend import BackendProtocol, NativeBackend
from .constants import (
    QEMU_MEMTX_ACCESS_ERROR,
    QEMU_MEMTX_DECODE_ERROR,
    QEMU_MEMTX_OK,
    QEMU_HEDGEHOG_MEM_ACCESS_FETCH,
    QEMU_HEDGEHOG_MEM_ACCESS_READ,
    QEMU_HEDGEHOG_MEM_ACCESS_WRITE,
    QEMU_HEDGEHOG_RUN_EXCEPTION,
    QEMU_HEDGEHOG_RUN_INVALID_MEMORY,
    HEDGEHOG_ARCH_X86,
    HEDGEHOG_ERR_ARCH,
    HEDGEHOG_ERR_ARG,
    HEDGEHOG_ERR_EXCEPTION,
    HEDGEHOG_ERR_FETCH_PROT,
    HEDGEHOG_ERR_FETCH_UNMAPPED,
    HEDGEHOG_ERR_HOOK,
    HEDGEHOG_ERR_MAP,
    HEDGEHOG_ERR_READ_PROT,
    HEDGEHOG_ERR_READ_UNMAPPED,
    HEDGEHOG_ERR_RESOURCE,
    HEDGEHOG_ERR_WRITE_PROT,
    HEDGEHOG_ERR_WRITE_UNMAPPED,
    HEDGEHOG_HOOK_BLOCK,
    HEDGEHOG_HOOK_CODE,
    HEDGEHOG_HOOK_MEM_FETCH_UNMAPPED,
    HEDGEHOG_HOOK_MEM_INVALID,
    HEDGEHOG_HOOK_MEM_READ_UNMAPPED,
    HEDGEHOG_HOOK_MEM_WRITE_UNMAPPED,
    HEDGEHOG_MODE_16,
    HEDGEHOG_MODE_32,
    HEDGEHOG_MODE_64,
    HEDGEHOG_PROT_ALL,
)
from .errors import HedgehogError

HookCallback = Callable[..., Any]

_MEM_INVALID_MASK = (
    HEDGEHOG_HOOK_MEM_INVALID |
    HEDGEHOG_HOOK_MEM_READ_UNMAPPED |
    HEDGEHOG_HOOK_MEM_WRITE_UNMAPPED |
    HEDGEHOG_HOOK_MEM_FETCH_UNMAPPED
)

_SUPPORTED_HOOK_MASK = (
    HEDGEHOG_HOOK_BLOCK |
    HEDGEHOG_HOOK_CODE |
    _MEM_INVALID_MASK
)

_DEFAULT_CPU_TYPES = {
    (HEDGEHOG_ARCH_X86, HEDGEHOG_MODE_16): 'qemu64-x86_64-cpu',
    (HEDGEHOG_ARCH_X86, HEDGEHOG_MODE_32): 'qemu64-x86_64-cpu',
    (HEDGEHOG_ARCH_X86, HEDGEHOG_MODE_64): 'qemu64-x86_64-cpu',
    (HEDGEHOG_ARCH_X86, 0): 'qemu64-x86_64-cpu',
}


@dataclass
class _HookRegistration:
    handle: int
    hook_type: int
    callback: HookCallback
    user_data: object
    begin: int
    end: int


class Hedgehog:
    """
    Hedgehog-compatible emulator object.

    The constructor mirrors Hedgehog's ``Hedgehog(arch, mode)`` and accepts two
    optional keyword-only extensions:
    - ``cpu_type`` to override QEMU CPU type selection;
    - ``machine_type`` to choose the QEMU machine type used by the backend;
    - ``backend`` to inject a custom backend implementation.
    """

    def __init__(
        self,
        arch: int,
        mode: int,
        *,
        cpu_type: Optional[str] = None,
        machine_type: Optional[str] = None,
        backend: Optional[BackendProtocol] = None,
        library_path: Optional[str] = None,
    ):
        self.arch = int(arch)
        self.mode = int(mode)

        if backend is None:
            selected_cpu = cpu_type or _default_cpu_type(self.arch, self.mode)
            if selected_cpu is None:
                raise HedgehogError(
                    HEDGEHOG_ERR_ARCH,
                    'unsupported arch/mode; provide cpu_type explicitly',
                )
            backend = NativeBackend.create(
                selected_cpu,
                machine_type=machine_type,
                library_path=library_path,
            )

        self._backend = backend
        self._hooks: Dict[int, _HookRegistration] = {}
        self._next_hook_handle = 1
        self._pending_exception: Optional[BaseException] = None
        self._last_invalid_access: Optional[int] = None

    def close(self) -> None:
        """Close the backend and release resources."""
        self._backend.close()

    def __enter__(self) -> 'Hedgehog':
        return self

    def __exit__(
        self,
        exc_type: Optional[type],
        exc_val: Optional[BaseException],
        exc_tb: Optional[object],
    ) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def mem_map(self, address: int, size: int, perms: int = HEDGEHOG_PROT_ALL) -> None:
        """
        Map RAM into the guest address space.

        The in-tree C backend currently provides RAM mapping only; ``perms`` is
        validated for compatibility but not yet enforced per-page.
        """
        if address < 0 or size <= 0:
            raise HedgehogError(HEDGEHOG_ERR_ARG, 'address and size must be positive')
        if perms & ~HEDGEHOG_PROT_ALL:
            raise HedgehogError(HEDGEHOG_ERR_ARG, 'invalid memory protection flags')

        name = f'uc-ram-{address:x}'
        if not self._backend.map_ram(name, address, size):
            raise HedgehogError(HEDGEHOG_ERR_MAP, 'unable to map RAM region')

    def mem_map_mmio(
        self,
        address: int,
        size: int,
        read_callback: Callable[[int, int], int],
        write_callback: Callable[[int, int, int], None],
    ) -> None:
        """
        Map an MMIO callback range.
        """
        if address < 0 or size <= 0:
            raise HedgehogError(HEDGEHOG_ERR_ARG, 'address and size must be positive')
        if not callable(read_callback) or not callable(write_callback):
            raise HedgehogError(HEDGEHOG_ERR_ARG, 'read_callback/write_callback must be callable')

        name = f'uc-mmio-{address:x}'
        if not self._backend.map_mmio(
            name,
            address,
            size,
            read_callback,
            write_callback,
        ):
            raise HedgehogError(HEDGEHOG_ERR_MAP, 'unable to map MMIO region')

    def mem_read(self, address: int, size: int) -> bytes:
        """Read bytes from guest memory."""
        if address < 0 or size < 0:
            raise HedgehogError(HEDGEHOG_ERR_ARG, 'address and size must be non-negative')

        result, data = self._backend.mem_read(address, size)
        if result != QEMU_MEMTX_OK:
            raise _memtx_error(result, is_write=False, is_fetch=False)
        return data

    def mem_write(self, address: int, data: bytes) -> None:
        """Write bytes to guest memory."""
        if address < 0:
            raise HedgehogError(HEDGEHOG_ERR_ARG, 'address must be non-negative')

        payload = bytes(data)
        result = self._backend.mem_write(address, payload)
        if result != QEMU_MEMTX_OK:
            raise _memtx_error(result, is_write=True, is_fetch=False)

    def reg_read(self, reg_id: int, size: int = 64) -> int:
        """
        Read a register and return an unsigned little-endian integer.
        """
        raw = self.reg_read_bytes(reg_id, size=size)
        return int.from_bytes(raw, byteorder='little', signed=False)

    def reg_read_bytes(self, reg_id: int, size: int = 64) -> bytes:
        """
        Read raw register bytes.
        """
        if reg_id < 0 or size <= 0:
            raise HedgehogError(HEDGEHOG_ERR_ARG, 'invalid register id or size')

        raw = self._backend.reg_read(reg_id, size)
        if raw is None:
            raise HedgehogError(HEDGEHOG_ERR_ARG, f'failed to read register {reg_id}')
        return raw

    def reg_write(self, reg_id: int, value: Union[int, bytes, bytearray, memoryview]) -> None:
        """
        Write a register using either an integer value or raw bytes.
        """
        if reg_id < 0:
            raise HedgehogError(HEDGEHOG_ERR_ARG, 'register id must be non-negative')

        if isinstance(value, int):
            if value < 0:
                raise HedgehogError(HEDGEHOG_ERR_ARG, 'integer register values must be >= 0')
            nbytes = max(1, (value.bit_length() + 7) // 8)
            payload = value.to_bytes(nbytes, byteorder='little', signed=False)
        else:
            payload = bytes(value)
            if not payload:
                raise HedgehogError(HEDGEHOG_ERR_ARG, 'register byte payload cannot be empty')

        if not self._backend.reg_write(reg_id, payload):
            raise HedgehogError(HEDGEHOG_ERR_ARG, f'failed to write register {reg_id}')

    def hook_add(
        self,
        hook_type: int,
        callback: HookCallback,
        user_data: object = None,
        begin: int = 1,
        end: int = 0,
        arg1: int = 0,
    ) -> int:
        """
        Register a Hedgehog-style hook and return its handle.

        Supported hook families:
        - HEDGEHOG_HOOK_BLOCK
        - HEDGEHOG_HOOK_CODE
        - HEDGEHOG_HOOK_MEM_INVALID and unmapped memory subsets
        """
        del arg1

        if not callable(callback):
            raise HedgehogError(HEDGEHOG_ERR_ARG, 'callback must be callable')

        htype = int(hook_type)
        if htype == 0:
            raise HedgehogError(HEDGEHOG_ERR_HOOK, 'hook type cannot be zero')

        unsupported = htype & ~_SUPPORTED_HOOK_MASK
        if unsupported:
            raise HedgehogError(HEDGEHOG_ERR_HOOK, f'unsupported hook mask 0x{unsupported:x}')

        handle = self._next_hook_handle
        self._next_hook_handle += 1

        self._hooks[handle] = _HookRegistration(
            handle=handle,
            hook_type=htype,
            callback=callback,
            user_data=user_data,
            begin=int(begin),
            end=int(end),
        )
        self._sync_backend_hooks()
        return handle

    def hook_del(self, handle: int) -> None:
        """
        Remove a previously registered hook.
        """
        if int(handle) not in self._hooks:
            raise HedgehogError(HEDGEHOG_ERR_ARG, f'unknown hook handle {handle}')

        del self._hooks[int(handle)]
        self._sync_backend_hooks()

    def emu_start(
        self,
        begin: int,
        until: int,
        timeout: int = 0,
        count: int = 0,
    ) -> None:
        """
        Start emulation from begin to until (best-effort), optionally bounded.

        timeout is currently unsupported and must be zero.
        """
        if begin < 0 or until < 0 or timeout < 0 or count < 0:
            raise HedgehogError(HEDGEHOG_ERR_ARG, 'begin/until/timeout/count must be >= 0')
        if timeout != 0:
            raise HedgehogError(HEDGEHOG_ERR_RESOURCE, 'timeout-based execution is unsupported')

        self._pending_exception = None
        self._last_invalid_access = None
        self._backend.set_pc(begin)

        until_handle: Optional[int] = None
        if until != 0:
            until_handle = self.hook_add(
                HEDGEHOG_HOOK_CODE,
                lambda _uc, pc, _size, _user_data: bool(pc == until),
            )

        try:
            budget = int(count) if count > 0 else 0
            run_result, _cpu_exit = self._backend.run(budget)
        finally:
            if until_handle is not None and until_handle in self._hooks:
                self.hook_del(until_handle)

        if self._pending_exception is not None:
            exc = self._pending_exception
            self._pending_exception = None
            raise HedgehogError(HEDGEHOG_ERR_EXCEPTION, 'hook callback raised an exception') from exc

        if run_result == QEMU_HEDGEHOG_RUN_INVALID_MEMORY:
            raise self._invalid_mem_error()
        if run_result == QEMU_HEDGEHOG_RUN_EXCEPTION:
            raise HedgehogError(HEDGEHOG_ERR_EXCEPTION)

    def emu_stop(self) -> None:
        """
        Request emulation stop.
        """
        self._backend.stop()

    def qemu_set_pc(self, address: int) -> None:
        """QEMU-specific helper to set the guest PC."""
        self._backend.set_pc(address)

    def qemu_get_pc(self) -> int:
        """QEMU-specific helper to get the guest PC."""
        return self._backend.get_pc()

    def qemu_run(self, max_instructions: int = 0) -> Tuple[int, int]:
        """
        Run using the backend-native API and return raw status tuple.
        """
        return self._backend.run(max_instructions)

    def _sync_backend_hooks(self) -> None:
        has_tb = any(reg.hook_type & HEDGEHOG_HOOK_BLOCK for reg in self._hooks.values())
        has_insn = any(reg.hook_type & HEDGEHOG_HOOK_CODE for reg in self._hooks.values())
        has_invalid = any(reg.hook_type & _MEM_INVALID_MASK for reg in self._hooks.values())

        self._backend.set_tb_hook(self._dispatch_tb if has_tb else None)
        self._backend.set_insn_hook(self._dispatch_insn if has_insn else None)
        self._backend.set_invalid_mem_hook(
            self._dispatch_invalid_mem if has_invalid else None,
        )

    def _dispatch_tb(self, pc: int) -> bool:
        return self._dispatch_exec(HEDGEHOG_HOOK_BLOCK, pc)

    def _dispatch_insn(self, pc: int) -> bool:
        return self._dispatch_exec(HEDGEHOG_HOOK_CODE, pc)

    def _dispatch_exec(self, hook_family: int, pc: int) -> bool:
        should_stop = False

        for reg in tuple(self._hooks.values()):
            if not (reg.hook_type & hook_family):
                continue
            if not _address_in_range(pc, reg.begin, reg.end):
                continue

            try:
                ret = reg.callback(self, pc, 0, reg.user_data)
            except BaseException as err:
                self._pending_exception = err
                return True

            should_stop = should_stop or bool(ret)

        return should_stop

    def _dispatch_invalid_mem(
        self,
        addr: int,
        size: int,
        access_type: int,
        response: int,
    ) -> bool:
        self._last_invalid_access = int(access_type)

        access_hook = _access_type_to_hook(int(access_type))
        matched = False
        continue_exec = False

        for reg in tuple(self._hooks.values()):
            if not (reg.hook_type & _MEM_INVALID_MASK):
                continue
            if not (reg.hook_type & access_hook):
                continue
            if not _address_in_range(addr, reg.begin, reg.end):
                continue

            matched = True
            try:
                ret = reg.callback(self, access_type, addr, size, 0, reg.user_data)
            except BaseException as err:
                self._pending_exception = err
                return True

            continue_exec = continue_exec or bool(ret)

        if not matched:
            return False

        del response
        return not continue_exec

    def _invalid_mem_error(self) -> HedgehogError:
        if self._last_invalid_access == QEMU_HEDGEHOG_MEM_ACCESS_WRITE:
            return HedgehogError(HEDGEHOG_ERR_WRITE_UNMAPPED)
        if self._last_invalid_access == QEMU_HEDGEHOG_MEM_ACCESS_FETCH:
            return HedgehogError(HEDGEHOG_ERR_FETCH_UNMAPPED)
        return HedgehogError(HEDGEHOG_ERR_READ_UNMAPPED)


def _default_cpu_type(arch: int, mode: int) -> Optional[str]:
    if (arch, mode) in _DEFAULT_CPU_TYPES:
        return _DEFAULT_CPU_TYPES[(arch, mode)]

    if arch == HEDGEHOG_ARCH_X86 and mode & (HEDGEHOG_MODE_16 | HEDGEHOG_MODE_32 | HEDGEHOG_MODE_64):
        return 'qemu64-x86_64-cpu'

    return None


def _access_type_to_hook(access_type: int) -> int:
    if access_type == QEMU_HEDGEHOG_MEM_ACCESS_WRITE:
        return HEDGEHOG_HOOK_MEM_WRITE_UNMAPPED
    if access_type == QEMU_HEDGEHOG_MEM_ACCESS_FETCH:
        return HEDGEHOG_HOOK_MEM_FETCH_UNMAPPED
    return HEDGEHOG_HOOK_MEM_READ_UNMAPPED


def _address_in_range(address: int, begin: int, end: int) -> bool:
    if begin == 1 and end == 0:
        return True

    if end == 0:
        return address >= begin

    if begin <= end:
        return begin <= address <= end

    return address >= begin or address <= end


def _memtx_error(result: int, is_write: bool, is_fetch: bool) -> HedgehogError:
    if result & QEMU_MEMTX_ACCESS_ERROR:
        if is_write:
            return HedgehogError(HEDGEHOG_ERR_WRITE_PROT)
        if is_fetch:
            return HedgehogError(HEDGEHOG_ERR_FETCH_PROT)
        return HedgehogError(HEDGEHOG_ERR_READ_PROT)

    if result & QEMU_MEMTX_DECODE_ERROR:
        if is_write:
            return HedgehogError(HEDGEHOG_ERR_WRITE_UNMAPPED)
        if is_fetch:
            return HedgehogError(HEDGEHOG_ERR_FETCH_UNMAPPED)
        return HedgehogError(HEDGEHOG_ERR_READ_UNMAPPED)

    if is_write:
        return HedgehogError(HEDGEHOG_ERR_WRITE_UNMAPPED)
    if is_fetch:
        return HedgehogError(HEDGEHOG_ERR_FETCH_UNMAPPED)
    return HedgehogError(HEDGEHOG_ERR_READ_UNMAPPED)


__all__ = (
    'Hedgehog',
)

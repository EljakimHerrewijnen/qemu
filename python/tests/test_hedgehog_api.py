# SPDX-License-Identifier: GPL-2.0-or-later

from typing import Callable, Dict, List, Optional, Tuple

import pytest

from qemu.hedgehog import Hedgehog, HedgehogError
from qemu.hedgehog.constants import (
    QEMU_MEMTX_DECODE_ERROR,
    QEMU_MEMTX_OK,
    QEMU_HEDGEHOG_MEM_ACCESS_READ,
    QEMU_HEDGEHOG_RUN_BUDGET_EXHAUSTED,
    QEMU_HEDGEHOG_RUN_INVALID_MEMORY,
    QEMU_HEDGEHOG_RUN_STOP_REQUESTED,
    HEDGEHOG_ARCH_X86,
    HEDGEHOG_ERR_READ_UNMAPPED,
    HEDGEHOG_HOOK_CODE,
    HEDGEHOG_HOOK_MEM_READ_UNMAPPED,
    HEDGEHOG_MODE_64,
)


class FakeBackend:
    def __init__(self):
        self._mapped: List[Tuple[int, int]] = []
        self._mem: Dict[int, int] = {}
        self._regs: Dict[int, bytes] = {}

        self.exec_sequence: List[int] = []
        self.invalid_event: Optional[Tuple[int, int, int, int]] = None
        self.run_result = QEMU_HEDGEHOG_RUN_BUDGET_EXHAUSTED

        self.tb_hook: Optional[Callable[[int], bool]] = None
        self.insn_hook: Optional[Callable[[int], bool]] = None
        self.invalid_hook: Optional[Callable[[int, int, int, int], bool]] = None

        self.stopped = False
        self.pc = 0

    def close(self) -> None:
        return

    def map_ram(self, name: str, addr: int, size: int) -> bool:
        del name
        self._mapped.append((addr, size))
        return True

    def map_mmio(
        self,
        name: str,
        addr: int,
        size: int,
        read_fn: Callable[[int, int], int],
        write_fn: Callable[[int, int, int], None],
    ) -> bool:
        del name
        del addr
        del size
        del read_fn
        del write_fn
        return True

    def mem_read(self, addr: int, size: int) -> Tuple[int, bytes]:
        if not self._is_mapped(addr, size):
            return QEMU_MEMTX_DECODE_ERROR, b''

        data = bytes(self._mem.get(addr + idx, 0) for idx in range(size))
        return QEMU_MEMTX_OK, data

    def mem_write(self, addr: int, data: bytes) -> int:
        if not self._is_mapped(addr, len(data)):
            return QEMU_MEMTX_DECODE_ERROR

        for idx, value in enumerate(data):
            self._mem[addr + idx] = value
        return QEMU_MEMTX_OK

    def reg_read(self, regno: int, buf_size: int) -> Optional[bytes]:
        if regno < 0:
            return None

        raw = self._regs.get(regno, b'\x00')
        return raw[:buf_size]

    def reg_write(self, regno: int, data: bytes) -> bool:
        if regno < 0:
            return False
        self._regs[regno] = bytes(data)
        return True

    def set_tb_hook(self, callback: Optional[Callable[[int], bool]]) -> None:
        self.tb_hook = callback

    def set_insn_hook(self, callback: Optional[Callable[[int], bool]]) -> None:
        self.insn_hook = callback

    def set_invalid_mem_hook(
        self,
        callback: Optional[Callable[[int, int, int, int], bool]],
    ) -> None:
        self.invalid_hook = callback

    def reset(self) -> None:
        return

    def set_pc(self, addr: int) -> None:
        self.pc = addr

    def get_pc(self) -> int:
        return self.pc

    def run(self, max_instructions: int) -> Tuple[int, int]:
        del max_instructions

        if self.invalid_event is not None and self.invalid_hook is not None:
            stop = self.invalid_hook(*self.invalid_event)
            if stop:
                return QEMU_HEDGEHOG_RUN_INVALID_MEMORY, 0

        for pc in self.exec_sequence:
            if self.tb_hook is not None and self.tb_hook(pc):
                return QEMU_HEDGEHOG_RUN_STOP_REQUESTED, 0
            if self.insn_hook is not None and self.insn_hook(pc):
                return QEMU_HEDGEHOG_RUN_STOP_REQUESTED, 0

        if self.stopped:
            self.stopped = False
            return QEMU_HEDGEHOG_RUN_STOP_REQUESTED, 0

        return self.run_result, 0

    def stop(self) -> None:
        self.stopped = True

    def _is_mapped(self, addr: int, size: int) -> bool:
        for base, region_size in self._mapped:
            if addr >= base and (addr + size) <= (base + region_size):
                return True
        return False


def test_mem_map_read_write_roundtrip() -> None:
    backend = FakeBackend()
    uc = Hedgehog(HEDGEHOG_ARCH_X86, HEDGEHOG_MODE_64, backend=backend)

    uc.mem_map(0x1000, 0x1000)
    uc.mem_write(0x1010, b'QEMU')

    assert uc.mem_read(0x1010, 4) == b'QEMU'


def test_code_hook_dispatch_and_until_stop() -> None:
    backend = FakeBackend()
    backend.exec_sequence = [0x2000, 0x2004, 0x2008]

    uc = Hedgehog(HEDGEHOG_ARCH_X86, HEDGEHOG_MODE_64, backend=backend)
    seen: List[int] = []

    def code_hook(_uc: Hedgehog, address: int, _size: int, _user_data: object) -> bool:
        seen.append(address)
        return False

    uc.hook_add(HEDGEHOG_HOOK_CODE, code_hook)
    uc.emu_start(0x2000, 0x2004)

    assert seen == [0x2000, 0x2004]


def test_invalid_mem_raises_ucerror() -> None:
    backend = FakeBackend()
    backend.invalid_event = (0x3000, 4, QEMU_HEDGEHOG_MEM_ACCESS_READ, 0)

    uc = Hedgehog(HEDGEHOG_ARCH_X86, HEDGEHOG_MODE_64, backend=backend)

    def invalid_hook(
        _uc: Hedgehog,
        _access: int,
        _address: int,
        _size: int,
        _value: int,
        _user_data: object,
    ) -> bool:
        return False

    uc.hook_add(HEDGEHOG_HOOK_MEM_READ_UNMAPPED, invalid_hook)

    with pytest.raises(HedgehogError) as exc_info:
        uc.emu_start(0x3000, 0)

    assert exc_info.value.errno == HEDGEHOG_ERR_READ_UNMAPPED


def test_invalid_mem_hook_can_continue() -> None:
    backend = FakeBackend()
    backend.invalid_event = (0x4000, 4, QEMU_HEDGEHOG_MEM_ACCESS_READ, 0)

    uc = Hedgehog(HEDGEHOG_ARCH_X86, HEDGEHOG_MODE_64, backend=backend)

    def invalid_hook(
        _uc: Hedgehog,
        _access: int,
        _address: int,
        _size: int,
        _value: int,
        _user_data: object,
    ) -> bool:
        return True

    uc.hook_add(HEDGEHOG_HOOK_MEM_READ_UNMAPPED, invalid_hook)

    uc.emu_start(0x4000, 0)

# SPDX-License-Identifier: GPL-2.0-or-later

from types import SimpleNamespace
from typing import Callable, Dict, List, Optional, Tuple, cast

import pytest

from qemu.hedgehog import Hedgehog, HedgehogError
from qemu.hedgehog.backend import NativeBackend, _maybe_wrap_invalid_hook
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
        self.chardevs: Dict[str, str] = {}
        self.property_bindings: List[Tuple[str, str, str]] = []
        self.serial_backends: Dict[int, str] = {}
        self.poll_calls: List[bool] = []

        self.exec_sequence: List[int] = []
        self.invalid_event: Optional[Tuple[int, int, int, int]] = None
        self.run_result = QEMU_HEDGEHOG_RUN_BUDGET_EXHAUSTED
        self.run_budgets: List[int] = []
        self.unbounded_exec_requires_budget_for_hooks = False
        self._exec_sequence_exhausted = False
        self._last_exec_sequence: List[int] = []

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
        self.run_budgets.append(max_instructions)

        # Reset exhausted state if the exec_sequence has changed
        if self.exec_sequence != self._last_exec_sequence:
            self._exec_sequence_exhausted = False
            self._last_exec_sequence = list(self.exec_sequence)

        if self.invalid_event is not None and self.invalid_hook is not None:
            stop = self.invalid_hook(*self.invalid_event)
            if stop:
                return QEMU_HEDGEHOG_RUN_INVALID_MEMORY, 0

        # For unbounded execution with hooks: execute sequence on first call only,
        # but skip if max_instructions == 0 (caller should use non-zero budget)
        should_exec_sequence = True
        if max_instructions == 0 and self.unbounded_exec_requires_budget_for_hooks:
            should_exec_sequence = False
        elif self.unbounded_exec_requires_budget_for_hooks and self._exec_sequence_exhausted:
            should_exec_sequence = False

        if should_exec_sequence:
            for pc in self.exec_sequence:
                if self.tb_hook is not None and self.tb_hook(pc):
                    return QEMU_HEDGEHOG_RUN_STOP_REQUESTED, 0
                if self.insn_hook is not None and self.insn_hook(pc):
                    return QEMU_HEDGEHOG_RUN_STOP_REQUESTED, 0
            # Mark sequence as exhausted after processing
            if self.unbounded_exec_requires_budget_for_hooks:
                self._exec_sequence_exhausted = True

        if self.stopped:
            self.stopped = False
            return QEMU_HEDGEHOG_RUN_STOP_REQUESTED, 0

        # If sequence is exhausted and we would return BUDGET_EXHAUSTED,
        # return the final result instead to break the chunked execution loop
        if self.unbounded_exec_requires_budget_for_hooks and self._exec_sequence_exhausted:
            if self.run_result == QEMU_HEDGEHOG_RUN_BUDGET_EXHAUSTED:
                return QEMU_HEDGEHOG_RUN_STOP_REQUESTED, 0

        return self.run_result, 0

    def stop(self) -> None:
        self.stopped = True

    def add_chardev(self, chardev_id: str, uri: str) -> bool:
        self.chardevs[chardev_id] = uri
        return True

    def bind_property(self, object_path: str, property_name: str, value: str) -> bool:
        self.property_bindings.append((object_path, property_name, value))
        return True

    def attach_serial_chardev(self, index: int, chardev_id: str) -> bool:
        self.serial_backends[index] = chardev_id
        return True

    def get_chardev_endpoint(self, chardev_id: str) -> Optional[str]:
        if chardev_id not in self.chardevs:
            return None
        return f'endpoint:{chardev_id}'

    def poll_events(self, block: bool) -> int:
        self.poll_calls.append(block)
        return 1

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


def test_code_hook_dispatch_with_unbounded_run_uses_chunked_budget() -> None:
    backend = FakeBackend()
    backend.exec_sequence = [0x1000, 0x1004, 0x1008, 0x100c]
    backend.unbounded_exec_requires_budget_for_hooks = True

    uc = Hedgehog(HEDGEHOG_ARCH_X86, HEDGEHOG_MODE_64, backend=backend)
    seen: List[int] = []

    def code_hook(_uc: Hedgehog, address: int, _size: int, _user_data: object) -> bool:
        seen.append(address)
        return len(seen) >= 3

    uc.hook_add(HEDGEHOG_HOOK_CODE, code_hook, begin=0x1000, end=0x1010)
    uc.emu_start(0x1000, 0)

    assert seen == [0x1000, 0x1004, 0x1008]
    assert backend.run_budgets
    assert all(budget > 0 for budget in backend.run_budgets)


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


def test_invalid_mem_bridge_forwards_response_argument() -> None:
    seen: List[Tuple[int, int, int, int]] = []

    def invalid_hook(addr: int, size: int, access_type: int, response: int) -> bool:
        seen.append((addr, size, access_type, response))
        return True

    bridge = _maybe_wrap_invalid_hook(invalid_hook)
    assert bridge is not None

    result = cast(Callable[..., bool], bridge)(0, 0x1234, 8, 2, 99, 0)

    assert result is True
    assert seen == [(0x1234, 8, 2, 99)]


def test_coverage_block_mode_collects_unique_blocks() -> None:
    backend = FakeBackend()
    backend.exec_sequence = [0x1000, 0x1004, 0x1000]
    backend.unbounded_exec_requires_budget_for_hooks = True

    uc = Hedgehog(HEDGEHOG_ARCH_X86, HEDGEHOG_MODE_64, backend=backend, coverage='block')
    uc.emu_start(0x1000, 0)

    cov = uc.get_coverage()
    assert cov['modes'] == ('block',)
    assert cov['blocks'] == {0x1000, 0x1004}
    assert cov['unique_blocks'] == 2


def test_coverage_insn_mode_collects_unique_instructions() -> None:
    backend = FakeBackend()
    backend.exec_sequence = [0x2000, 0x2004, 0x2008, 0x2004]
    backend.unbounded_exec_requires_budget_for_hooks = True

    uc = Hedgehog(HEDGEHOG_ARCH_X86, HEDGEHOG_MODE_64, backend=backend, coverage='insn')
    uc.emu_start(0x2000, 0)

    cov = uc.get_coverage()
    assert cov['modes'] == ('insn',)
    assert cov['insn'] == {0x2000, 0x2004, 0x2008}
    assert cov['unique_insn'] == 3


def test_coverage_digest_and_edge_digest_modes() -> None:
    backend = FakeBackend()
    backend.exec_sequence = [0x3000, 0x3004, 0x3008]
    backend.unbounded_exec_requires_budget_for_hooks = True

    uc = Hedgehog(
        HEDGEHOG_ARCH_X86,
        HEDGEHOG_MODE_64,
        backend=backend,
        coverage=('block', 'digest', 'edge_digest'),
    )
    uc.emu_start(0x3000, 0)

    cov = uc.get_coverage()
    assert cov['unique_blocks'] == 3
    assert cov['unique_edges'] == 2
    assert isinstance(cov['coverage_digest'], str)
    assert isinstance(cov['edge_digest'], str)
    assert len(cov['coverage_digest']) == 32
    assert len(cov['edge_digest']) == 32

    old_cov_digest = uc.get_coverage_digest()
    old_edge_digest = uc.get_edge_digest()

    backend.exec_sequence = [0x3000, 0x3004, 0x3008, 0x3010]
    uc.emu_start(0x3000, 0)

    assert uc.get_coverage_digest() != old_cov_digest
    assert uc.get_edge_digest() != old_edge_digest


def test_clear_coverage_resets_collected_data() -> None:
    backend = FakeBackend()
    backend.exec_sequence = [0x4000, 0x4004]
    backend.unbounded_exec_requires_budget_for_hooks = True

    uc = Hedgehog(
        HEDGEHOG_ARCH_X86,
        HEDGEHOG_MODE_64,
        backend=backend,
        coverage=('block', 'digest', 'edge_digest'),
    )
    uc.emu_start(0x4000, 0)
    assert uc.get_coverage()['unique_blocks'] == 2

    uc.clear_coverage()
    cov = uc.get_coverage()
    assert cov['unique_blocks'] == 0
    assert cov['unique_edges'] == 0


def test_reset_coverage_alias_resets_collected_data() -> None:
    backend = FakeBackend()
    backend.exec_sequence = [0x5000, 0x5004]
    backend.unbounded_exec_requires_budget_for_hooks = True

    uc = Hedgehog(
        HEDGEHOG_ARCH_X86,
        HEDGEHOG_MODE_64,
        backend=backend,
        coverage=('block', 'digest', 'edge_digest'),
    )
    uc.emu_start(0x5000, 0)
    assert uc.get_coverage()['unique_blocks'] == 2

    uc.reset_coverage()
    cov = uc.get_coverage()
    assert cov['unique_blocks'] == 0
    assert cov['unique_edges'] == 0


def test_invalid_coverage_mode_raises() -> None:
    backend = FakeBackend()

    with pytest.raises(HedgehogError):
        Hedgehog(HEDGEHOG_ARCH_X86, HEDGEHOG_MODE_64, backend=backend, coverage='nope')


def test_machine_type_is_forwarded(monkeypatch: pytest.MonkeyPatch) -> None:
    captured: Dict[str, object] = {}
    backend = FakeBackend()

    def fake_create(
        cpu_type: str,
        machine_type: Optional[str] = None,
        library_path: Optional[str] = None,
        chardevs: Optional[Dict[str, str]] = None,
        property_bindings: Optional[Dict[str, Dict[str, str]]] = None,
        serial_backends: Optional[Dict[int, str]] = None,
    ) -> FakeBackend:
        captured['cpu_type'] = cpu_type
        captured['machine_type'] = machine_type
        captured['library_path'] = library_path
        captured['chardevs'] = chardevs
        captured['property_bindings'] = property_bindings
        captured['serial_backends'] = serial_backends
        return backend

    monkeypatch.setattr('qemu.hedgehog.api.NativeBackend.create', fake_create)

    uc = Hedgehog(
        HEDGEHOG_ARCH_X86,
        HEDGEHOG_MODE_64,
        machine_type='none',
    )

    assert captured['machine_type'] == 'none'
    uc.close()


def test_native_create_forwards_chardev_configuration(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    captured: Dict[str, object] = {}
    backend = FakeBackend()

    def fake_create(
        cpu_type: str,
        machine_type: Optional[str] = None,
        library_path: Optional[str] = None,
        chardevs: Optional[Dict[str, str]] = None,
        property_bindings: Optional[Dict[str, Dict[str, str]]] = None,
        serial_backends: Optional[Dict[int, str]] = None,
    ) -> FakeBackend:
        captured['cpu_type'] = cpu_type
        captured['machine_type'] = machine_type
        captured['library_path'] = library_path
        captured['chardevs'] = chardevs
        captured['property_bindings'] = property_bindings
        captured['serial_backends'] = serial_backends
        return backend

    monkeypatch.setattr('qemu.hedgehog.api.NativeBackend.create', fake_create)

    uc = Hedgehog(
        HEDGEHOG_ARCH_X86,
        HEDGEHOG_MODE_64,
        machine_type='none',
        chardevs={'console': 'pty'},
        property_bindings={'/machine/uart0': {'chardev': 'console'}},
        serial_backends={0: 'console'},
    )

    assert captured['chardevs'] == {'console': 'pty'}
    assert captured['property_bindings'] == {'/machine/uart0': {'chardev': 'console'}}
    assert captured['serial_backends'] == {0: 'console'}
    assert backend.chardevs == {}
    assert backend.property_bindings == []
    assert backend.serial_backends == {}
    uc.close()


def test_injected_backend_applies_chardev_configuration() -> None:
    backend = FakeBackend()

    uc = Hedgehog(
        HEDGEHOG_ARCH_X86,
        HEDGEHOG_MODE_64,
        backend=backend,
        chardevs={'console': 'pty'},
        property_bindings={'/machine/uart0': {'chardev': 'console'}},
        serial_backends={0: 'console'},
    )

    assert backend.chardevs == {'console': 'pty'}
    assert backend.property_bindings == [('/machine/uart0', 'chardev', 'console')]
    assert backend.serial_backends == {0: 'console'}
    uc.close()


def test_qemu_chardev_helpers_delegate_to_backend() -> None:
    backend = FakeBackend()
    uc = Hedgehog(HEDGEHOG_ARCH_X86, HEDGEHOG_MODE_64, backend=backend)

    uc.qemu_chardev_add('console', 'pty')
    uc.qemu_property_bind('/machine/uart0', 'chardev', 'console')
    uc.qemu_chardev_bind('/machine/uart1', 'chardev', 'console')
    uc.qemu_chardev_attach_serial(0, 'console')

    assert uc.qemu_chardev_get_endpoint('console') == 'endpoint:console'
    assert backend.property_bindings == [
        ('/machine/uart0', 'chardev', 'console'),
        ('/machine/uart1', 'chardev', 'console'),
    ]
    assert uc.qemu_events_poll() == 1
    assert uc.qemu_events_poll(block=True) == 1
    assert backend.poll_calls == [False, True]
    uc.close()


class _FakeNativeFunction:
    def __init__(self, return_value: object = None):
        self.return_value = return_value
        self.calls: List[Tuple[object, ...]] = []
        self.argtypes = None
        self.restype = None

    def __call__(self, *args: object) -> object:
        self.calls.append(args)
        return self.return_value


def _make_fake_native_library() -> SimpleNamespace:
    return SimpleNamespace(
        hedgehog_backend_initialize=_FakeNativeFunction(True),
        hedgehog_backend_new=_FakeNativeFunction(0x1234),
        hedgehog_backend_chardev_add=_FakeNativeFunction(True),
        hedgehog_backend_bind_property=_FakeNativeFunction(True),
        hedgehog_backend_chardev_attach_serial=_FakeNativeFunction(True),
        hedgehog_backend_set_tb_hook=_FakeNativeFunction(None),
        hedgehog_backend_set_insn_hook=_FakeNativeFunction(None),
        hedgehog_backend_set_invalid_mem_hook=_FakeNativeFunction(None),
        hedgehog_backend_free=_FakeNativeFunction(None),
        _name='fake-hedgehog-backend.so',
    )


def test_native_backend_rejects_second_process_local_create(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    fake_lib = _make_fake_native_library()

    monkeypatch.setattr('qemu.hedgehog.backend._NATIVE_BACKEND_PROCESS_SINGLETON', None)
    monkeypatch.setattr('qemu.hedgehog.backend._load_native_library', lambda _path: fake_lib)
    monkeypatch.setattr('qemu.hedgehog.backend._configure_library_api', lambda _lib: None)
    monkeypatch.setattr(
        'qemu.hedgehog.backend._call_bool_with_error',
        lambda _lib, func, *args: (bool(func(*args)), None),
    )
    monkeypatch.setattr(
        'qemu.hedgehog.backend._call_pointer_with_error',
        lambda _lib, func, *args: (int(func(*args)), None),
    )

    backend = NativeBackend.create('qemu64-x86_64-cpu')

    with pytest.raises(HedgehogError, match='only be initialized once per process'):
        NativeBackend.create('qemu64-x86_64-cpu')

    backend.close()


def test_native_backend_close_detaches_callbacks_without_freeing() -> None:
    fake_lib = _make_fake_native_library()
    backend = NativeBackend(fake_lib, 0x1234)

    backend.close()

    assert fake_lib.hedgehog_backend_free.calls == []
    assert len(fake_lib.hedgehog_backend_set_tb_hook.calls) == 1
    assert len(fake_lib.hedgehog_backend_set_insn_hook.calls) == 1
    assert len(fake_lib.hedgehog_backend_set_invalid_mem_hook.calls) == 1


def test_native_backend_operations_fail_after_close() -> None:
    fake_lib = _make_fake_native_library()
    backend = NativeBackend(fake_lib, 0x1234)

    backend.close()

    with pytest.raises(HedgehogError, match='has been closed'):
        backend.set_pc(0x1000)

# SPDX-License-Identifier: GPL-2.0-or-later

import os
import subprocess
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]


def _backend_library() -> Path:
    configured = os.environ.get('QEMU_HEDGEHOG_BACKEND_LIBRARY')
    if configured:
        return Path(configured)
    return ROOT / 'build' / 'libqemu-hedgehog-backend.so'


def test_standalone_virtual_clock_freezes_outside_direct_runs() -> None:
    backend_library = _backend_library()
    if not backend_library.exists():
        pytest.skip(f'Hedgehog backend library not found: {backend_library}')

    script = r'''
import ctypes
import os
import struct
import threading
import time
from qemu.hedgehog import Hedgehog, HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM
from qemu.hedgehog.constants import (
    QEMU_HEDGEHOG_RUN_BUDGET_EXHAUSTED, QEMU_HEDGEHOG_RUN_HALTED,
    QEMU_HEDGEHOG_RUN_STOP_REQUESTED,
)

emu = Hedgehog(HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM, cpu_type='cortex-a55')
emu.qemu_set_aarch64_reset_state(current_el=3, secure=True)
lib = ctypes.CDLL(os.environ['QEMU_HEDGEHOG_BACKEND_LIBRARY'])
lib.qemu_clock_get_ns.argtypes = [ctypes.c_int]
lib.qemu_clock_get_ns.restype = ctypes.c_int64

def frozen():
    before = lib.qemu_clock_get_ns(1)  # QEMU_CLOCK_VIRTUAL
    time.sleep(0.03)
    assert lib.qemu_clock_get_ns(1) == before
    return before

frozen()  # Construction must not start guest time.
emu.mem_map(0x1000, 0x1000)
emu.mem_write(0x1000, struct.pack('<3I', 0xd53be020, 0xd53be021, 0x14000000))
emu.mem_write(0x1100, struct.pack('<I', 0xd503207f))  # wfi
emu.qemu_set_pc(0x1000)
before = frozen()
assert emu.qemu_run(2)[0] == QEMU_HEDGEHOG_RUN_BUDGET_EXHAUSTED
assert emu.reg_read(1, size=8) > emu.reg_read(0, size=8)
assert frozen() > before

# Host timeout stays asynchronous and independent of the frozen guest clock.
emu.qemu_reset_stop()
timer = threading.Timer(0.03, emu.emu_stop)
timer.start()
assert emu.qemu_run(0)[0] == QEMU_HEDGEHOG_RUN_STOP_REQUESTED
timer.join()
frozen()

# A stop requested before entry must not start the clock or lose its state.
before = frozen()
assert emu.qemu_run(0)[0] == QEMU_HEDGEHOG_RUN_STOP_REQUESTED
assert frozen() == before

emu.qemu_reset_stop()
emu.qemu_set_pc(0x1100)
assert emu.qemu_run(1)[0] == QEMU_HEDGEHOG_RUN_HALTED
frozen()
emu.close()
'''
    env = os.environ.copy()
    env['PYTHONPATH'] = str(ROOT / 'python')
    env['QEMU_HEDGEHOG_BACKEND_LIBRARY'] = str(backend_library)
    result = subprocess.run(
        [sys.executable, '-c', script], cwd=ROOT, env=env,
        text=True, capture_output=True, check=False, timeout=15,
    )
    assert result.returncode == 0, result.stdout + result.stderr


def test_aarch64_cpu_wait_observer_can_queue_architectural_reset() -> None:
    backend_library = _backend_library()
    if not backend_library.exists():
        pytest.skip(f'Hedgehog backend library not found: {backend_library}')

    script = r'''
from qemu.hedgehog import Hedgehog, HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM
from qemu.hedgehog.constants import QEMU_HEDGEHOG_RUN_STOP_REQUESTED

emu = Hedgehog(HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM, cpu_type='cortex-a55')
emu.qemu_set_aarch64_reset_state(current_el=3, secure=True)
emu.mem_map(0, 0x2000)
emu.mem_write(0, bytes.fromhex('7f2003d5'))  # wfi
emu.mem_write(0x1000, bytes.fromhex('7f2003d5'))  # first wfi before reset
emu.qemu_set_pc(0x1000)
seen = []
reset_seen = []

def observe_wait(event):
    seen.append((event.pc, event.kind, event.current_el))
    if len(seen) == 1:
        emu.qemu_request_cpu_reset()
    else:
        emu.emu_stop()

emu.qemu_connect_cpu_wait_observer(observe_wait)
emu.qemu_connect_cpu_reset_observer(
    lambda: reset_seen.append(emu.qemu_arm_diagnostics()['pc'])
)
result, _cpu_exit = emu.qemu_run(0)
assert result == QEMU_HEDGEHOG_RUN_STOP_REQUESTED
# The target reports the next architectural PC after accepting WFI. The first
# wait came from 0x1000, the second from the reset vector at 0x0.
assert seen == [(0x1004, 'wfi', 3), (4, 'wfi', 3)]
assert reset_seen == [0]
emu.close()
'''
    env = os.environ.copy()
    env['PYTHONPATH'] = str(ROOT / 'python')
    env['QEMU_HEDGEHOG_BACKEND_LIBRARY'] = str(backend_library)
    result = subprocess.run(
        [sys.executable, '-c', script], cwd=ROOT, env=env,
        text=True, capture_output=True, check=False, timeout=15,
    )
    assert result.returncode == 0, result.stdout + result.stderr


def test_aarch64_invalid_instruction_hook_emulates_and_continues() -> None:
    backend_library = _backend_library()
    if not backend_library.exists():
        pytest.skip(f'Hedgehog backend library not found: {backend_library}')

    script = r'''
from qemu.hedgehog import Hedgehog, HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM
from qemu.hedgehog.constants import QEMU_HEDGEHOG_RUN_BUDGET_EXHAUSTED

code = bytes.fromhex(
    '00000000'  # permanently undefined in A64
    '01040091'  # add x1, x0, #1
    '1f2003d5'  # nop
)

emu = Hedgehog(HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM, cpu_type='max')
emu.qemu_set_aarch64_reset_state(current_el=3, secure=True)
emu.mem_map(0, 0x1000)
emu.mem_write(0, code)
seen = []

def emulate_invalid(uc, instruction, _user_data):
    seen.append(instruction)
    assert instruction.pc == 0
    assert instruction.data == b'\x00\x00\x00\x00'
    assert instruction.size == 4
    uc.reg_write(0, 0x41)
    return True

emu.hook_invalid_instruction(emulate_invalid)
emu.emu_start(0, 0, count=2)

assert len(seen) == 1
assert emu.reg_read(1) == 0x42
assert emu.qemu_get_pc() == 8
'''
    env = os.environ.copy()
    env['PYTHONPATH'] = str(ROOT / 'python')
    env['QEMU_HEDGEHOG_BACKEND_LIBRARY'] = str(backend_library)

    result = subprocess.run(
        [sys.executable, '-c', script],
        cwd=ROOT,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stdout + result.stderr


def test_aarch64_invalid_instruction_hook_can_preserve_guest_exception() -> None:
    backend_library = _backend_library()
    if not backend_library.exists():
        pytest.skip(f'Hedgehog backend library not found: {backend_library}')

    script = r'''
from qemu.hedgehog import Hedgehog, HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM

emu = Hedgehog(HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM, cpu_type='max')
emu.qemu_set_aarch64_reset_state(current_el=3, secure=True)
emu.mem_map(0, 0x1000)
emu.mem_write(0, bytes.fromhex('00000000'))
seen = []

def pass_through(_emu, instruction, _user_data):
    seen.append(instruction)
    return False

emu.hook_invalid_instruction(pass_through)
emu.emu_start(0, 0, count=1)

assert len(seen) == 1
assert emu.qemu_get_pc() == 0x200
assert emu.qemu_arm_diagnostics()['esr_el3'] >> 26 == 0
'''
    env = os.environ.copy()
    env['PYTHONPATH'] = str(ROOT / 'python')
    env['QEMU_HEDGEHOG_BACKEND_LIBRARY'] = str(backend_library)

    result = subprocess.run(
        [sys.executable, '-c', script],
        cwd=ROOT,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stdout + result.stderr


def test_aarch64_invalid_memory_diagnostic_reports_tlb_and_region_metadata() -> None:
    backend_library = _backend_library()
    if not backend_library.exists():
        pytest.skip(f'Hedgehog backend library not found: {backend_library}')

    script = r'''
import struct
from qemu.hedgehog import Hedgehog, HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM
from qemu.hedgehog.constants import (
    HEDGEHOG_HOOK_MEM_INVALID,
    QEMU_HEDGEHOG_MEM_ACCESS_READ,
    QEMU_HEDGEHOG_RUN_INVALID_MEMORY,
    QEMU_MEMTX_DECODE_ERROR,
)

# A canonical high address with no matching root address-space mapping.  QEMU
# reaches cputlb.io_failed() with a full TLB entry for the unassigned region.
TARGET_VA = 0xffffff8012345010

emu = Hedgehog(HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM, cpu_type='cortex-a55')
emu.qemu_set_aarch64_reset_state(current_el=1, secure=False)
emu.mem_map(0, 0x7000)

memory = bytearray(0x7000)
struct.pack_into('<3I', memory, 0,
                 0x58000080,  # ldr x0, #0x10
                 0xb9400001,  # ldr w1, [x0]
                 0x14000000)  # b .
struct.pack_into('<Q', memory, 0x10, TARGET_VA)
emu.mem_write(0, memory)

emu.qemu_set_invalid_memory_diagnostic_capture()

seen = []
emu.hook_add(HEDGEHOG_HOOK_MEM_INVALID,
             lambda _uc, access, addr, size, _value, _data:
                 seen.append((access, addr, size)) or False)
emu.qemu_set_pc(0)
run_result, _cpu_exit = emu.qemu_run(2)
assert run_result == QEMU_HEDGEHOG_RUN_INVALID_MEMORY
assert seen == [(QEMU_HEDGEHOG_MEM_ACCESS_READ, TARGET_VA, 4)]

diagnostic = emu.qemu_last_invalid_memory_diagnostic()
assert diagnostic is not None
assert diagnostic.fault_vaddr == TARGET_VA
assert diagnostic.translation_valid is True
assert diagnostic.physical_address == TARGET_VA
assert diagnostic.region_valid is True
assert diagnostic.region_base == 0
assert diagnostic.mmu_idx >= 0
assert diagnostic.size == 4
assert diagnostic.access_type == QEMU_HEDGEHOG_MEM_ACCESS_READ
assert diagnostic.memtx_result & QEMU_MEMTX_DECODE_ERROR
emu.close()
'''
    env = os.environ.copy()
    env['PYTHONPATH'] = str(ROOT / 'python')
    env['QEMU_HEDGEHOG_BACKEND_LIBRARY'] = str(backend_library)
    result = subprocess.run(
        [sys.executable, '-c', script], cwd=ROOT, env=env,
        text=True, capture_output=True, check=False, timeout=15,
    )
    assert result.returncode == 0, result.stdout + result.stderr


def test_aarch64_finite_runs_advance_generic_counter() -> None:
    backend_library = _backend_library()
    if not backend_library.exists():
        pytest.skip(f'Hedgehog backend library not found: {backend_library}')

    script = r'''
from qemu.hedgehog import Hedgehog, HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM
from qemu.hedgehog.constants import QEMU_HEDGEHOG_RUN_BUDGET_EXHAUSTED

code = bytes.fromhex(
    '20e03bd5'  # mrs x0, cntpct_el0
    '21e03bd5'  # mrs x1, cntpct_el0
    '42e03bd5'  # mrs x2, cntvct_el0
    '03e03bd5'  # mrs x3, cntfrq_el0
    '1f2003d5'  # nop
)

emu = Hedgehog(HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM, cpu_type='max')
emu.qemu_set_aarch64_reset_state(current_el=3, secure=True)
emu.mem_map(0, 0x1000)
emu.mem_write(0, code)
emu.qemu_set_pc(0)
run_result, cpu_exit = emu.qemu_run(5)

assert run_result == QEMU_HEDGEHOG_RUN_BUDGET_EXHAUSTED, (run_result, cpu_exit)
assert emu.reg_read(3) > 0
assert emu.reg_read(1) > emu.reg_read(0)
assert emu.reg_read(2) > 0
assert emu.qemu_get_pc() == len(code)
'''
    env = os.environ.copy()
    env['PYTHONPATH'] = str(ROOT / 'python')
    env['QEMU_HEDGEHOG_BACKEND_LIBRARY'] = str(backend_library)

    result = subprocess.run(
        [sys.executable, '-c', script],
        cwd=ROOT,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stdout + result.stderr


def test_aarch64_cpreg_overlay_and_live_reset_values() -> None:
    backend_library = _backend_library()
    if not backend_library.exists():
        pytest.skip(f'Hedgehog backend library not found: {backend_library}')

    script = r'''
from qemu.hedgehog import Hedgehog, HedgehogError, HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM
from qemu.hedgehog.constants import QEMU_HEDGEHOG_RUN_BUDGET_EXHAUSTED

SYNTHETIC_MPIDR = 0x80000042
SYNTHETIC_CTR = 0x8444C004
SYNTHETIC_ERRIDR = 3
SYNTHETIC_CNTFRQ = 10_000_003

code = bytes.fromhex(
    'e0ff3fd5'  # mrs x0, synthetic s3_7_c15_c15_7
    '410480d2'  # mov x1, #0x22
    'e1ff1fd5'  # msr synthetic s3_7_c15_c15_7, x1
    'e2ff3fd5'  # mrs x2, synthetic s3_7_c15_c15_7
    'a30038d5'  # mrs x3, mpidr_el1
    '24003bd5'  # mrs x4, ctr_el0
    '055338d5'  # mrs x5, erridr_el1
    '06e03bd5'  # mrs x6, cntfrq_el0
    '1f2003d5'  # nop
)

emu = Hedgehog(HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM, cpu_type='cortex-a55')
emu.qemu_set_aarch64_reset_state(current_el=3, secure=True)
emu.qemu_add_aarch64_cp_reg(3, 7, 15, 15, 7, reset_value=0x11)
emu.qemu_set_aarch64_cp_reg_value(3, 0, 0, 0, 5, reset_value=SYNTHETIC_MPIDR)
emu.qemu_set_aarch64_cp_reg_value(3, 3, 0, 0, 1, reset_value=SYNTHETIC_CTR)
emu.qemu_set_aarch64_cp_reg_value(3, 0, 5, 3, 0, reset_value=SYNTHETIC_ERRIDR)
emu.qemu_set_aarch64_cp_reg_value(3, 3, 14, 0, 0, reset_value=SYNTHETIC_CNTFRQ)

try:
    emu.qemu_add_aarch64_cp_reg(3, 7, 15, 15, 7)
except HedgehogError:
    pass
else:
    raise AssertionError('duplicate CPREG overlay was accepted')

emu.mem_map(0, 0x1000)
emu.mem_write(0, code)
emu.qemu_set_pc(0)
run_result, cpu_exit = emu.qemu_run(9)
assert run_result == QEMU_HEDGEHOG_RUN_BUDGET_EXHAUSTED, (run_result, cpu_exit)
assert emu.reg_read(0) == 0x11
assert emu.reg_read(2) == 0x22
assert emu.reg_read(3) == SYNTHETIC_MPIDR
assert emu.reg_read(4) == SYNTHETIC_CTR
assert emu.reg_read(5) == SYNTHETIC_ERRIDR
assert emu.reg_read(6) == SYNTHETIC_CNTFRQ

try:
    emu.qemu_add_aarch64_cp_reg(3, 7, 15, 15, 6)
except HedgehogError:
    pass
else:
    raise AssertionError('late CPREG overlay was accepted')

emu._backend.reset()
emu.qemu_reset_stop()
emu.qemu_set_pc(0)
run_result, cpu_exit = emu.qemu_run(1)
assert run_result == QEMU_HEDGEHOG_RUN_BUDGET_EXHAUSTED, (run_result, cpu_exit)
assert emu.reg_read(0) == 0x11
'''
    env = os.environ.copy()
    env['PYTHONPATH'] = str(ROOT / 'python')
    env['QEMU_HEDGEHOG_BACKEND_LIBRARY'] = str(backend_library)

    result = subprocess.run(
        [sys.executable, '-c', script],
        cwd=ROOT,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stdout + result.stderr


def test_aarch64_unknown_cpreg_still_raises_undefined_exception() -> None:
    backend_library = _backend_library()
    if not backend_library.exists():
        pytest.skip(f'Hedgehog backend library not found: {backend_library}')

    script = r'''
from qemu.hedgehog import Hedgehog, HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM

emu = Hedgehog(HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM, cpu_type='cortex-a55')
emu.qemu_set_aarch64_reset_state(current_el=3, secure=True)
emu.mem_map(0, 0x1000)
emu.mem_write(0, bytes.fromhex('e0f738d51f2003d5'))  # mrs x0,s3_0_c15_c7_7; nop
emu.qemu_set_pc(0)
emu.qemu_run(1)

assert emu.qemu_get_pc() == 0x200
assert emu.qemu_arm_diagnostics()['esr_el3'] >> 26 == 0
'''
    env = os.environ.copy()
    env['PYTHONPATH'] = str(ROOT / 'python')
    env['QEMU_HEDGEHOG_BACKEND_LIBRARY'] = str(backend_library)

    result = subprocess.run(
        [sys.executable, '-c', script],
        cwd=ROOT,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stdout + result.stderr


def test_aarch64_prefired_stop_is_observed_without_executing_guest() -> None:
    backend_library = _backend_library()
    if not backend_library.exists():
        pytest.skip(f'Hedgehog backend library not found: {backend_library}')

    script = r'''
from qemu.hedgehog import Hedgehog, HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM
from qemu.hedgehog.constants import QEMU_HEDGEHOG_RUN_STOP_REQUESTED

emu = Hedgehog(HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM, cpu_type='max')
emu.qemu_set_aarch64_reset_state(current_el=3, secure=True)
emu.mem_map(0, 0x1000)
emu.mem_write(0, bytes.fromhex('00040091ffffff17'))  # add x0, x0, #1; loop
emu.qemu_set_pc(0)
emu.qemu_reset_stop()
emu.emu_stop()  # Model a zero-delay timer firing before qemu_run().
run_result, cpu_exit = emu.qemu_run(0)

assert run_result == QEMU_HEDGEHOG_RUN_STOP_REQUESTED, (run_result, cpu_exit)
assert emu.qemu_get_pc() == 0
assert emu.reg_read(0) == 0
emu.close()
'''
    env = os.environ.copy()
    env['PYTHONPATH'] = str(ROOT / 'python')
    env['QEMU_HEDGEHOG_BACKEND_LIBRARY'] = str(backend_library)

    result = subprocess.run(
        [sys.executable, '-c', script],
        cwd=ROOT,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stdout + result.stderr


def test_aarch64_host_backing_survives_until_close_unmaps_it() -> None:
    backend_library = _backend_library()
    if not backend_library.exists():
        pytest.skip(f'Hedgehog backend library not found: {backend_library}')

    script = r'''
import ctypes
import gc
from qemu.hedgehog import Hedgehog, HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM

backing = (ctypes.c_ubyte * 0x1000)()
emu = Hedgehog(HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM, cpu_type='max')
emu.mem_map_ptr(0x1000, 0x1000, 7, ctypes.addressof(backing))
emu.mem_write(0x1000, b'live')
assert bytes(backing[:4]) == b'live'
emu.close()
del backing
gc.collect()
assert emu.mem_regions() == ()
'''
    env = os.environ.copy()
    env['PYTHONPATH'] = str(ROOT / 'python')
    env['QEMU_HEDGEHOG_BACKEND_LIBRARY'] = str(backend_library)

    result = subprocess.run(
        [sys.executable, '-c', script],
        cwd=ROOT,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stdout + result.stderr


def test_aarch64_many_region_close_drains_flatview_cleanup() -> None:
    backend_library = _backend_library()
    if not backend_library.exists():
        pytest.skip(f'Hedgehog backend library not found: {backend_library}')

    script = r'''
import time

from qemu.hedgehog import Hedgehog, HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM

emu = Hedgehog(HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM, cpu_type='max')
for index in range(512):
    base = index * 0x2000
    emu.mem_map(base, 0x1000)
    emu.mem_map_mmio(
        base + 0x1000,
        0x1000,
        lambda *_args: 0,
        lambda *_args: None,
    )

emu.close()
time.sleep(0.1)
assert emu.mem_regions() == ()
'''
    env = os.environ.copy()
    env['PYTHONPATH'] = str(ROOT / 'python')
    env['QEMU_HEDGEHOG_BACKEND_LIBRARY'] = str(backend_library)

    result = subprocess.run(
        [sys.executable, '-c', script],
        cwd=ROOT,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stdout + result.stderr


def test_aarch64_finite_runs_handle_tlbi_atomic_step() -> None:
    backend_library = _backend_library()
    if not backend_library.exists():
        pytest.skip(f'Hedgehog backend library not found: {backend_library}')

    script = r'''
from qemu.hedgehog import Hedgehog, HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM
from qemu.hedgehog.constants import QEMU_HEDGEHOG_RUN_BUDGET_EXHAUSTED

code = bytes.fromhex(
    '9f830cd5'  # tlbi alle1is
    '1f2003d5'  # nop
)

emu = Hedgehog(HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM, cpu_type='max')
emu.qemu_set_aarch64_reset_state(current_el=3, secure=True)
emu.mem_map(0, 0x1000)
emu.mem_write(0, code)
emu.qemu_set_pc(0)
run_result, cpu_exit = emu.qemu_run(1)

assert run_result == QEMU_HEDGEHOG_RUN_BUDGET_EXHAUSTED, (run_result, cpu_exit)
assert emu.qemu_get_pc() == 4
'''
    env = os.environ.copy()
    env['PYTHONPATH'] = str(ROOT / 'python')
    env['QEMU_HEDGEHOG_BACKEND_LIBRARY'] = str(backend_library)

    result = subprocess.run(
        [sys.executable, '-c', script],
        cwd=ROOT,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stdout + result.stderr


def test_aarch64_unbounded_run_resumes_after_tlbi_and_observes_stop() -> None:
    backend_library = _backend_library()
    if not backend_library.exists():
        pytest.skip(f'Hedgehog backend library not found: {backend_library}')

    script = r'''
from threading import Timer

from qemu.hedgehog import Hedgehog, HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM
from qemu.hedgehog.constants import QEMU_HEDGEHOG_RUN_STOP_REQUESTED

code = bytes.fromhex(
    '9f830cd5'  # tlbi alle1is
    '400580d2'  # mov x0, #0x2a
    '00000014'  # b .
)

emu = Hedgehog(HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM, cpu_type='max')
emu.qemu_set_aarch64_reset_state(current_el=3, secure=True)
emu.mem_map(0, 0x1000)
emu.mem_write(0, code)
emu.qemu_set_pc(0)
emu.qemu_reset_stop()
timer = Timer(0.1, emu.emu_stop)
timer.start()
try:
    run_result, cpu_exit = emu.qemu_run(0)
finally:
    timer.cancel()

assert run_result == QEMU_HEDGEHOG_RUN_STOP_REQUESTED, (run_result, cpu_exit)
assert emu.reg_read(0) == 0x2a
assert emu.qemu_get_pc() == 8
'''
    env = os.environ.copy()
    env['PYTHONPATH'] = str(ROOT / 'python')
    env['QEMU_HEDGEHOG_BACKEND_LIBRARY'] = str(backend_library)

    result = subprocess.run(
        [sys.executable, '-c', script],
        cwd=ROOT,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stdout + result.stderr


def test_aarch64_repeated_tlbi_keeps_direct_run_scheduler_ownership() -> None:
    backend_library = _backend_library()
    if not backend_library.exists():
        pytest.skip(f'Hedgehog backend library not found: {backend_library}')

    script = r'''
from threading import Timer

from qemu.hedgehog import Hedgehog, HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM
from qemu.hedgehog.constants import QEMU_HEDGEHOG_RUN_STOP_REQUESTED

code = bytes.fromhex(
    '000082d2'  # mov x0, #0x1000
    '9f830cd5'  # tlbi alle1is
    '000400f1'  # subs x0, x0, #1
    'c1ffff54'  # b.ne 4
    '410580d2'  # mov x1, #0x2a
    '00000014'  # b .
)

emu = Hedgehog(HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM, cpu_type='max')
emu.qemu_set_aarch64_reset_state(current_el=3, secure=True)
emu.mem_map(0, 0x1000)
emu.mem_write(0, code)
emu.qemu_set_pc(0)
emu.qemu_reset_stop()
timer = Timer(1.0, emu.emu_stop)
timer.start()
try:
    run_result, cpu_exit = emu.qemu_run(0)
finally:
    timer.cancel()

assert run_result == QEMU_HEDGEHOG_RUN_STOP_REQUESTED, (run_result, cpu_exit)
assert emu.reg_read(0) == 0
assert emu.reg_read(1) == 0x2a
assert emu.qemu_get_pc() == 0x14
'''
    env = os.environ.copy()
    env['PYTHONPATH'] = str(ROOT / 'python')
    env['QEMU_HEDGEHOG_BACKEND_LIBRARY'] = str(backend_library)

    result = subprocess.run(
        [sys.executable, '-c', script],
        cwd=ROOT,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stdout + result.stderr


def test_aarch64_hard_interrupt_wakes_wfi_and_vectors_natively() -> None:
    backend_library = _backend_library()
    if not backend_library.exists():
        pytest.skip(f'Hedgehog backend library not found: {backend_library}')

    script = r'''
from threading import Timer

from qemu.hedgehog import Hedgehog, HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM
from qemu.hedgehog.constants import (
    QEMU_HEDGEHOG_RUN_HALTED,
    QEMU_HEDGEHOG_RUN_STOP_REQUESTED,
)

code = bytearray(0x1000)
code[0x000:0x01c] = bytes.fromhex(
    '01113ed5'  # mrs x1, scr_el3
    '21007fb2'  # orr x1, x1, #2 (route IRQ to EL3)
    '01111ed5'  # msr scr_el3, x1
    'df3f03d5'  # isb
    'ff4203d5'  # msr daifclr, #2
    '7f2003d5'  # wfi
    '1f2003d5'  # nop
)
code[0x280:0x288] = bytes.fromhex(
    '400580d2'  # mov x0, #0x2a
    '00000014'  # b .
)

emu = Hedgehog(HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM, cpu_type='max')
emu.qemu_set_aarch64_reset_state(current_el=3, secure=True)
emu.mem_map(0, 0x1000)
emu.mem_write(0, bytes(code))
emu.qemu_set_pc(0)

run_result, cpu_exit = emu.qemu_run(6)
assert run_result == QEMU_HEDGEHOG_RUN_HALTED, (run_result, cpu_exit)
assert emu.qemu_get_pc() == 0x18
assert emu.qemu_arm_diagnostics()['halted'] == 1

emu.qemu_set_hard_interrupt(True)
assert emu.qemu_arm_diagnostics()['interrupt_request'] != 0

emu.qemu_reset_stop()
timer = Timer(0.1, emu.emu_stop)
timer.start()
try:
    run_result, cpu_exit = emu.qemu_run(0)
finally:
    timer.cancel()

assert run_result == QEMU_HEDGEHOG_RUN_STOP_REQUESTED, (run_result, cpu_exit)
assert emu.reg_read(0) == 0x2a
assert emu.qemu_get_pc() == 0x284

emu.qemu_set_hard_interrupt(False)
assert emu.qemu_arm_diagnostics()['interrupt_request'] == 0
'''
    env = os.environ.copy()
    env['PYTHONPATH'] = str(ROOT / 'python')
    env['QEMU_HEDGEHOG_BACKEND_LIBRARY'] = str(backend_library)

    result = subprocess.run(
        [sys.executable, '-c', script],
        cwd=ROOT,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stdout + result.stderr


def test_aarch64_hard_interrupt_rejects_board_backed_machine() -> None:
    backend_library = _backend_library()
    if not backend_library.exists():
        pytest.skip(f'Hedgehog backend library not found: {backend_library}')

    script = r'''
from qemu.hedgehog import (
    Hedgehog,
    HedgehogError,
    HEDGEHOG_ARCH_ARM64,
    HEDGEHOG_MODE_ARM,
)

emu = Hedgehog(
    HEDGEHOG_ARCH_ARM64,
    HEDGEHOG_MODE_ARM,
    cpu_type='cortex-a55',
    machine_type='virt',
)
try:
    emu.qemu_set_hard_interrupt(True)
except HedgehogError:
    pass
else:
    raise AssertionError('board-backed hard interrupt input was accepted')
'''
    env = os.environ.copy()
    env['PYTHONPATH'] = str(ROOT / 'python')
    env['QEMU_HEDGEHOG_BACKEND_LIBRARY'] = str(backend_library)

    result = subprocess.run(
        [sys.executable, '-c', script],
        cwd=ROOT,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stdout + result.stderr


def test_aarch64_yield_hint_resumes_standalone_execution() -> None:
    backend_library = _backend_library()
    if not backend_library.exists():
        pytest.skip(f'Hedgehog backend library not found: {backend_library}')

    script = r'''
from threading import Timer

from qemu.hedgehog import Hedgehog, HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM
from qemu.hedgehog.constants import QEMU_HEDGEHOG_RUN_STOP_REQUESTED

code = bytes.fromhex(
    '3f2003d5'  # yield
    '400580d2'  # mov x0, #0x2a
    '00000014'  # b .
)

emu = Hedgehog(HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM, cpu_type='max')
emu.qemu_set_aarch64_reset_state(current_el=3, secure=True)
emu.mem_map(0, 0x1000)
emu.mem_write(0, code)
emu.qemu_set_pc(0)
emu.qemu_reset_stop()
timer = Timer(0.1, emu.emu_stop)
timer.start()
try:
    run_result, cpu_exit = emu.qemu_run(0)
finally:
    timer.cancel()

assert run_result == QEMU_HEDGEHOG_RUN_STOP_REQUESTED, (run_result, cpu_exit)
assert emu.reg_read(0) == 0x2a
assert emu.qemu_get_pc() == 8
'''
    env = os.environ.copy()
    env['PYTHONPATH'] = str(ROOT / 'python')
    env['QEMU_HEDGEHOG_BACKEND_LIBRARY'] = str(backend_library)

    result = subprocess.run(
        [sys.executable, '-c', script],
        cwd=ROOT,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stdout + result.stderr


def test_aarch64_arm_diagnostics_capture_smc_eret_state() -> None:
    backend_library = _backend_library()
    if not backend_library.exists():
        pytest.skip(f'Hedgehog backend library not found: {backend_library}')

    script = r'''
from qemu.hedgehog import Hedgehog, HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM
from qemu.hedgehog.constants import QEMU_HEDGEHOG_RUN_BUDGET_EXHAUSTED

code = bytearray(0x1000)
code[0x000:0x004] = bytes.fromhex('030000d4')  # smc #0
code[0x004:0x008] = bytes.fromhex('1f2003d5')  # nop
code[0x600:0x620] = bytes.fromhex(
    '0a113cd5'  # mrs x10, hcr_el2
    '4a0161b2'  # orr x10, x10, #0x80000000
    '0a111cd5'  # msr hcr_el2, x10
    '0a113ed5'  # mrs x10, scr_el3
    '4a0176b2'  # orr x10, x10, #0x400
    '0a111ed5'  # msr scr_el3, x10
    'df3f03d5'  # isb
    'e0039fd6'  # eret
)

emu = Hedgehog(HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM, cpu_type='max')
emu.qemu_set_aarch64_reset_state(current_el=1, secure=False)
emu.mem_map(0, 0x1000)
emu.mem_write(0, bytes(code))
emu.qemu_set_pc(0)

before = emu.qemu_arm_diagnostics()
assert before['current_el'] == 1
assert before['pc'] == 0
assert before['generic_timer_count'] == 7
assert len(before['generic_timer_cval']) == 7
assert len(before['generic_timer_ctl']) == 7
assert len(before['generic_timer_enabled']) == 7
assert len(before['generic_timer_imask']) == 7
assert len(before['generic_timer_istatus']) == 7

run_result, cpu_exit = emu.qemu_run(1)
assert run_result == QEMU_HEDGEHOG_RUN_BUDGET_EXHAUSTED, (run_result, cpu_exit)

in_el3 = emu.qemu_arm_diagnostics()
assert in_el3['current_el'] == 3
assert in_el3['pc'] == 0x600
assert in_el3['elr_el3'] == 4
assert in_el3['spsr_el3'] & 0xf == 5
assert in_el3['esr_el3'] >> 26 == 0x17
assert in_el3['exception_syndrome'] >> 26 == 0x17
assert in_el3['exception_target_el'] == 3

run_result, cpu_exit = emu.qemu_run(8)
assert run_result == QEMU_HEDGEHOG_RUN_BUDGET_EXHAUSTED, (run_result, cpu_exit)

after_eret = emu.qemu_arm_diagnostics()
assert after_eret['current_el'] == 1
assert after_eret['pc'] == 4
assert after_eret['elr_el3'] == 4
assert after_eret['scr_el3'] & 0x400
assert after_eret['hcr_el2'] & 0x80000000
'''
    env = os.environ.copy()
    env['PYTHONPATH'] = str(ROOT / 'python')
    env['QEMU_HEDGEHOG_BACKEND_LIBRARY'] = str(backend_library)

    result = subprocess.run(
        [sys.executable, '-c', script],
        cwd=ROOT,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stdout + result.stderr


def test_standalone_native_timer_queue_runs_during_execution_and_idle() -> None:
    backend_library = _backend_library()
    if not backend_library.exists():
        pytest.skip(f'Hedgehog backend library not found: {backend_library}')
    script = r'''
import struct
import threading
import time
from qemu.hedgehog import Hedgehog, HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM
from qemu.hedgehog.constants import QEMU_HEDGEHOG_RUN_STOP_REQUESTED
emu = Hedgehog(HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM, cpu_type='cortex-a55')
emu.qemu_set_aarch64_reset_state(current_el=3, secure=True)
emu.qemu_set_aarch64_cp_reg_value(3, 3, 14, 0, 0, reset_value=26000000)
emu.mem_map(0x1000, 0x1000)
# Guest writes TVAL/CTL, then loops. A separate guest read observes CTL.
emu.mem_write(0x1000, struct.pack('<3I', 0xd51be202, 0xd51be223, 0x14000000))
emu.mem_write(0x1100, struct.pack('<I', 0xd53be224))
emu.mem_write(0x1200, struct.pack('<2I', 0xd503207f, 0x14000000))
def program(ticks, ctl):
    emu.reg_write(2, ticks)
    emu.reg_write(3, ctl)
    emu.qemu_set_pc(0x1000)
    emu.qemu_reset_stop()
    emu.qemu_run(2)
def control():
    # Until timer outputs are connected, explicitly wake the halted CPU to
    # inspect status. DAIF masks exception entry; this does not prove IRQ wiring.
    halted = emu.qemu_arm_diagnostics()['halted']
    if halted:
        emu.qemu_set_hard_interrupt(True)
    emu.qemu_set_pc(0x1100)
    emu.qemu_reset_stop()
    emu.qemu_run(1)
    if halted:
        emu.qemu_set_hard_interrupt(False)
    return emu.reg_read(4, size=8)
def active(pc):
    emu.qemu_set_pc(pc)
    emu.qemu_reset_stop()
    timer = threading.Timer(0.08, emu.emu_stop)
    timer.start()
    assert emu.qemu_run(0)[0] == QEMU_HEDGEHOG_RUN_STOP_REQUESTED
    timer.join()
program(1300000, 1)  # 50 ms in guest time.
assert control() == 1
before = emu.qemu_virtual_clock_ns()
time.sleep(0.08)
assert emu.qemu_virtual_clock_ns() == before
assert control() == 1
active(0x1008)
assert control() == 5, 'native timer queue must update ISTATUS without explicit polling'
program(1300000, 3)  # Rearm, masked output; ISTATUS still expires.
assert control() == 3
active(0x1200)  # WFI must keep virtual time active and accept external stop.
assert control() == 7
program(1300000, 0)
active(0x1008)
assert control() == 0
# Exercise stop arriving before, during, and immediately after idle entry.
for iteration in range(32):
    emu.qemu_set_pc(0x1200)
    emu.qemu_reset_stop()
    timer = threading.Timer((iteration % 4) * 0.0001, emu.emu_stop)
    timer.start()
    assert emu.qemu_run(0)[0] == QEMU_HEDGEHOG_RUN_STOP_REQUESTED
    timer.join()
before = emu.qemu_virtual_clock_ns()
time.sleep(0.03)
assert emu.qemu_virtual_clock_ns() == before
emu.close()
'''
    env = os.environ.copy()
    env['PYTHONPATH'] = str(ROOT / 'python')
    env['QEMU_HEDGEHOG_BACKEND_LIBRARY'] = str(backend_library)
    result = subprocess.run(
        [sys.executable, '-c', script], cwd=ROOT, env=env,
        text=True, capture_output=True, check=False, timeout=15,
    )
    assert result.returncode == 0, result.stdout + result.stderr


def test_native_timer_output_wakes_wfi_and_deasserts_outside_dispatcher() -> None:
    backend_library = _backend_library()
    if not backend_library.exists():
        pytest.skip(f'Hedgehog backend library not found: {backend_library}')
    script = r'''
import struct
import threading
from qemu.hedgehog import Hedgehog, HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM
from qemu.hedgehog.constants import QEMU_HEDGEHOG_RUN_STOP_REQUESTED
emu = Hedgehog(HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM, cpu_type='cortex-a55')
emu.qemu_set_aarch64_reset_state(current_el=3, secure=True)
emu.qemu_set_aarch64_cp_reg_value(3, 3, 14, 0, 0, reset_value=26000000)
owner = threading.get_ident()
events = []
def output(index, level):
    assert threading.get_ident() == owner
    events.append((index, level))
    emu.qemu_set_hard_interrupt(level)
emu.qemu_connect_cpu_output(0, output)
code = bytearray(0x1000)
code[:0x24] = struct.pack('<9I',
    0xd53e1101, 0xb27f0021, 0xd51e1101, 0xd5033fdf,
    0xd50342ff, 0xd51be202, 0xd51be223, 0xd503207f, 0x14000000)
code[0x280:0x28c] = struct.pack('<3I', 0xd2800540, 0xd51be23f, 0x14000000)
emu.mem_map(0, 0x1000)
emu.mem_write(0, code)
emu.reg_write(2, 520000)  # Native timer expires after 20 ms.
emu.reg_write(3, 1)
emu.qemu_set_pc(0)
timer = threading.Timer(0.08, emu.emu_stop)
timer.start()
assert emu.qemu_run(0)[0] == QEMU_HEDGEHOG_RUN_STOP_REQUESTED
timer.join()
assert emu.reg_read(0, size=8) == 42
assert emu.qemu_get_pc() == 0x288
assert events == [(0, True), (0, False)], events
try:
    emu.qemu_connect_cpu_output(1, output)
except Exception:
    pass
else:
    raise AssertionError('CPU output connections must be setup-only')
emu.close()
'''
    env = os.environ.copy()
    env['PYTHONPATH'] = str(ROOT / 'python')
    env['QEMU_HEDGEHOG_BACKEND_LIBRARY'] = str(backend_library)
    result = subprocess.run(
        [sys.executable, '-c', script], cwd=ROOT, env=env,
        text=True, capture_output=True, check=False, timeout=15,
    )
    assert result.returncode == 0, result.stdout + result.stderr


def test_host_counter_frequency_rescales_native_timer_deadlines() -> None:
    backend_library = _backend_library()
    if not backend_library.exists():
        pytest.skip(f'Hedgehog backend library not found: {backend_library}')
    script = r'''
import ctypes, os, struct
from qemu.hedgehog import Hedgehog, HedgehogError, HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM
emu=Hedgehog(HEDGEHOG_ARCH_ARM64,HEDGEHOG_MODE_ARM,cpu_type='cortex-a55')
emu.qemu_set_aarch64_reset_state(current_el=3,secure=True)
emu.qemu_set_aarch64_cp_reg_value(3,3,14,0,0,reset_value=26000000)
# Both invalid live and reset frequency must fail before replacing the bank.
for reset, live in ((0, 26000000), (26000000, 0)):
    try:
        emu.qemu_set_aarch64_cp_reg_value(3, 3, 14, 0, 0,
                                        reset_value=reset, value=live)
    except HedgehogError:
        pass
    else:
        raise AssertionError('zero host frequency accepted')
emu.mem_map(0x1000,0x1000)
emu.mem_write(0x1000,struct.pack('<3I',0xd51be202,0xd51be223,0x14000000))
emu.reg_write(2,26000000);emu.reg_write(3,1);emu.qemu_set_pc(0x1000)
lib=ctypes.CDLL(os.environ['QEMU_HEDGEHOG_BACKEND_LIBRARY'])
lib.qemu_clock_deadline_ns_all.argtypes=[ctypes.c_int,ctypes.c_int]
lib.qemu_clock_deadline_ns_all.restype=ctypes.c_int64
from qemu.hedgehog import HEDGEHOG_HOOK_CODE
values=[]
def observe(*args):
    values.append(lib.qemu_clock_deadline_ns_all(1,-1))
emu.hook_add(HEDGEHOG_HOOK_CODE,observe,begin=0x1008,end=0x1008)
emu.qemu_run(3)

assert len(values)==1 and 800000000 < values[0] < 1050000000, values
# Guest CNTFRQ metadata writes must not retime the physical counter/timers.
emu.mem_write(0x1100, struct.pack('<2I', 0xd51be005, 0xd53be006))
emu.reg_write(5, 1000000)
emu.qemu_set_pc(0x1100)
emu.qemu_run(2)
assert emu.reg_read(6, size=8) == 1000000
emu.qemu_set_pc(0x1000)
emu.qemu_run(3)
assert len(values) == 2 and 800000000 < values[1] < 1050000000, values
emu.close()
'''
    env = os.environ.copy()
    env['PYTHONPATH'] = str(ROOT / 'python')
    env['QEMU_HEDGEHOG_BACKEND_LIBRARY'] = str(backend_library)
    result = subprocess.run(
        [sys.executable, '-c', script], cwd=ROOT, env=env,
        text=True, capture_output=True, check=False, timeout=15,
    )
    assert result.returncode == 0, result.stdout + result.stderr


def test_standalone_virtual_device_timer_lifecycle() -> None:
    backend_library = _backend_library()
    if not backend_library.exists():
        pytest.skip(f'Hedgehog backend library not found: {backend_library}')
    script = r'''
import struct, threading, time
from qemu.hedgehog import Hedgehog, HedgehogError, HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM
from qemu.hedgehog.constants import QEMU_HEDGEHOG_RUN_STOP_REQUESTED
emu = Hedgehog(HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM, cpu_type='cortex-a55')
emu.mem_map(0, 4096)
emu.mem_write(0, struct.pack('<2I', 0xd503207f, 0x14000000))
owner = threading.get_ident()
events = []
def expired():
    assert threading.get_ident() == owner
    events.append(emu.qemu_virtual_clock_ns())
emu.qemu_connect_virtual_timer(expired)
emu.qemu_arm_virtual_timer(emu.qemu_virtual_clock_ns() + 20_000_000)
time.sleep(0.03)
assert not events
# Cancellation suppresses the pending callback, including across WFI.
emu.qemu_arm_virtual_timer(None)
def run():
    emu.qemu_reset_stop()
    stopper = threading.Timer(0.05, emu.emu_stop)
    stopper.start()
    assert emu.qemu_run(0)[0] == QEMU_HEDGEHOG_RUN_STOP_REQUESTED
    stopper.join()
run()
assert not events
try:
    emu.qemu_connect_virtual_timer(expired)
except HedgehogError:
    pass
else:
    raise AssertionError('late timer connection accepted')
deadline = emu.qemu_virtual_clock_ns() + 10_000_000
emu.qemu_arm_virtual_timer(deadline)
run()  # Callback must be dispatched on the run thread even from halted reentry.
assert len(events) == 1 and events[0] >= deadline
emu.qemu_arm_virtual_timer(emu.qemu_virtual_clock_ns() + 10_000_000)
emu.qemu_arm_virtual_timer(None)
run()
assert len(events) == 1
emu.close()
'''
    env = os.environ.copy()
    env['PYTHONPATH'] = str(ROOT / 'python')
    env['QEMU_HEDGEHOG_BACKEND_LIBRARY'] = str(backend_library)
    result = subprocess.run(
        [sys.executable, '-c', script], cwd=ROOT, env=env,
        text=True, capture_output=True, check=False, timeout=15,
    )
    assert result.returncode == 0, result.stdout + result.stderr


def test_aarch64_system_call_observer_reports_native_returns() -> None:
    backend_library = _backend_library()
    if not backend_library.exists():
        pytest.skip(f'Hedgehog backend library not found: {backend_library}')

    script = r'''
import struct
import threading
import os
from qemu.hedgehog import Hedgehog, HedgehogError, HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM
from qemu.hedgehog.constants import QEMU_HEDGEHOG_RUN_STOP_REQUESTED

owner = threading.get_ident()
instruction = os.environ['HEDGEHOG_SYSTEM_CALL_INSTRUCTION']
opcode, source_el, target_el, call_pc, vector_pc = {
    'svc': (0xd4001561, 3, 3, 0, 0x200),
    'hvc': (0xd4001562, 3, 3, 0xc, 0x200),
    'smc': (0xd4001563, 3, 3, 0, 0x200),
}[instruction]
emu = Hedgehog(HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM, cpu_type='cortex-a55')
emu.qemu_set_aarch64_reset_state(current_el=3, secure=True)
emu.mem_map(0, 0x1000)
code = bytearray(0x1000)
if instruction == 'hvc':
    # EL3.HCE is consulted by pre_hvc. Configure it architecturally before
    # the HVC so this test observes a delivered call rather than an UNDEF.
    struct.pack_into(
        '<3I', code, 0,
        0xd2802001,  # mov x1, #0x100 (SCR_EL3.HCE)
        0xd51e1101,  # msr scr_el3, x1
        0xd5033fdf,  # isb
    )
struct.pack_into('<3I', code, call_pc, opcode, 0xd503207f, 0x14000000)
struct.pack_into('<2I', code, vector_pc, 0xd2800840, 0xd69f03e0)
emu.mem_write(0, code)
emu.reg_write(0, 0x1234)
events = []
def observe(event):
    assert threading.get_ident() == owner
    events.append(event)
emu.qemu_connect_system_call_observer(observe)
emu.qemu_set_pc(0)
timer = threading.Timer(0.03, emu.emu_stop)
timer.start()
try:
    assert emu.qemu_run(0)[0] == QEMU_HEDGEHOG_RUN_STOP_REQUESTED
finally:
    timer.cancel()
    timer.join()
assert len(events) == 2, (instruction, events)
request, complete = events
assert request.phase == 'request'
assert complete.phase == 'complete'
assert request.instruction == complete.instruction == instruction
assert request.sequence == complete.sequence == 1
assert request.pc == call_pc and request.return_pc == call_pc + 4
assert request.source_el == source_el and request.immediate == 0xab
assert request.target_el is None
assert request.registers[0] == 0x1234
assert complete.target_el == target_el
assert complete.return_el == source_el
assert complete.registers[0] == 0x42
try:
    emu.qemu_connect_system_call_observer(observe)
except HedgehogError:
    pass
else:
    raise AssertionError('late system-call observer connection accepted')
emu.close()
'''
    env = os.environ.copy()
    env['PYTHONPATH'] = str(ROOT / 'python')
    env['QEMU_HEDGEHOG_BACKEND_LIBRARY'] = str(backend_library)
    for instruction in ('svc', 'hvc', 'smc'):
        child_env = env.copy()
        child_env['HEDGEHOG_SYSTEM_CALL_INSTRUCTION'] = instruction
        result = subprocess.run(
            [sys.executable, '-c', script], cwd=ROOT, env=child_env,
            text=True, capture_output=True, check=False, timeout=15,
        )
        assert result.returncode == 0, (
            f'{instruction}: {result.stdout}{result.stderr}'
        )

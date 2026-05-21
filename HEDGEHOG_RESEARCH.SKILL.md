# Hedgehog Codebase Research Skill

## Purpose
Provide guidance for thorough exploration and analysis of the hedgehog module's architecture, implementation, and integration points. This skill helps researchers and developers understand the codebase structure, trace data flows, identify key components, and plan implementation work.

## When to Use This Skill
- Exploring new areas of the hedgehog codebase
- Understanding how features interact (Python ↔ C ↔ QEMU)
- Planning a new feature implementation
- Debugging issues across the Python-C boundary
- Preparing for upstream merges
- Learning the architecture by reading code systematically

## Key Research Questions Addressed

1. **"How does feature X work?"** → Code walk-through with file references
2. **"Where should I add new code?"** → Navigation to appropriate layer + patterns
3. **"How do Python and C communicate?"** → ctypes bridge explanation + code samples
4. **"What breaks on upstream merge?"** → Analysis of integration points
5. **"How is X tested?"** → Testing strategy explanation + test file references

## Codebase Structure & Navigation

### Python Layer (`python/qemu/hedgehog/`)

**Key Files:**
- **`api.py` (~700 lines)**: Main `Hedgehog` class
  - Constructor takes arch, mode, cpu_type, machine_type
  - Public methods: `emu_start()`, `hook_add()`, `hook_del()`, `mem_read()`, `mem_write()`, `reg_read()`, `reg_write()`, `get_coverage()`
  - Manages hook registry and coverage state
  - Delegates to `NativeBackend` (stored as `self._backend`)

- **`backend.py` (~500 lines)**: Backend protocol and implementation
  - `BackendProtocol`: Abstract interface (typing.Protocol) for backend implementations
  - `NativeBackend`: ctypes-based implementation that calls C backend library
  - Key responsibility: ctypes bridge, callback marshalling, error translation
  - Methods correspond 1:1 with C backend functions

- **`constants.py` (~150 lines)**: Enums and constants
  - `Arch`: 11 architecture types (X86, X86_64, ARM, ARM64, MIPS, etc.)
  - `Mode`: BOARD vs MACHINE execution modes
  - `HookType`: Block, instruction, memory-invalid, MMIO callback types
  - `MemProt`: Memory protection flags (READ, WRITE, EXEC, ALL)
  - `RunResult`: Execution result codes (BUDGET_EXHAUSTED, HALTED, EXCEPTION, etc.)
  - `Error`: Error codes (READ_UNMAPPED, WRITE_PROT, EXCEPTION, etc.)

- **`errors.py` (~85 lines)**: Exception handling
  - Maps C backend error codes → Python exceptions
  - `HedgehogError` base exception
  - Specific exceptions: `ReadUnmapped`, `WriteProtected`, `InvalidInstruction`, etc.

- **`docs.md` (~300 lines)**: API reference
  - Constructor options and examples
  - Memory management API
  - Register access API
  - Hook registration examples (board + machine modes)
  - Coverage tracking examples
  - Device property binding examples

### C Backend Layer (`accel/hedgehog/`)

**File Structure:**
- `backend.c`: Core backend lifecycle and dispatch
- Related headers: `include/system/hedgehog-backend.h` (public API), `include/system/hedgehog-exec-hooks.h` (hook types)

**Key Abstractions:**
- `HedgehogBackend`: Main backend object (lifecycle, memory, execution)
- Memory regions: RAM (`MemoryRegionOps`) and MMIO callbacks
- Hook dispatch: Integrated with TCG execution paths

### TCG Integration Points (`accel/tcg/`)

**Modified Files:**
- `hedgehog-exec-hooks.c`: Hedgehog-specific execution hooks
- `cpu-exec.c`: Modified to call TB hooks before/after block execution
- `cputlb.c`: Modified to call invalid-memory hooks for unmapped/protected accesses

**Pattern:** Each hook is conditionally compiled (`#ifdef CONFIG_HEDGEHOG`) and only fires if a hook is registered in the backend.

## Common Research Workflows

### Workflow 1: Understand Hook Execution Flow
**Goal:** How do hooks reach Python callbacks?

**Path:**
1. Start: User calls `hedgehog.hook_add(address, HookType.BLOCK, my_callback)`
2. → `Hedgehog.hook_add()` in `api.py` registers in `_hooks` dict
3. → `_sync_backend_hooks()` sends registration to C backend via ctypes
4. → During execution: TCG calls `hedgehog_exec_tb_hook()` (in `accel/tcg/cpu-exec.c`)
5. → C backend checks hook registry, calls Python callback via CFUNCTYPE bridge
6. → Python callback runs in user code context

**Key Files:**
- `python/qemu/hedgehog/api.py`: `hook_add()`, `_sync_backend_hooks()`
- `accel/hedgehog/backend.c`: Hook registry and dispatch
- `accel/tcg/cpu-exec.c`: Hook call site
- `accel/tcg/hedgehog-exec-hooks.c`: Hook implementation

### Workflow 2: Add a New Hook Type
**Goal:** Implement a custom hook (e.g., "on branch taken")

**Path:**
1. C side: Add enum to `hedgehog-exec-hooks.h`, implement hook function in `hedgehog-exec-hooks.c`
2. Python constants: Add to `HookType` enum in `constants.py`
3. Python protocol: Add method signature to `BackendProtocol` in `backend.py`
4. Python backend: Implement ctypes wrapper in `NativeBackend`
5. Python API: Add dispatch in `Hedgehog.hook_add()` and `_sync_backend_hooks()`
6. Testing: Test hook registration and callback invocation
7. Docs: Update `docs.md` with example usage

**Files Changed:** 5 (1 header + 1 hook impl + 3 Python)

### Workflow 3: Understand Board vs Machine Mode
**Goal:** Learn the execution model differences

**Path:**
1. **Board Mode:**
   - Constructor: `Hedgehog(Arch.X86_64, Mode.BOARD, cpu_type="qemu64-x86_64-cpu")`
   - Memory: User manually calls `mem_map()` to add RAM/MMIO regions
   - Execution: `emu_start(address, max_instructions=100)` runs blocking until budget exhausted
   - No device models, no event loop

2. **Machine Mode:**
   - Constructor: `Hedgehog(Arch.ARM, Mode.MACHINE, machine_type="raspi3b")`
   - Memory: Inherited from machine's device tree (automatic)
   - Device I/O: Requires `qemu_run()` event pump between execution calls
   - Execution: `emu_start()` runs within QEMU's event loop context

**Key Files:**
- `python/qemu/hedgehog/api.py`: Constructor branch on `mode` parameter
- `python/qemu/hedgehog/backend.py`: Backend initialization differs per mode
- `accel/hedgehog/backend.c`: Separate code paths for `HEDGEHOG_MODE_BOARD` vs `HEDGEHOG_MODE_MACHINE`

### Workflow 4: Trace Coverage Tracking
**Goal:** Understand deterministic coverage feedback mechanism

**Path:**
1. User sets: `emu = Hedgehog(..., coverage_mode=CoverageMode.EDGE_DIGEST)`
2. Constructor stores mode in `self._coverage_mode`
3. On `emu_start()`: Installs coverage hook if mode is set (via `_sync_backend_hooks()`)
4. During execution: Coverage hook called after each instruction
5. Hook stores: Block/instruction PC in a set for fast dedup
6. User calls: `coverage = emu.get_coverage()` returns set of covered PCs
7. Optional: `digest = emu.get_coverage_digest()` returns BLAKE2b hash for fuzzing feedback
8. Can clear with: `emu.clear_coverage()` to start fresh tracking

**Key Files:**
- `python/qemu/hedgehog/api.py`: Coverage registration and return methods
- `python/qemu/hedgehog/coverage.md`: Detailed coverage architecture doc
- `constants.py`: `CoverageMode` enum (BLOCK, INSN, DIGEST, EDGE_DIGEST)

## Data Flow Diagrams

### Hook Dispatch Data Flow
```
User Python Code
    ↓
hedgehog.hook_add(address, HookType.BLOCK, my_callback)
    ↓
Hedgehog.hook_add() → stores in _hooks dict
    ↓
Hedgehog._sync_backend_hooks() → calls NativeBackend.set_*_hook()
    ↓
NativeBackend (ctypes) → calls C backend set_*_hook function
    ↓
C backend stores hook registration
    ↓
[During execution]
    ↓
TCG path calls hedgehog_exec_tb_hook() → C backend → CFUNCTYPE bridge
    ↓
my_callback() invoked in Python context
```

### Memory Read/Write Data Flow
```
emu.mem_read(address, size)
    ↓
Hedgehog.mem_read() → calls NativeBackend.mem_read()
    ↓
NativeBackend (ctypes) → calls C backend mem_read_qemu()
    ↓
QEMU MMU & TLB lookup → returns data
    ↓
bytes returned to user
```

## Code Reading Recommendations

### For Quick Understanding (30 min)
1. Read `Python API Docs`: `python/qemu/hedgehog/docs.md` (API overview)
2. Skim `api.py` constructor and `emu_start()` method
3. Read `backend.py` class docstrings and method signatures
4. Review `constants.py` to understand available types

### For Medium Deep Dive (2-3 hours)
1. Above + read complete `api.py` and `backend.py`
2. Trace `hook_add()` → `_sync_backend_hooks()` path
3. Understand `BackendProtocol` and why it's used
4. Read coverage tracking in `api.py` (`get_coverage()`, `clear_coverage()`)

### For Full Understanding (1 day)
1. Above + read complete C backend implementation
2. Review TCG integration in `accel/tcg/cpu-exec.c` and `cputlb.c`
3. Understand ctypes bridge details in `backend.py`
4. Trace execution paths for board vs machine modes
5. Review error code translation in `errors.py`

## Testing & Validation

### Python Unit Tests
Location: Likely `python/tests/` (if exists in repo)
- Test `BackendProtocol` via mock backends
- Test error code translation
- Test hook registration/dispatch
- Test coverage tracking

### Integration Tests
- Build with `--enable-hedgehog --target-list=x86_64-softmmu,aarch64-softmmu`
- Run examples from `hedgehog_quickstart.md`
- Test board mode with manual memory mapping
- Test machine mode with raspi3b or similar

### Cross-Architecture Validation
- Minimum: x86_64 and aarch64
- Test with: `Arch.X86_64`, `Arch.ARM64`, verify coverage/hooks work

## Key Insights to Document

When researching, capture:
- **Why** each architectural decision was made (protocol-based, ctypes, etc.)
- **Constraints** (QEMU API dependencies, cross-arch support, Python GIL)
- **Tradeoffs** (Python flexibility vs C speed, coverage modes, execution models)
- **Integration points** with QEMU (where hedgehog hooks into execution)
- **Testing approach** (how to validate changes)

## Questions to Answer During Research

1. Where does flow enter from Python to C?
2. Where does control return from C to Python?
3. How are errors translated and propagated?
4. What is stateful on each side (Python vs C)?
5. How would a new feature be added?
6. What breaks on QEMU upstream API changes?
7. Why are there two execution modes instead of one?
8. How is thread-safety handled (GIL, C threading)?

## Output of This Research

Good research produces:
- Code map (files, key functions, interactions)
- Data flow diagrams (hook dispatch, memory access, error handling)
- Implementation patterns (what to imitate for new features)
- Testing strategy (how to validate changes)
- Upstream merge impact analysis (what could break)

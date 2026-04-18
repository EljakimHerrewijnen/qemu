# Hedgehog Coverage Tracking

Hedgehog provides optional coverage collection to track executed code blocks, instructions, and control flow during emulation. This is useful for fuzzing, testing, and dynamic analysis workflows.

## Quick Start

Enable coverage tracking by passing the `coverage` parameter to the `Hedgehog` constructor:

```python
from qemu.hedgehog import Hedgehog, HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM

# Enable block-level coverage (default)
h = Hedgehog(HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM, coverage=True)

# Or explicitly specify modes
h = Hedgehog(HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM, coverage='block')

# Enable multiple modes
h = Hedgehog(
    HEDGEHOG_ARCH_ARM64,
    HEDGEHOG_MODE_ARM,
    coverage=('block', 'digest', 'edge_digest')
)

# Collect coverage during emulation
h.mem_map(0x1000, 0x1000)
h.mem_write(0x1000, b'\x1f\x20\x03\xd5')  # ARM64 NOP
h.emu_start(0x1000, 0)

# Query collected coverage
cov = h.get_coverage()
print(f"Unique blocks: {cov['unique_blocks']}")
print(f"Coverage digest: {cov['coverage_digest']}")

# Clear coverage for next run
h.clear_coverage()

# Or use the reset alias
h.reset_coverage()
```

## Coverage Modes

### 1. Block Coverage (`'block'`)

Tracks unique basic blocks executed during emulation.

- **Granularity**: Block-level (QEMU BB transitions)
- **Cost**: Very low (~1-2% overhead)
- **Output**: Set of block PCs, unique count
- **Use case**: General fuzzing, path diversity

**Example:**
```python
h = Hedgehog(HEDGEHOG_ARCH_X86, HEDGEHOG_MODE_64, coverage='block')
h.emu_start(0x1000, 0)

cov = h.get_coverage()
assert 'blocks' in cov  # set of executed block PCs
assert 'unique_blocks' in cov  # count
```

### 2. Instruction Coverage (`'insn'`)

Tracks unique instructions executed.

- **Granularity**: Instruction-level
- **Cost**: Moderate (~5-10% overhead)
- **Output**: Set of instruction PCs, unique count
- **Use case**: Fine-grained analysis, instruction profiling

**Example:**
```python
h = Hedgehog(HEDGEHOG_ARCH_X86, HEDGEHOG_MODE_64, coverage='insn')
h.emu_start(0x1000, 0)

cov = h.get_coverage()
assert 'insn' in cov  # set of executed instruction PCs
assert 'unique_insn' in cov  # count
```

### 3. Coverage Digest (`'digest'`)

Computes a stable BLAKE2b hash of covered block addresses. Useful for rapid comparison of coverage across runs without shipping full PC lists.

- **Output**: 32-character hex BLAKE2b digest, unique block count
- **Deterministic**: Same blocks always produce same digest
- **Collision risk**: Negligible (128-bit hash)
- **Use case**: Fuzzing feedback, coverage change detection

**Example:**
```python
h = Hedgehog(HEDGEHOG_ARCH_X86, HEDGEHOG_MODE_64, coverage=('block', 'digest'))

h.emu_start(0x1000, 0)
digest1 = h.get_coverage_digest()  # Returns hex string like 'a1b2c3...'

h.clear_coverage()
h.emu_start(0x2000, 0)
digest2 = h.get_coverage_digest()

if digest1 != digest2:
    print("Coverage changed!")
```

### 4. Edge Digest (`'edge_digest'`)

Tracks executed control-flow edges (block-to-block or instruction-to-instruction transitions) and computes a stable digest.

- **Output**: 32-character hex BLAKE2b digest, unique edge count
- **Edges**: Pairs of consecutive execution points (src, dst)
- **Cost**: Low (~2-3% overhead)
- **Use case**: Path-sensitive fuzzing, control-flow diversity

**Example:**
```python
h = Hedgehog(
    HEDGEHOG_ARCH_X86,
    HEDGEHOG_MODE_64,
    coverage=('block', 'edge_digest')
)

h.emu_start(0x1000, 0)
cov = h.get_coverage()
print(f"Edges: {cov['unique_edges']}")
print(f"Edge digest: {cov['edge_digest']}")
```

## API Reference

### Constructor Parameter
```python
Hedgehog(
    arch,
    mode,
    coverage: Union[bool, str, Iterable[str]] = False,
    ...
)
```

- `coverage=False` (default): No coverage tracking
- `coverage=True`: Enable block coverage
- `coverage='block'`: Explicitly enable block coverage
- `coverage=('block', 'digest')`: Enable multiple modes
- `coverage=['insn', 'edge_digest']`: List notation also supported

### Methods

#### `get_coverage() -> Dict[str, Any]`

Returns all collected coverage data for enabled modes.

**Returns:**
- `'modes'`: Tuple of enabled coverage modes
- `'blocks'`: Set[int] of block PCs (if block mode enabled)
- `'unique_blocks'`: int count
- `'insn'`: Set[int] of instruction PCs (if insn mode enabled)
- `'unique_insn'`: int count
- `'coverage_digest'`: str hex digest (if digest mode enabled)
- `'edge_digest'`: str hex digest (if edge_digest mode enabled)
- `'unique_edges'`: int count (if edge_digest mode enabled)

#### `get_coverage_digest() -> str`

Returns the BLAKE2b digest of covered block addresses (hex string, 32 chars).

#### `get_edge_digest() -> str`

Returns the BLAKE2b digest of execution edges (hex string, 32 chars).

#### `clear_coverage() -> None`

Reset all coverage state. Useful for multi-run fuzzing where each test case should start fresh.

#### `reset_coverage() -> None`

Alias for `clear_coverage()`. Use this if `reset` naming is clearer in your workflow.

## Recommendations for Fuzzing

### Recommended Configuration

For **coverage-guided fuzzing**, use:

```python
h = Hedgehog(
    arch,
    mode,
    coverage=('block', 'digest', 'edge_digest')
)
```

**Why this combination:**

1. **`block` mode** provides accurate block-level coverage for feedback.
2. **`digest` mode** enables fast coverage-changed detection in the feedback loop (compare 32 hex chars instead of iterating a set).
3. **`edge_digest` mode** captures control-flow diversity, improving path exploration vs. block-only approach.

### Fuzzing Loop Example

```python
def fuzz_testcase(corpus: bytes) -> Tuple[bool, str]:
    """Run one fuzzing iteration and return (new_cov, cov_digest)."""
    h = Hedgehog(HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM,
                  coverage=('block', 'digest', 'edge_digest'))

    h.mem_map(0x100000, 0x100000)
    h.mem_write(0x100000, corpus)
    h.qemu_set_pc(0x100000)

    try:
        h.emu_start(0x100000, until=0, timeout=0, count=100000)
    except HedgehogError:
        pass  # Crash/invalid memory is interesting

    finally:
        h.close()

    cov = h.get_coverage()
    return cov['unique_blocks'] > 0, cov['coverage_digest']

# Maintain a set of seen coverage digests
seen_digests = set()

for test_case in generate_test_cases():
    new_cov, digest = fuzz_testcase(test_case)
    if digest not in seen_digests:
        seen_digests.add(digest)
        print(f"New coverage path found! Digest: {digest}")
        # Add test_case to corpus for mutation/expansion
```

### Performance Considerations

| Mode | Overhead | Block Set Size | Digest Size |
|------|----------|----------------|-------------|
| None | 0% | 0 | 0 |
| `block` | ~1–2% | ~1KB per 1K unique blocks | 0 |
| `insn` | ~5–10% | ~1KB per 1K unique insn | 0 |
| `digest` | ~1–2% | (internal only) | 16 bytes |
| `edge_digest` | ~2–3% | ~1.5KB per 1K edges | 16 bytes |
| All modes | ~10–15% | Sum of above | 32 bytes |

For **continuous fuzzing**, use `digest` + `edge_digest` only to minimize memory while keeping coverage signals. Periodically sample the exact `block` set to understand coverage depth.

### Avoiding Memory Bloat

If fuzzing for hours with millions of unique blocks:

1. Use `'digest'` and `'edge_digest'` only (no exact sets).
2. Sample the exact coverage every N iterations: `h.get_coverage()` then `h.clear_coverage()`.
3. Export digests to a compact log file for batch analysis.

### Noise Reduction

If you have user-installed hooks that depend on coverage state:

```python
h = Hedgehog(..., coverage='block')

def my_hook(_uc, pc, _size, _userdata):
    cov = _uc.get_coverage()
    if cov['unique_blocks'] > threshold:
        return True  # Stop if coverage grew enough
    return False

h.hook_add(HEDGEHOG_HOOK_CODE, my_hook)
```

Coverage collection happens automatically before user hooks are invoked, so you can read coverage state inside user callbacks.

## Tips & Tricks

1. **Coverage delta between runs:**
   ```python
   digest_before = h.get_coverage_digest()
   h.emu_start(...)
   digest_after = h.get_coverage_digest()
   if digest_before != digest_after:
       print("Coverage improved")
   ```

2. **Export coverage to JSON:**
   ```python
   import json
   cov = h.get_coverage()
   with open('coverage.json', 'w') as f:
       json.dump({
           'blocks': sorted(cov.get('blocks', [])),
           'digest': cov.get('coverage_digest'),
       }, f)
   ```

3. **Combine coverage with hooks:**
   ```python
   h = Hedgehog(..., coverage=('block', 'digest'))
   h.hook_add(HEDGEHOG_HOOK_CODE, my_hook)  # User hook + automatic coverage
   ```

4. **Reset between test cases:**
   ```python
   for test in test_cases:
       h.clear_coverage()
       h.emu_start(...)
       new_cov = h.get_coverage_digest() != expected_digest
   ```

## Troubleshooting

**Q: Coverage is not being collected.**
- Ensure `coverage` parameter is not `False` in the constructor.
- Verify `emu_start()` is called (coverage only updates during execution).
- Check that the emulator actually executes code (not hitting unmapped memory immediately).

**Q: Digest always the same.**
- Check if code path is deterministic. Looping on same instructions will have same digest.
- Try `edge_digest` mode to detect repeated patterns.
- Verify coverage is being cleared between runs if comparing multiple executions.

**Q: Memory usage is high.**
- Disable `'block'` and `'insn'` modes if only digests are needed.
- Use `get_coverage()` then `clear_coverage()` periodically in long-running fuzz loops.

**Q: Performance degradation.**
- Coverage modes have low overhead (~1–3%) but compound with other hooks.
- Profile with/without coverage enabled using `time` on your fuzzing loop.
- If slow, disable fine-grained modes (`'insn'`) and use block level only.

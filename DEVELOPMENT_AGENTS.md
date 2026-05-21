# QEMU Hedgehog Fork: Setup & Agent Documentation

Welcome! This is a QEMU fork that adds **hedgehog**, a Python embedding library for using QEMU CPUs like Unicorn for binary analysis, fuzzing, and dynamic instrumentation.

## Quick Start for New Developers

### 1. Orientation (5 minutes)
- **What is hedgehog?** A Hedgehog-compatible Python API that wraps QEMU's in-tree C backend
- **Where is it?** `python/qemu/hedgehog/` (Python) and `accel/hedgehog/` (C)
- **How to build?** `./configure --enable-hedgehog --target-list=x86_64-softmmu,aarch64-softmmu`
- **Current version?** `v0.1.1a9` (see tags in git log)

### 2. Read These Docs (In Order)
1. **`.instructions.md`** (40 min) — Architecture, patterns, upstream management
2. **`hedgehog.md`** (20 min) — Overview of in-tree implementation
3. **`HEDGEHOG_RESEARCH.SKILL.md`** (as needed) — Deep-dive research guide
4. **`AGENTS.md`** (10 min) — How to use the agent setup effectively
5. **`.github/agents/upstream-sync.agent.md`** (10 min) — Upstream synchronization workflow agent
6. **`.github/agents/python-api-build-test.agent.md`** (10 min) — Python wheel build/test workflow agent
7. **`.github/agents/feature-orchestrator.agent.md`** (10 min) — Feature planning and staged execution orchestrator
8. **`.github/agents/qemu-feature-edit-proposal.agent.md`** (10 min) — Merge-safe patch-set proposal specialist
9. **`.github/agents/python-api-exposure.agent.md`** (10 min) — Python API exposure specialist

### 3. Set Up Your Environment
```bash
# Clone this repo (you have it already at /home/eljakim/Source/qemu/)
cd /home/eljakim/Source/qemu

# Build hedgehog backend libraries
mkdir -p build-hedgehog
./configure --enable-hedgehog --target-list=x86_64-softmmu,aarch64-softmmu
ninja -C build-hedgehog libqemu-hedgehog-backend.so libqemu-hedgehog-backend-aarch64.so

# Install Python package
cd python && pip install -e .

# Test it works
python3 -c "from qemu.hedgehog import Hedgehog; print('✓ Hedgehog loaded')"
```

### 4. Try an Example
```bash
# From repo root:
python3 << 'EOF'
from qemu.hedgehog import Hedgehog, Arch, Mode

# Create a simple x86_64 emulator
emu = Hedgehog(Arch.X86_64, Mode.BOARD, cpu_type="qemu64-x86_64-cpu")

# Map 4KB of RAM at address 0x1000
from qemu.hedgehog import MemProt
emu.mem_map(0x1000, 0x1000, MemProt.ALL)

# Write some x86_64 code (return instruction)
emu.mem_write(0x1000, b"\xc3")  # ret

# Set PC and run
emu.set_pc(0x1000)
result = emu.emu_start(0x1000, max_instructions=10)
print(f"✓ Execution completed: {result}")
EOF
```

## Finding Your Way Around

### For Different Tasks

| Task | Start Here |
|------|-----------|
| **Add a new hook type** | `.instructions.md` → "Common Implementation Tasks" + `HEDGEHOG_RESEARCH.SKILL.md` Workflow 2 |
| **Merge upstream QEMU** | `/memories/repo/qemu_upstream_merge_strategy.md` |
| **Understand board vs machine mode** | `.instructions.md` → "Execution Models" +  `HEDGEHOG_RESEARCH.SKILL.md` Workflow 3 |
| **Implement a new feature** | `.instructions.md` → "Implementation Patterns to Follow" |
| **Debug runtime issue** | Use `/troubleshoot` slash command + `.instructions.md` → "Troubleshooting" |
| **Learn the architecture** | `HEDGEHOG_RESEARCH.SKILL.md` → "Code Reading Recommendations" |

### File Organization

**Documentation:**
```
/home/eljakim/Source/qemu/
├── README.rst                      ← Original QEMU readme
├── hedgehog.md                     ← Hedgehog implementation overview
├── hedgehog_quickstart.md          ← Quick start examples
├── .instructions.md                ← Development guide (NEW) ★ START HERE
├── .github/agents/                 ← Copilot custom agents (NEW)
│   ├── hedgehog-core.agent.md      ← Agent config (NEW)
│   ├── upstream-sync.agent.md      ← Upstream sync/rebase specialist (NEW)
│   ├── python-api-build-test.agent.md ← Python build/test specialist (NEW)
│   ├── feature-orchestrator.agent.md ← Multi-stage feature workflow orchestrator (NEW)
│   ├── qemu-feature-edit-proposal.agent.md ← Merge-safe edit planner (NEW)
│   └── python-api-exposure.agent.md ← Python API exposure planner (NEW)
├── AGENTS.md                       ← Agent setup & capabilities (NEW)
└── HEDGEHOG_RESEARCH.SKILL.md      ← Research guide (NEW)
```

**Code:**
```
python/qemu/hedgehog/               ← Python layer (~1000 lines)
├── api.py                          ← Main Hedgehog class
├── backend.py                      ← Backend protocol + ctypes bridge
├── constants.py                    ← Enums (Arch, Mode, HookType, etc.)
├── errors.py                       ← Exception handling
├── docs.md                         ← Python API reference
├── coverage.md                     ← Coverage tracking docs
└── __init__.py                     ← Package exports

accel/hedgehog/                     ← C backend (~500 lines)
├── backend.c                       ← Core implementation
└── meson.build                     ← Build config

accel/tcg/hedgehog*.c               ← TCG hook integration
include/system/hedgehog*.h          ← C backend headers
```

**Configuration & Memory:**
```
/memories/repo/
├── qemu_upstream_merge_strategy.md ← Git workflows & conflict resolution (NEW)
└── hedgehog_*.md                   ← Previous analysis notes
```

## Key Concepts Quick Reference

### Hedgehog Architecture Layers
```
User Python Code
    ↓
Hedgehog (public Python API - api.py)
    ↓
BackendProtocol (interface - backend.py)
    ↓
NativeBackend (ctypes wrapper - backend.py)
    ↓
C Backend Library (libqemu-hedgehog-backend.so)
    ↓
QEMU Core (execution, memory, hooks)
```

### Two Execution Modes
- **Board mode:** Manual memory management, no devices, fast (good for fuzzing)
- **Machine mode:** Full QEMU machines (raspi3b, etc.), device models, real I/O

### Hook System
Callbacks can fire on:
- Translation blocks (TB) before/after execution
- Individual instructions
- Invalid memory accesses (unmapped, write-protected)
- MMIO callbacks (custom Python handlers for devices)

### Coverage Tracking
Four modes for fuzzing feedback:
- `block`: Set of basic blocks executed
- `insn`: Set of individual instructions
- `digest`: BLAKE2b hash of block sequence
- `edge_digest`: Hash of block transitions (AFL-style edges)

## Agent System (NEW)

This repo now has **intelligent agent setup** for guided development:

### Auto-Activated Agent
When you edit files in `python/qemu/hedgehog/`, `accel/hedgehog/`, etc., an agent automatically activates with:
- Full context about hedgehog architecture
- Expert knowledge about implementation patterns
- Guidance on upstream merges
- Troubleshooting help

**Slash Commands:**
- `/research` — Deep-dive into components
- `/feature` — Implement new features
- `/upstream` — Guide through merges
- `/troubleshoot` — Debug issues

### How to Use
```
@copilot /research How does the hook registration and synchronization work?
@copilot /feature I want to add support for conditional breakpoints
@copilot /upstream Help me merge upstream QEMU without losing hedgehog changes
@copilot /troubleshoot I'm getting "backend library not found" error
```

See `AGENTS.md` for more details.

### Specialized Workflow Agents

- **Upstream sync specialist:** use `.github/agents/upstream-sync.agent.md` when pulling latest QEMU commits or resolving rebase/merge conflicts.
- **Python build/test specialist:** use `.github/agents/python-api-build-test.agent.md` when building wheels, validating bundled native libraries, or running API smoke tests.
- **Feature orchestrator:** use `.github/agents/feature-orchestrator.agent.md` to enforce a staged workflow: research, proposal, implementation, optional Python exposure.
- **Edit proposal specialist:** use `.github/agents/qemu-feature-edit-proposal.agent.md` to generate merge-friendly patch-set plans before coding.
- **Python exposure specialist:** use `.github/agents/python-api-exposure.agent.md` when backend features need to become stable public API.

### Streamlined Feature Workflow

For non-trivial features, use this sequence:

1. Orchestrate in `.github/agents/feature-orchestrator.agent.md`.
2. Write source analysis to `docs/hedgehog/feature-research/<feature-name>.md`.
3. Write edit proposal to `docs/hedgehog/feature-proposals/<feature-name>.md`.
4. Implement in small patch sets.
5. Expose to Python via `.github/agents/python-api-exposure.agent.md` only if needed.
6. Validate with `.github/agents/python-api-build-test.agent.md`.
7. If upstream-sensitive, run checks with `.github/agents/upstream-sync.agent.md`.

## Upstream QEMU Management

This fork tracks QEMU's upstream while maintaining hedgehog features. Key strategies:

### Quick Merge (One Command)
```bash
# Add upstream remote (one-time)
git remote add upstream https://github.com/qemu/qemu.git

# Setup git aliases (one-time) - see /memories/repo/qemu_upstream_merge_strategy.md
git config alias.sync-rebase "!git fetch upstream && git rebase upstream/master"
git config alias.hedgehog-build-test "!./configure --enable-hedgehog --target-list=x86_64-softmmu,aarch64-softmmu && ninja -C build-hedgehog"

# Now: One-command sync with testing
git sync-rebase
git hedgehog-build-test
```

### Detailed Workflow
See `/memories/repo/qemu_upstream_merge_strategy.md` for:
- Step-by-step rebase/merge instructions
- Conflict resolution by file type
- Pre/post-sync checklists
- Common problems & solutions

### Expected Conflicts (Rare)
Conflicts likely only in:
- `accel/tcg/cpu-exec.c` or `cputlb.c` (if upstream modified TCG execution)
- `meson.build` or `meson_options.txt` (build config)
- Hedgehog-specific files (unlikely unless upstream adds competing features)

## Development Workflow

### For Features
1. Create feature branch: `git checkout -b feature/my-feature`
2. Read `.instructions.md` → "Implementation Patterns"
3. Implement in appropriate layer (C or Python)
4. Test with: `./configure --enable-hedgehog && ninja -C build-hedgehog`
5. Run examples from `hedgehog_quickstart.md`
6. Merge: `git checkout master && git merge feature/my-feature`

### For Upstream Merges
1. Reference `/memories/repo/qemu_upstream_merge_strategy.md`
2. Use one-liner: `git sync-rebase` (after aliases setup)
3. Resolve conflicts using guide
4. Validate with: `git hedgehog-build-test`
5. Run Python tests if they exist

Tip: For complex conflicts, use the dedicated upstream sync agent and its `/resolve-conflicts` command.
6. Tag and release: `git tag v0.X.Ya && git push origin v0.X.Ya`

### For Releases
- Tag format: `v0.X.Ya` (alpha) or `v0.X.Y` (stable)
- CI builds wheels and publishes to GitHub releases
- Users can install: `pip install https://github.com/EljakimHerrewijnen/qemu/releases/download/...`

## Documentation Map

| Document | Purpose | Read Time | When |
|----------|---------|-----------|------|
| `.instructions.md` | Core patterns & upstream | 40 min | **Start here** |
| `.agent.md` | Agent config & capabilities | 5 min | Understanding automation |
| `AGENTS.md` | How to use agent setup | 10 min | Using the agents |
| `HEDGEHOG_RESEARCH.SKILL.md` | Deep research guide | Variable | Learning architecture |
| `/memories/repo/qemu_upstream_merge_strategy.md` | Git workflows | 30 min | Planning merges |
| `hedgehog.md` | Implementation overview | 20 min | Implementation details |
| `hedgehog_quickstart.md` | Quick start examples | 15 min | Getting started |
| `python/qemu/hedgehog/docs.md` | Python API reference | 30 min | API usage |
| `python/qemu/hedgehog/coverage.md` | Coverage tracking | 15 min | Fuzzing features |

## FAQ

**Q: How do I add a new hook type?**
A: See `.instructions.md` → "Common Implementation Tasks" → "Task: Add a New Hook Type"

**Q: How do I merge upstream without losing hedgehog?**
A: See `/memories/repo/qemu_upstream_merge_strategy.md` or use `/upstream` agent command

**Q: What's the difference between board and machine mode?**
A: See `.instructions.md` → "Execution Models" or `HEDGEHOG_RESEARCH.SKILL.md` Workflow 3

**Q: Where is the Python-C boundary?**
A: Python `NativeBackend` (backend.py) ←→ C backend via ctypes. See `HEDGEHOG_RESEARCH.SKILL.md` data flows

**Q: Why use Protocol instead of ABC?**
A: Runtime-checkable protocols allow flexible backends (easy to mock for testing). Explained in `.instructions.md`

**Q: Can I use hedgehog outside this repo?**
A: Yes! Package is published as `qemu` on PyPI wheels. See `hedgehog_quickstart.md` installation section

**Q: What breaks when QEMU upstream changes?**
A: Usually TCG integration or memory/CPU state APIs. See "Conflict Scenarios" in `/memories/repo/qemu_upstream_merge_strategy.md`

## Support & Resources

### Inside This Repo
- **Architecture questions:** Use `/research` or read `HEDGEHOG_RESEARCH.SKILL.md`
- **Implementation guidance:** Use `/feature` or read `.instructions.md`
- **Upstream helps:** Use `/upstream` or read `/memories/repo/qemu_upstream_merge_strategy.md`
- **Bug reports:** Use `/troubleshoot` or check `.instructions.md` troubleshooting section

### External Resources
- **QEMU upstream:** https://github.com/qemu/qemu
- **This fork:** https://github.com/EljakimHerrewijnen/qemu
- **Hedgehog (original):** https://github.com/elasticfuzz/hedgehog (inspiration)
- **Unicorn:** https://github.com/unicorn-engine/unicorn (similar purpose)

## Contributors

This documentation set was created to support sustainable development of the hedgehog module within the QEMU fork. Key areas covered:

✓ Complete architecture overview
✓ Implementation pattern guide
✓ Upstream merge strategy with conflict resolution
✓ Agent-based intelligent guidance
✓ Research skill for exploring codebase
✓ Quick-reference guides for common tasks

## Next Steps

1. **Read** `.instructions.md` (start immediately!)
2. **Reference** `/memories/repo/qemu_upstream_merge_strategy.md` when doing git operations
3. **Ask** the agent `/research`, `/feature`, `/upstream`, or `/troubleshoot` questions
4. **Contribute** new findings back to `/memories/repo/`

---

**Last Updated:** May 4, 2026
**Hedgehog Version:** v0.1.1a9
**QEMU Fork:** EljakimHerrewijnen/qemu

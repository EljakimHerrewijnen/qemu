# QEMU Hedgehog Development Agents & Customization

This document explains the agent setup, instructions, and skills available for working on the hedgehog module and QEMU codebase.

## Available Customization Files

### 1. `.instructions.md` (Core Development Guide)
**Location:** `/home/eljakim/Source/qemu/.instructions.md`

**Purpose:** Comprehensive development guide covering:
- Quick context (what is hedgehog, where it lives)
- Key architectural patterns (protocol-based backend, ctypes, hooks, coverage)
- Implementation patterns (how to add features)
- Execution models (board vs machine)
- **Upstream management** (git workflow, conflict resolution, merging strategy)
- Version tagging and releases
- Common tasks and troubleshooting

**When to Reference:**
- Starting work on a new feature
- Planning architectural changes
- Preparing for upstream merges
- Setting up developer environment
- Understanding design decisions

**Key Sections:**
- "Implementation Patterns to Follow"
- "Upstream Management: Merging QEMU Commits"
- "Common Implementation Tasks"
- "Troubleshooting"

### 2. `hedgehog-core.agent.md` (Agent Configuration)
**Location:** `/home/eljakim/Source/qemu/.github/agents/hedgehog-core.agent.md`

**Purpose:** Defines the QEMU Hedgehog Module Development agent
- Activates when working on files in hedgehog-related directories
- Defines slash commands for common workflows
- Lists expertise areas and responsibilities
- Sets constraints and limitations

**Auto-Activation:**
Triggered when editing files matching:
- `python/qemu/hedgehog/**`
- `accel/hedgehog/**`
- `include/system/hedgehog*.h`
- `accel/tcg/hedgehog*.c`
- Documentation files

**Slash Commands (in this agent context):**
- `/research` — Deep dive into a specific hedgehog feature or component
- `/feature` — Implement a new feature (Python or C)
- `/upstream` — Guide through merging upstream QEMU commits
- `/troubleshoot` — Debug build, runtime, or integration issues

**Example Usage:**
```
@copilot /research How does the hook synchronization work?
@copilot /feature Add a new hook type for conditional branches
@copilot /upstream Guide me through rebasing upstream QEMU
@copilot /troubleshoot Backend library not found error
```

### 3. `HEDGEHOG_RESEARCH.SKILL.md` (Codebase Research Skill)
**Location:** `/home/eljakim/Source/qemu/HEDGEHOG_RESEARCH.SKILL.md`

**Purpose:** Structured guidance for exploring the hedgehog codebase
- Explains how different components fit together
- Provides research workflows (hook flow, adding features, tracing coverage)
- Includes data flow diagrams
- Recommends code reading paths (quick/medium/deep dives)
- Lists research questions and how to answer them

**When to Use:**
- You're new to the codebase and want to understand it systematically
- Planning a complex feature that spans multiple layers
- Debugging issues across the Python-C boundary
- Preparing for major refactors or upstream merges

**Key Workflows:**
1. Understand hook execution flow
2. Add a new hook type
3. Understand board vs machine mode
4. Trace coverage tracking
5. Navigate between Python and C layers

### 4. Repository Memory: `qemu_upstream_merge_strategy.md`
**Location:** `/memories/repo/qemu_upstream_merge_strategy.md`

**Purpose:** Detailed upstream merge workflow and conflict resolution
- Repository structure implications for merging
- Three merge strategies (rebase, merge, cherry-pick)
- Step-by-step conflict resolution guide
- Pre/post-sync checklists
- Git aliases for convenience
- Release process
- Common problems & solutions

**When to Reference:**
- Planning an upstream merge
- During merge conflicts
- After merge to validate
- Setting up git workflow
- Publishing releases

**Key Sections:**
- "Merge Workflow Options"
- "Handling Conflicts: Step-By-Step"
- "Conflict Scenarios & Solutions"
- "Pre-Sync/Post-Sync Checklists"

### 5. `upstream-sync.agent.md` (Dedicated Upstream Sync Agent)
**Location:** `/home/eljakim/Source/qemu/.github/agents/upstream-sync.agent.md`

**Purpose:** Focused agent for syncing latest upstream QEMU while preserving hedgehog integration
- Plans sync strategy (rebase, merge, cherry-pick)
- Guides conflict resolution in TCG/build integration hotspots
- Runs post-sync validation commands

**When to Use:**
- Preparing regular upstream syncs
- Resolving sync conflicts in `accel/tcg/*` and build files
- Verifying fork stability after sync

**Slash Commands:**
- `/sync-upstream`
- `/resolve-conflicts`
- `/validate-sync`

### 6. `python-api-build-test.agent.md` (Python API Build/Test Agent)
**Location:** `/home/eljakim/Source/qemu/.github/agents/python-api-build-test.agent.md`

**Purpose:** Focused agent for wheel builds and Python API runtime validation
- Builds native backend libraries and Python wheels
- Verifies bundled native libraries in wheel artifacts
- Runs install/import/runtime smoke checks

**When to Use:**
- Preparing or validating Python package releases
- Debugging wheel packaging issues
- Running local build-and-test loops for `qemu.hedgehog`

**Slash Commands:**
- `/build-wheel`
- `/test-python-api`
- `/release-check`

### 7. `feature-orchestrator.agent.md` (Feature Planning Orchestrator)
**Location:** `/home/eljakim/Source/qemu/.github/agents/feature-orchestrator.agent.md`

**Purpose:** Orchestrates feature work in staged steps from source research to implementation and optional Python API exposure
- Enforces a research-first workflow with written artifacts
- Delegates merge-safe edit design to a proposal specialist
- Delegates Python API exposure to a dedicated Python specialist

**When to Use:**
- Starting any non-trivial new feature
- Planning work that must stay merge-friendly with upstream QEMU
- Coordinating cross-layer C + Python changes

**Slash Commands:**
- `/plan-feature`
- `/research-source`
- `/propose-edits`
- `/expose-python`

### 8. `qemu-feature-edit-proposal.agent.md` (Merge-Safe Edit Specialist)
**Location:** `/home/eljakim/Source/qemu/.github/agents/qemu-feature-edit-proposal.agent.md`

**Purpose:** Produces file-by-file proposal documents and patch-set boundaries designed for future upstream merges

**Output Artifact:**
- `docs/hedgehog/feature-proposals/<feature-name>.md`

### 9. `python-api-exposure.agent.md` (Python API Exposure Specialist)
**Location:** `/home/eljakim/Source/qemu/.github/agents/python-api-exposure.agent.md`

**Purpose:** Exposes backend capabilities through `qemu.hedgehog` API and aligns protocol/bridge/docs/tests changes

**Output Focus:**
- `python/qemu/hedgehog/api.py`
- `python/qemu/hedgehog/backend.py`
- `python/qemu/hedgehog/docs.md`

## Workflow Examples

### Scenario 1: "I'm adding support for a new hook type"

**Steps:**
1. Open `.instructions.md` → "Common Implementation Tasks" → "Task: Add a New Hook Type"
2. Follow the 6-step implementation path (C header, Python constants, backend, integration, docs)
3. Use `/feature` slash command to guide implementation
4. Reference `HEDGEHOG_RESEARCH.SKILL.md` → "Workflow 2: Add a New Hook Type" for detailed walkthrough
5. Use `backend.py` understanding to implement ctypes bridge

**Files Touched:** `hedgehog-exec-hooks.h`, `constants.py`, `backend.py`, `api.py`, `docs.md`

### Scenario 2: "I need to merge upstream QEMU commits"

**Steps:**
1. Open `/memories/repo/qemu_upstream_merge_strategy.md` for detailed strategy
2. Use git aliases from the file for one-command operations: `git sync-rebase`, `git hedgehog-build-test`
3. Use `.instructions.md` → "Upstream Management" for quick reference on git workflow
4. During conflicts, reference "Conflict Scenarios & Solutions" table in memory file
5. Use checklists (Pre-Sync/Post-Sync) from memory file to validate
6. Use `git hedgehog-test` alias to run full build validation

**Expected Conflicts:**
- `accel/tcg/cpu-exec.c` or `cputlb.c` (TCG changes)
- `meson.build` or `meson_options.txt` (build system)

### Scenario 3: "I'm new to hedgehog and want to understand the architecture"

**Steps:**
1. Start with `.instructions.md` → "Quick Context" and "Key Architectural Patterns"
2. Read `HEDGEHOG_RESEARCH.SKILL.md` → "For Quick Understanding" (30 min) or "For Medium Deep Dive" (2-3 hours)
3. Follow the recommended code reading path
4. Use research workflows to understand specific components
5. Use slash command `/research` to deep-dive specific areas
6. Reference `python/qemu/hedgehog/docs.md` for API examples

### Scenario 4: "I'm debugging a runtime issue with machine mode"

**Steps:**
1. Use `/troubleshoot` slash command
2. Reference `.instructions.md` → "Troubleshooting" section
3. Use `HEDGEHOG_RESEARCH.SKILL.md` → "Workflow 3" to understand board vs machine differences
4. Check `python/qemu/hedgehog/docs.md` examples for machine-backed mode (raspi3b)
5. Validate build with `git hedgehog-build-test` alias
6. Review chardev/QOM integration in `api.py` for device I/O issues

## File Organization Reference

```
/home/eljakim/Source/qemu/
├── .instructions.md              ← Core development guide (start here!)
├── .github/agents/               ← Copilot-discovered custom agents
│   ├── hedgehog-core.agent.md
│   ├── upstream-sync.agent.md
│   ├── python-api-build-test.agent.md
│   ├── feature-orchestrator.agent.md
│   ├── qemu-feature-edit-proposal.agent.md
│   └── python-api-exposure.agent.md
├── HEDGEHOG_RESEARCH.SKILL.md    ← Structured research guide
├── hedgehog.md                   ← High-level overview (read next)
├── hedgehog_quickstart.md        ← Quick start examples
├── python/qemu/hedgehog/
│   ├── api.py                    ← Main Hedgehog class (700 lines)
│   ├── backend.py                ← Backend protocol + NativeBackend (500 lines)
│   ├── constants.py              ← Enums and constants (150 lines)
│   ├── errors.py                 ← Exception handling (85 lines)
│   └── docs.md                   ← Python API reference
├── accel/hedgehog/               ← C backend implementation
├── include/system/hedgehog-*.h   ← C backend headers
├── accel/tcg/
│   ├── hedgehog-exec-hooks.c     ← Hook implementation
│   ├── cpu-exec.c                ← TB hook integration
│   └── cputlb.c                  ← Invalid memory hook integration
└── /memories/repo/
    └── qemu_upstream_merge_strategy.md ← Git merge strategy
```

## Agent Capabilities Reference

### Default Agent (Current)
Primary capabilities:
- General QEMU/C programming questions
- Build system questions
- Cross-platform compatibility issues
- Security analysis
- Code review

### QEMU Hedgehog Module Development Agent (Activated on hedgehog files)
Specialized capabilities:
- Hedgehog architecture and design
- Python-C interop and ctypes patterns
- Hook system and coverage tracking
- Board vs machine execution models
- Upstream merge strategy and conflict resolution
- Testing and validation approaches

**Activation:** Automatic when editing:
- `python/qemu/hedgehog/**`
- `accel/hedgehog/**`
- `include/system/hedgehog*.h`
- `accel/tcg/hedgehog*.c`

**Switch Context:** Use `/research` to trigger deep exploratory mode

### QEMU Upstream Sync and Rebase Agent
Specialized capabilities:
- Safe upstream synchronization planning and execution
- Conflict resolution for TCG hooks and Meson integration points
- Post-sync validation and release prep workflow

**Activation:** Automatic when editing:
- `accel/tcg/**`
- `meson.build`, `meson_options.txt`, `accel/meson.build`
- `include/system/**`

### Hedgehog Python API Build and Test Agent
Specialized capabilities:
- Native backend build orchestration for wheel packaging
- Python wheel build and bundled-library verification
- Clean-install and runtime smoke validation

**Activation:** Automatic when editing:
- `python/**`
- `python/qemu/hedgehog/**`
- `.github/workflows/hedgehog-release-wheels.yml`
- `tests/**`

### Hedgehog Feature Orchestrator
Specialized capabilities:
- Research-first feature planning with written artifacts
- Delegation to merge-safe edit proposal and Python exposure specialists
- Stepwise checkpoints for implementation and validation

**Activation:** Automatic when editing:
- `accel/**`
- `include/**`
- `python/qemu/hedgehog/**`
- `docs/**`

### QEMU Merge-Safe Edit Proposal Agent
Specialized capabilities:
- File-level edit planning and patch-set sequencing
- Conflict-minimizing boundaries for future upstream sync
- Validation and rollback criteria per patch set

### Hedgehog Python API Exposure Agent
Specialized capabilities:
- Pythonic API design for backend features
- Protocol and native bridge alignment
- API docs and smoke-test guidance

## Streamlined Feature Process

For new features, follow this pipeline:

1. Use `feature-orchestrator.agent.md` to create a staged plan.
2. Write source findings to `docs/hedgehog/feature-research/<feature-name>.md`.
3. Use `qemu-feature-edit-proposal.agent.md` to create merge-safe edit proposals in `docs/hedgehog/feature-proposals/<feature-name>.md`.
4. Implement C/backend changes in small patch sets.
5. If needed, use `python-api-exposure.agent.md` to expose the feature via `qemu.hedgehog`.
6. Validate with `python-api-build-test.agent.md` and (if syncing) `upstream-sync.agent.md`.

## Using This Setup Effectively

### For Coding Tasks
1. Open the file you're about to edit
2. Agent auto-activates if it's hedgehog-related
3. Ask specific questions (agent has full context)
4. Use `.instructions.md` for reference patterns
5. Reference `HEDGEHOG_RESEARCH.SKILL.md` for architecture questions

### For Git/Merge Tasks
1. Reference `/memories/repo/qemu_upstream_merge_strategy.md`
2. Use prepared git aliases for common operations
3. Follow the step-by-step conflict resolution guide
4. Run provided checklists before and after

### For Learning
1. Start with `.instructions.md` quick context
2. Move to `HEDGEHOG_RESEARCH.SKILL.md` for deep learning
3. Read `python/qemu/hedgehog/docs.md` for examples
4. Use `/research` command to explore specific areas

### For Problem Solving
1. Use `/troubleshoot` slash command
2. Check `.instructions.md` troubleshooting section
3. Reference relevant research workflow in `HEDGEHOG_RESEARCH.SKILL.md`
4. Validate with provided build/test aliases

## Keeping This Setup Current

When updating the codebase:
1. **Major feature:** Update both `.instructions.md` and `/memories/repo/qemu_upstream_merge_strategy.md` with new patterns
2. **New research findings:** Add to `/memories/repo/` with `hedgehog_*` prefix
3. **Merge conflicts:** Document solution and add to "Conflict Scenarios & Solutions" table
4. **New common tasks:** Add to `.instructions.md` "Common Implementation Tasks" section
5. **API changes:** Update `HEDGEHOG_RESEARCH.SKILL.md` data flow diagrams

## Quick Command Reference

```bash
# Setup upstream remote (one-time)
git remote add upstream https://github.com/qemu/qemu.git

# Convenient aliases (from git config)
git sync-rebase           # Fetch + rebase onto upstream/master
git sync-merge            # Fetch + merge upstream/master
git hedgehog-build-test   # Build hedgehog and validate
git hedgehog-py-test      # Quick Python import test
git status-sync           # See commits ahead of upstream

# Manual sync with full validation
./configure --enable-hedgehog --target-list=x86_64-softmmu,aarch64-softmmu
ninja -C build-hedgehog libqemu-hedgehog-backend.so libqemu-hedgehog-backend-aarch64.so
cd python && python -m pytest tests/  # If tests exist
```

## Next Steps

1. **Read** `.instructions.md` for overall context and patterns
2. **Skim** `HEDGEHOG_RESEARCH.SKILL.md` to understand research workflows
3. **Reference** `/memories/repo/qemu_upstream_merge_strategy.md` when doing git operations
4. **Ask** specific questions using the hedgehog agent (auto-activated on relevant files)
5. **Contribute** findings back to `/memories/repo/` for future reference

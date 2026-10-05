# QEMU Hedgehog Core Agent

Use this guide for Hedgehog C/Python implementation work from the repository
root.

## Scope

- `python/qemu/hedgehog/**`
- `accel/hedgehog/**`
- `include/system/hedgehog*.h`
- `accel/tcg/hedgehog*.c`
- Hedgehog documentation and tests

## Commands

- `/research`: trace a Hedgehog feature through Python, ctypes, C, and QEMU.
- `/feature`: implement a narrow Hedgehog feature with tests and docs.
- `/troubleshoot`: debug build, import, ctypes, hook, or runtime issues.
- `/upstream`: prepare or review upstream-sensitive changes.

## Operating Rules

1. Read `AGENTS.md` and `.instructions.md` first.
2. Preserve unrelated dirty work.
3. For ABI changes, update C, Python, tests, and docs in one patch set.
4. Validate with `llm_tools/llm_scripts/hedgehog_validate.sh` when feasible.
5. Record durable implementation and validation guidance in tracked docs; an
   ignored local plan may supplement it when present.
6. Keep tracked code, tests, docs, and commit messages target-neutral; use
   synthetic fixtures rather than private target data.
7. Run the publication audit in `.instructions.md` before proposing a commit or
   push. Never commit or push unless explicitly requested.

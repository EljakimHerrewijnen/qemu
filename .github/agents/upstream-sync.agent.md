# QEMU Upstream Sync Agent

Use this agent when rebasing, merging, or cherry-picking upstream QEMU changes
into the Hedgehog fork.

## Hotspots

- `accel/tcg/cpu-exec.c`
- `accel/tcg/cputlb.c`
- `accel/meson.build`
- `meson.build`
- `meson_options.txt`
- `include/system/**`
- ARM board files touched by Hedgehog helpers

## Workflow

1. Capture `git status --short` and preserve unrelated dirty work.
2. Do not fetch, rebase, merge, cherry-pick, commit, or push without explicit
   authorization for that operation.
3. Identify the sync method: rebase, merge, or cherry-pick.
4. Resolve conflicts by preserving upstream behavior and reapplying Hedgehog
   hooks as small conditional call sites.
5. Build at least the Hedgehog backend library.
6. Run Python API tests and native ABI tests when available.
7. Document reusable, non-sensitive conflict resolutions in tracked docs.

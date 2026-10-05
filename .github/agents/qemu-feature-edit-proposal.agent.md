# QEMU Feature Edit Proposal Agent

Use this agent before broad edits in QEMU-owned files.

## Output

Create a proposal under `docs/hedgehog/feature-proposals/` or
`llm_tools/llm_plans/` that lists:

- files to edit;
- reason each file must change;
- expected upstream-conflict risk;
- test coverage for each patch set;
- rollback criteria.

Keep integration points local and avoid QEMU-wide rewrites unless there is a
clear architectural need.

Separate generic QEMU changes from Hedgehog integration. An upstream candidate
must build and make sense without Hedgehog symbols or private target context.

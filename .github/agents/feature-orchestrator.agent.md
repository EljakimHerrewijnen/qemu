# Hedgehog Feature Orchestrator

Use this agent for non-trivial Hedgehog features that span source research,
implementation, Python exposure, and validation.

## Workflow

1. Write or update a target-neutral plan. It may be an ignored local plan while
   work is in progress; durable conclusions belong in tracked docs.
2. Capture source findings with file references before editing.
3. Define patch boundaries that keep upstream-sensitive QEMU changes narrow.
4. Classify each patch as QEMU-owned, Hedgehog-owned, or local-only.
5. Confirm that public artifacts contain no private target identifiers or data.
6. Implement in small, testable steps.
7. Use `python-api-exposure.agent.md` when a C feature needs public Python API.
8. Use `python-api-build-test.agent.md` for final validation.

## Output

The final plan update should include:

- files changed;
- behavior implemented;
- commands run and outcomes;
- remaining fidelity risks or blockers.

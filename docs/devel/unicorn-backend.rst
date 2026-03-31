============================
Unicorn-like backend proposal
============================

.. note::

   This document is a design plan for adding a Unicorn-like backend to
   QEMU. It intentionally focuses on the smallest viable integration
   points so that the backend can live close to upstream QEMU instead of
   becoming a deep fork of the project.

Goals
-----

The backend should make it possible to embed QEMU as a CPU emulator with
an API that looks closer to Unicorn than to the system emulator command
line. The initial design should:

* reuse existing ``target/*`` CPU implementations instead of creating a
  new forked target tree;
* keep the guest memory model based on ``MemoryRegion`` and
  ``AddressSpace`` so that RAM and MMIO continue to use normal QEMU
  mechanisms;
* keep the change set local to a new optional backend plus a small number
  of narrow TCG hook points;
* allow hook registration for code execution, memory access, invalid
  memory, and interrupts/exceptions;
* compile out cleanly when the feature is disabled.

Non-goals
---------

The first step should not try to expose all of QEMU machine creation,
device models, migration, block, or monitor features. A Unicorn-like
backend is best treated as an embedding API layered on top of existing
CPU and memory subsystems, not as a new system emulator binary.

High-level shape
----------------

The lowest-risk design is to add a small optional backend that builds a
CPU plus a private address space without introducing a new architecture
fork.

1. Add a new optional subsystem, for example ``accel/unicorn/``, that
   owns the embedding API and lifecycle.
2. Reuse existing target CPUs from ``target/*`` by creating them through
   QOM, exactly like other accelerators and test harnesses do.
3. Construct a minimal board/container object that only provides:

   * one CPU instance;
   * one root RAM region;
   * optional MMIO subregions for callback-backed devices;
   * reset/run/stop helpers for the embedding API.

4. Keep all of this behind a dedicated build flag, for example
   ``CONFIG_UNICORN_BACKEND``.

This keeps target-specific logic where it already lives today, and keeps
the new code responsible only for embedding, memory mapping, and hook
dispatch.

Minimal object model
--------------------

The backend does not need a full machine model at first. A small QOM
container is enough:

* ``TYPE_UNICORN_BACKEND``:
  owns the API state, guest address space, RAM blocks, and hook lists.
* ``TYPE_UNICORN_DEVICE``:
  a simple ``SysBusDevice`` or ``DeviceState``-derived helper that backs
  callback-driven MMIO ranges.

The important point is that the backend should use standard QEMU objects
instead of maintaining a second memory/device model. That lets RAM, TLB
fills, exceptions, and target CPU state continue to flow through the
normal QEMU code paths.

Build and configuration strategy
--------------------------------

This backend should be optional and isolated behind build-time guards.
The smallest upstream-friendly shape is:

* add a meson feature option, disabled by default;
* translate that option into ``CONFIG_UNICORN_BACKEND``;
* place the new sources in a dedicated ``accel/unicorn/`` subtree;
* avoid touching unrelated target or device code unless a hook must run
  on every TCG execution path.

Using ifdefs in only a few files is preferable to carrying large target
patch stacks. The backend should compile out entirely when the option is
disabled.

Why reuse QEMU targets instead of adding a Unicorn-only target
--------------------------------------------------------------

Creating a separate ``target/unicorn-*`` tree would immediately duplicate
instruction decoding, CPU state definitions, helpers, and exception
handling. That is the same maintenance burden that made the historic
Unicorn fork diverge heavily.

Instead, the backend should instantiate existing CPU types from
``target/arm/``, ``target/aarch64/``, ``target/i386/``, and so on. The
backend API can decide which CPU type to create, but once the CPU exists,
execution should stay in normal QEMU target code.

This means the "base device" for a Unicorn-like implementation is not a
new guest architecture; it is a small host-side container that wraps an
existing QEMU CPU and address space.

TCG hook placement
------------------

The main requirement is to support Unicorn-style hooks without rewriting
all translators. There are three layers of hook support, listed in
preferred order.

Use existing plugin-style instrumentation first
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

QEMU already has TCG instrumentation support documented in
``docs/devel/tcg-plugins.rst`` and implemented in files such as
``plugins/api.c`` and ``include/plugins/qemu-plugin.h``. That
infrastructure already understands:

* translation block callbacks;
* per-instruction execution callbacks;
* successful memory access callbacks.

For a Unicorn backend, the cleanest first step is to reuse the same style
of callback dispatch internally instead of teaching every target
translator about a second hook ABI.

Add narrow execution shims in ``accel/tcg/cpu-exec.c`` only when needed
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The main TCG execution loop in ``accel/tcg/cpu-exec.c`` is the best place
for backend-owned control hooks that are not naturally covered by plugin
callbacks. In particular, ``cpu_exec_loop()``, ``cpu_loop_exec_tb()``,
``cpu_tb_exec()``, and the ``tb_gen_code()`` path are the concrete
functions to study first. Typical backend-owned control hooks there are:

* stop-after-N-instructions or stop-at-address checks before entering the
  next translation block;
* run-loop exit reasons that must be returned synchronously to the
  embedding API;
* block-level enter/leave notifications when the backend wants lower
  overhead than per-instruction callbacks.

These hooks should stay narrow: helper calls around translation-block
execution are acceptable, but target-specific translator changes should
be avoided unless a target exposes state that cannot otherwise be read.

Use ``accel/tcg/cputlb.c`` only for memory semantics that plugins cannot
express
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Memory hooks in Unicorn are often expected to observe unmapped or
protection-fault accesses, not just successful reads and writes. The
plugin API mostly reports successful accesses, so the backend will
probably need one additional TCG/MMU integration layer.

The best place for that is the TLB/MMU path in ``accel/tcg/cputlb.c``,
because that is where QEMU already resolves guest accesses and classifies
RAM versus MMIO versus faulting operations. Even before choosing an exact
helper, that file is the right layer because it already owns TLB fill,
flush, and miss/fault handling. A small backend callback hook there can
provide:

* invalid read/write/fetch notifications;
* page permission failures;
* a synchronous way to stop execution before QEMU converts the condition
  into a guest exception visible to the embedding API.

Avoid direct translator edits unless a hook needs guest ISA details
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Most Unicorn hooks do not require translator changes. Direct edits under
``target/*/translate.c`` should be a last resort for architecture-specific
features such as precise instruction metadata that cannot be reconstructed
from the translated block or plugin callbacks.

Phased implementation plan
--------------------------

Phase 1: backend skeleton
~~~~~~~~~~~~~~~~~~~~~~~~~

Add the new optional backend, but keep it as a private developer-facing
API first. It should be able to:

* create a CPU of a requested QOM type;
* create an address space with RAM and MMIO regions;
* set/reset PC and registers through existing CPU class helpers;
* run until stop/exception/limit.

Phase 2: callback-backed MMIO device
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Add the minimal "base device" used by the backend for MMIO mapping. It
should expose standard ``MemoryRegionOps`` callbacks and forward read and
write events into backend-owned hook tables. This avoids inventing a
parallel device model for memory-mapped callbacks.

Phase 3: TCG execution hooks
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Start with translation-block and instruction hooks using the same overall
shape as the plugin subsystem. Only add a new helper in
``accel/tcg/cpu-exec.c`` if the backend needs faster block-level
termination checks than a plugin-style path can provide.

Phase 4: invalid memory and exception hooks
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Add a narrow callback path in ``accel/tcg/cputlb.c`` so the backend can
turn QEMU MMU failures into Unicorn-like status codes and hook callbacks.

Phase 5: architecture polish
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Only after the backend skeleton works should target-specific register
access helpers or architecture-specific convenience APIs be added.

Phase 6: Python API parity
~~~~~~~~~~~~~~~~~~~~~~~~~~

After the C backend is stable, add a Python package that mirrors the
capabilities of Unicorn's Python API on top of the in-tree backend. That
phase should:

* live under ``python/qemu/unicorn/`` so it can reuse QEMU's existing
  Python packaging layout;
* wrap the phase-1 C API first, and only add higher-level Python helpers
  once the low-level surface is stable;
* expose the same core concepts users expect from Unicorn's Python API:
  emulator construction, memory map/read/write, register access, bounded
  execution, and hook registration;
* provide compatibility shims for Unicorn-style constants and exception
  types so Python users can port code incrementally rather than
  rewriting everything around QEMU-specific types.

Files to study while implementing
---------------------------------

The following files are the most relevant starting points:

* ``accel/tcg/cpu-exec.c`` for ``cpu_exec_loop()``, ``cpu_loop_exec_tb()``,
  ``cpu_tb_exec()``, and ``tb_gen_code()``;
* ``accel/tcg/cputlb.c`` for memory access, TLB, and MMU fault handling;
* ``include/hw/core/cpu.h`` for generic CPU interfaces;
* ``hw/core/qdev.c`` and ``hw/core/sysbus.c`` for the standard device
  model;
* ``hw/misc/unimp.c`` for a small ``MemoryRegionOps``-based device
  example;
* ``target/*/cpu.c`` for target CPU creation and reset helpers;
* ``plugins/api.c`` and ``include/plugins/qemu-plugin.h`` for the
  existing callback model.

Recommended first code changes
------------------------------

To keep the first implementation small and reviewable, the first code
series after this plan should do only the following:

* add the optional build flag and backend directory;
* create the backend/container object and MMIO callback device;
* wire in one block-execution hook and one invalid-memory hook;
* expose a tiny C API for create/map/run/stop.

Everything else should remain a follow-up. That keeps the backend close
to upstream QEMU and avoids the large-scale divergence that made Unicorn
hard to rebase historically.

===========
QEMU README
===========

This repository is a fork of QEMU.  It follows upstream QEMU closely while
carrying an additional in-tree addon named ``hedgehog``.

Upstream QEMU is a generic and open source machine & userspace emulator and
virtualizer.

QEMU is capable of emulating a complete machine in software without any
need for hardware virtualization support. By using dynamic translation,
it achieves very good performance. QEMU can also integrate with the Xen
and KVM hypervisors to provide emulated hardware while allowing the
hypervisor to manage the CPU. With hypervisor support, QEMU can achieve
near native performance for CPUs. When QEMU emulates CPUs directly it is
capable of running operating systems made for one machine (e.g. an ARMv7
board) on a different machine (e.g. an x86_64 PC board).

QEMU is also capable of providing userspace API virtualization for Linux
and BSD kernel interfaces. This allows binaries compiled against one
architecture ABI (e.g. the Linux PPC64 ABI) to be run on a host using a
different architecture ABI (e.g. the Linux x86_64 ABI). This does not
involve any hardware emulation, simply CPU and syscall emulation.

QEMU aims to fit into a variety of use cases. It can be invoked directly
by users wishing to have full control over its behaviour and settings.
It also aims to facilitate integration into higher level management
layers, by providing a stable command line interface and monitor API.
It is commonly invoked indirectly via the libvirt library when using
open source applications such as oVirt, OpenStack and virt-manager.

About This Fork
===============

This fork is intended to stay close to the official QEMU repository while
adding Hedgehog-specific embedding support.

In practice that means:

* the repository still contains the normal QEMU codebase and build system;
* upstream QEMU remains the reference project for the core emulator;
* this fork carries Hedgehog changes on top, and tries to keep tracking
  upstream QEMU rather than diverging into a separate emulator;
* large parts of the Hedgehog code were generated or iterated with LLM
  assistance, so the feature should be treated as experimental and reviewed
  with extra care.

Hedgehog is the main reason this fork exists.  It adds a Unicorn-like C and
Python embedding API so QEMU can be driven as a library instead of only as a
standalone process.

The current Hedgehog goal is to make QEMU usable as a Python module for tasks
such as:

* mapping memory and loading guest code;
* reading and writing registers and memory;
* running for bounded instruction budgets to approximate stepping;
* adding execution and invalid-memory hooks;
* reusing selected QEMU machine models and device trees from Python.

QEMU as a whole is released under the GNU General Public License,
version 2. For full licensing details, consult the LICENSE file.


Documentation
=============

Documentation can be found hosted online at
`<https://www.qemu.org/documentation/>`_. The documentation for the
current development version that is available at
`<https://www.qemu.org/docs/master/>`_ is generated from the ``docs/``
folder in the source tree, and is built by `Sphinx
<https://www.sphinx-doc.org/en/master/>`_.


Building
========

QEMU is multi-platform software intended to be buildable on all modern
Linux platforms, OS-X, Win32 (via the Mingw64 toolchain) and a variety
of other UNIX targets. The simple steps to build QEMU are:


.. code-block:: shell

  mkdir build
  cd build
  ../configure
  make

Additional information can also be found online via the QEMU website:

* `<https://wiki.qemu.org/Hosts/Linux>`_
* `<https://wiki.qemu.org/Hosts/Mac>`_
* `<https://wiki.qemu.org/Hosts/W32>`_


Embedding API (Hedgehog backend)
================================

This fork adds an optional Hedgehog backend that lets you use QEMU's TCG CPU
emulation as a library rather than only as a standalone process.  It is aimed
at firmware analysis, emulator-style embedding, architecture experiments, and
focused test harnesses.

The Python layer exposes a Unicorn-like API under ``qemu.hedgehog``.  It is
meant to feel familiar for scripted emulation workloads while still reusing
QEMU CPUs, memory handling, and selected board/device models.

Building with the embedding API enabled
---------------------------------------

Pass ``--enable-hedgehog`` to ``configure`` together with the system targets
you want to embed.  When the option is disabled, the backend compiles out.

.. code-block:: shell

  mkdir build-hedgehog
  cd build-hedgehog
  ../configure --enable-hedgehog --target-list=x86_64-softmmu,aarch64-softmmu
  make

The public C header is ``include/system/hedgehog-backend.h``.

The build emits a loadable backend library at:

* ``build-hedgehog/libqemu-hedgehog-backend.so``
* ``build-hedgehog/libqemu-hedgehog-backend-aarch64.so`` when
  ``aarch64-softmmu`` is enabled

Board-backed mode
-----------------

Board-backed mode gives you a single CPU and a private address space that you
populate yourself with RAM and MMIO callback regions.  Use it when you want a
small emulator surface and do not need a full machine model.

The core C API supports:

* backend lifecycle
* RAM and MMIO mapping
* guest memory reads and writes
* register access
* bounded execution and stop requests
* translation-block, instruction, and invalid-memory hooks

The bounded-run interface is also the current way to do stepping-style control
from Python: run for a small instruction budget, inspect state, then continue.

Machine-backed mode
-------------------

Machine-backed mode instantiates a real QEMU machine type and uses that
machine's existing address space and device models.  Use it when your firmware
expects a specific board layout such as ``raspi3b``.

In this mode:

* ``machine_type`` selects the board model
* memory access goes through the machine's real device tree
* manual ``map_ram`` and ``map_mmio`` overlays are not supported
* changing machine type within one process is not supported reliably today

Python API
----------

This fork also provides a Hedgehog-compatible Python package under
``python/``.  Install it into a virtual environment with:

.. code-block:: shell

  cd python
  python3 -m venv .venv
  source .venv/bin/activate
  pip install -e .

The Python entry point is ``qemu.hedgehog.Hedgehog``.  It auto-loads bundled
or explicitly configured backend libraries via ``QEMU_HEDGEHOG_BACKEND_LIBRARY``.
In machine-backed mode it also supports named chardev creation,
constructor-time QOM property bindings, legacy serial-slot attachment,
endpoint discovery for backends such as PTYs, and explicit event pumping via
``qemu_events_poll()``.

.. code-block:: python

  from qemu.hedgehog import Hedgehog, HEDGEHOG_ARCH_ARM64, HEDGEHOG_MODE_ARM

  emu = Hedgehog(
      HEDGEHOG_ARCH_ARM64,
      HEDGEHOG_MODE_ARM,
      cpu_type="cortex-a53",
      machine_type="raspi3b",
      chardevs={"console": "pty"},
      property_bindings={
          "/machine/soc/peripherals/uart0": {"chardev": "console"},
      },
  )
  print(emu.qemu_chardev_get_endpoint("console"))

Further reading
---------------

Additional Hedgehog documentation in this tree:

* ``hedgehog.md`` for an implementation and build overview
* ``hedgehog_quickstart.md`` for out-of-tree Python usage
* ``python/qemu/hedgehog/docs.md`` for the Python API surface
* ``docs/devel/hedgehog-backend.rst`` for the backend design and architecture


Submitting patches
==================

The QEMU source code is maintained under the GIT version control system.

.. code-block:: shell

   git clone https://gitlab.com/qemu-project/qemu.git

When submitting patches, one common approach is to use 'git
format-patch' and/or 'git send-email' to format & send the mail to the
qemu-devel@nongnu.org mailing list. All patches submitted must contain
a 'Signed-off-by' line from the author. Patches should follow the
guidelines set out in the `style section
<https://www.qemu.org/docs/master/devel/style.html>`_ of
the Developers Guide.

Additional information on submitting patches can be found online via
the QEMU website:

* `<https://wiki.qemu.org/Contribute/SubmitAPatch>`_
* `<https://wiki.qemu.org/Contribute/TrivialPatches>`_

The QEMU website is also maintained under source control.

.. code-block:: shell

  git clone https://gitlab.com/qemu-project/qemu-web.git

* `<https://www.qemu.org/2017/02/04/the-new-qemu-website-is-up/>`_

A 'git-publish' utility was created to make above process less
cumbersome, and is highly recommended for making regular contributions,
or even just for sending consecutive patch series revisions. It also
requires a working 'git send-email' setup, and by default doesn't
automate everything, so you may want to go through the above steps
manually for once.

For installation instructions, please go to:

*  `<https://github.com/stefanha/git-publish>`_

The workflow with 'git-publish' is:

.. code-block:: shell

  $ git checkout master -b my-feature
  $ # work on new commits, add your 'Signed-off-by' lines to each
  $ git publish

Your patch series will be sent and tagged as my-feature-v1 if you need to refer
back to it in the future.

Sending v2:

.. code-block:: shell

  $ git checkout my-feature # same topic branch
  $ # making changes to the commits (using 'git rebase', for example)
  $ git publish

Your patch series will be sent with 'v2' tag in the subject and the git tip
will be tagged as my-feature-v2.

Bug reporting
=============

The QEMU project uses GitLab issues to track bugs. Bugs
found when running code built from QEMU git or upstream released sources
should be reported via:

* `<https://gitlab.com/qemu-project/qemu/-/issues>`_

If using QEMU via an operating system vendor pre-built binary package, it
is preferable to report bugs to the vendor's own bug tracker first. If
the bug is also known to affect latest upstream code, it can also be
reported via GitLab.

For additional information on bug reporting consult:

* `<https://wiki.qemu.org/Contribute/ReportABug>`_


ChangeLog
=========

For version history and release notes, please visit
`<https://wiki.qemu.org/ChangeLog/>`_ or look at the git history for
more detailed information.


Contact
=======

The QEMU community can be contacted in a number of ways, with the two
main methods being email and IRC:

* `<mailto:qemu-devel@nongnu.org>`_
* `<https://lists.nongnu.org/mailman/listinfo/qemu-devel>`_
* #qemu on irc.oftc.net

Information on additional methods of contacting the community can be
found online via the QEMU website:

* `<https://wiki.qemu.org/Contribute/StartHere>`_

"""
QEMU Hedgehog-compatible Python package.

This package mirrors Hedgehog's core Python concepts while targeting the
in-tree QEMU Hedgehog backend implementation.
"""

# Copyright (C) 2026 Red Hat Inc.
#
# This work is licensed under the terms of the GNU GPL, version 2.  See
# the COPYING file in the top-level directory.

from . import constants
from .api import Hedgehog
from .backend import BackendProtocol, NativeBackend
from .errors import HedgehogError, hedgehog_strerror


for _name in constants.__all__:
    globals()[_name] = getattr(constants, _name)


__all__ = (
    'Hedgehog',
    'HedgehogError',
    'hedgehog_strerror',
    'BackendProtocol',
    'NativeBackend',
) + constants.__all__

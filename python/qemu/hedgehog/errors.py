"""
Hedgehog-compatible exception helpers for qemu.hedgehog.
"""

# Copyright (C) 2026 Red Hat Inc.
#
# This work is licensed under the terms of the GNU GPL, version 2.  See
# the COPYING file in the top-level directory.

from typing import Dict

from .constants import (
    HEDGEHOG_ERR_ARCH,
    HEDGEHOG_ERR_ARG,
    HEDGEHOG_ERR_EXCEPTION,
    HEDGEHOG_ERR_FETCH_PROT,
    HEDGEHOG_ERR_FETCH_UNMAPPED,
    HEDGEHOG_ERR_HANDLE,
    HEDGEHOG_ERR_HOOK,
    HEDGEHOG_ERR_HOOK_EXIST,
    HEDGEHOG_ERR_INSN_INVALID,
    HEDGEHOG_ERR_MAP,
    HEDGEHOG_ERR_MODE,
    HEDGEHOG_ERR_NOMEM,
    HEDGEHOG_ERR_OK,
    HEDGEHOG_ERR_READ_PROT,
    HEDGEHOG_ERR_READ_UNALIGNED,
    HEDGEHOG_ERR_READ_UNMAPPED,
    HEDGEHOG_ERR_RESOURCE,
    HEDGEHOG_ERR_VERSION,
    HEDGEHOG_ERR_WRITE_PROT,
    HEDGEHOG_ERR_WRITE_UNALIGNED,
    HEDGEHOG_ERR_WRITE_UNMAPPED,
)


_UC_ERROR_MESSAGES: Dict[int, str] = {
    HEDGEHOG_ERR_OK: 'OK',
    HEDGEHOG_ERR_NOMEM: 'Out of memory',
    HEDGEHOG_ERR_ARCH: 'Unsupported architecture',
    HEDGEHOG_ERR_HANDLE: 'Invalid handle',
    HEDGEHOG_ERR_MODE: 'Unsupported mode',
    HEDGEHOG_ERR_VERSION: 'Version mismatch',
    HEDGEHOG_ERR_READ_UNMAPPED: 'Invalid memory read (unmapped)',
    HEDGEHOG_ERR_WRITE_UNMAPPED: 'Invalid memory write (unmapped)',
    HEDGEHOG_ERR_FETCH_UNMAPPED: 'Invalid memory fetch (unmapped)',
    HEDGEHOG_ERR_HOOK: 'Invalid hook type',
    HEDGEHOG_ERR_INSN_INVALID: 'Invalid instruction',
    HEDGEHOG_ERR_MAP: 'Memory mapping failed',
    HEDGEHOG_ERR_WRITE_PROT: 'Invalid memory write (protected)',
    HEDGEHOG_ERR_READ_PROT: 'Invalid memory read (protected)',
    HEDGEHOG_ERR_FETCH_PROT: 'Invalid memory fetch (protected)',
    HEDGEHOG_ERR_ARG: 'Invalid argument',
    HEDGEHOG_ERR_READ_UNALIGNED: 'Invalid memory read (unaligned)',
    HEDGEHOG_ERR_WRITE_UNALIGNED: 'Invalid memory write (unaligned)',
    HEDGEHOG_ERR_HOOK_EXIST: 'Hook already exists',
    HEDGEHOG_ERR_RESOURCE: 'Resource not available',
    HEDGEHOG_ERR_EXCEPTION: 'Unhandled CPU exception',
}


def hedgehog_strerror(errno: int) -> str:
    """
    Return a user-facing error string for a Hedgehog-compatible errno.
    """
    return _UC_ERROR_MESSAGES.get(errno, f'Unknown error {errno}')


class HedgehogError(Exception):
    """
    Hedgehog-style exception carrying an errno code.
    """

    def __init__(self, errno: int, message: str = ''):
        self.errno = int(errno)
        self.message = message or hedgehog_strerror(self.errno)
        super().__init__(self.message)

    def __str__(self) -> str:
        return f'{self.message} (errno={self.errno})'


__all__ = (
    'HedgehogError',
    'hedgehog_strerror',
)

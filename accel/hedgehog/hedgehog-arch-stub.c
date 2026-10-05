/*
 * Target-independent fallback for the Hedgehog architecture adapter
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hedgehog-internal.h"

const HedgehogArchOps *hedgehog_arch_ops(void)
{
    return NULL;
}

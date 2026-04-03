/*
 * Hedgehog backend MMIO callback device
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ACCEL_HEDGEHOG_MMIO_DEVICE_H
#define ACCEL_HEDGEHOG_MMIO_DEVICE_H

#include <stdint.h>

#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "system/hedgehog-backend.h"

#define TYPE_HEDGEHOG_MMIO_DEVICE "hedgehog-mmio-device"
OBJECT_DECLARE_SIMPLE_TYPE(HedgehogMMIODevice, HEDGEHOG_MMIO_DEVICE)

void hedgehog_mmio_device_configure(HedgehogMMIODevice *dev,
                                   const char *name,
                                   uint64_t size,
                                   HedgehogMMIOReadFunc read_fn,
                                   HedgehogMMIOWriteFunc write_fn,
                                   void *opaque);

MemoryRegion *hedgehog_mmio_device_region(HedgehogMMIODevice *dev);

#endif
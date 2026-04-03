/*
 * Hedgehog backend MMIO callback device
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "qemu/module.h"
#include "qapi/error.h"

#include "hedgehog-mmio-device.h"

struct HedgehogMMIODevice {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    char *name;
    uint64_t size;
    HedgehogMMIOReadFunc read_fn;
    HedgehogMMIOWriteFunc write_fn;
    void *opaque;
};

static uint64_t hedgehog_mmio_device_read(void *opaque,
                                         hwaddr addr,
                                         unsigned size)
{
    HedgehogMMIODevice *s = HEDGEHOG_MMIO_DEVICE(opaque);

    if (!s->read_fn) {
        return 0;
    }

    return s->read_fn(s->opaque, addr, size);
}

static void hedgehog_mmio_device_write(void *opaque,
                                      hwaddr addr,
                                      uint64_t value,
                                      unsigned size)
{
    HedgehogMMIODevice *s = HEDGEHOG_MMIO_DEVICE(opaque);

    if (s->write_fn) {
        s->write_fn(s->opaque, addr, value, size);
    }
}

static const MemoryRegionOps hedgehog_mmio_device_ops = {
    .read = hedgehog_mmio_device_read,
    .write = hedgehog_mmio_device_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

void hedgehog_mmio_device_configure(HedgehogMMIODevice *dev,
                                   const char *name,
                                   uint64_t size,
                                   HedgehogMMIOReadFunc read_fn,
                                   HedgehogMMIOWriteFunc write_fn,
                                   void *opaque)
{
    g_assert(dev);

    g_free(dev->name);
    dev->name = g_strdup(name);
    dev->size = size;
    dev->read_fn = read_fn;
    dev->write_fn = write_fn;
    dev->opaque = opaque;
}

MemoryRegion *hedgehog_mmio_device_region(HedgehogMMIODevice *dev)
{
    g_assert(dev);
    return &dev->iomem;
}

static void hedgehog_mmio_device_realize(DeviceState *dev, Error **errp)
{
    HedgehogMMIODevice *s = HEDGEHOG_MMIO_DEVICE(dev);

    if (s->size == 0) {
        error_setg(errp, "MMIO device size must be non-zero");
        return;
    }

    memory_region_init_io(&s->iomem, OBJECT(s), &hedgehog_mmio_device_ops, s,
                          s->name ?: "hedgehog-mmio", s->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static void hedgehog_mmio_device_finalize(Object *obj)
{
    HedgehogMMIODevice *s = HEDGEHOG_MMIO_DEVICE(obj);

    g_free(s->name);
}

static void hedgehog_mmio_device_class_init(ObjectClass *klass,
                                           const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    (void)data;

    dc->realize = hedgehog_mmio_device_realize;
}

static const TypeInfo hedgehog_mmio_device_info = {
    .name = TYPE_HEDGEHOG_MMIO_DEVICE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(HedgehogMMIODevice),
    .instance_finalize = hedgehog_mmio_device_finalize,
    .class_init = hedgehog_mmio_device_class_init,
};

static void hedgehog_mmio_device_register_types(void)
{
    type_register_static(&hedgehog_mmio_device_info);
}

type_init(hedgehog_mmio_device_register_types)
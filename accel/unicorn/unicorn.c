/*
 * Minimal Unicorn-like embedding backend
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qom/object.h"
#include "qapi/error.h"
#include "hw/core/qdev.h"
#include "hw/core/cpu.h"
#include "exec/cpu-common.h"
#include "system/cpus.h"
#include "system/memory.h"
#include "system/tcg.h"
#include "system/unicorn-backend.h"
#include "tcg/startup.h"
#include "accel/tcg/internal-common.h"

typedef struct UnicornRAMRegion {
    MemoryRegion mr;
} UnicornRAMRegion;

typedef struct UnicornMMIORegion {
    MemoryRegion mr;
    UnicornMMIOReadFunc read_fn;
    UnicornMMIOWriteFunc write_fn;
    void *opaque;
} UnicornMMIORegion;

struct UnicornBackend {
    CPUState *cpu;
    MemoryRegion root;
    AddressSpace as;
    GPtrArray *ram_regions;
    GPtrArray *mmio_regions;
    bool stop_requested;
};

static uint64_t unicorn_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    UnicornMMIORegion *region = opaque;

    if (!region->read_fn) {
        return 0;
    }

    return region->read_fn(region->opaque, addr, size);
}

static void unicorn_mmio_write(void *opaque, hwaddr addr,
                               uint64_t value, unsigned size)
{
    UnicornMMIORegion *region = opaque;

    if (region->write_fn) {
        region->write_fn(region->opaque, addr, value, size);
    }
}

static const MemoryRegionOps unicorn_mmio_ops = {
    .read = unicorn_mmio_read,
    .write = unicorn_mmio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

bool unicorn_backend_initialize(Error **errp)
{
    static gsize initialized;
    bool have_machine;

    have_machine = object_resolve_path_component(object_get_root(),
                                                 "machine") != NULL;

    if (g_once_init_enter(&initialized)) {
        if (!have_machine) {
            module_call_init(MODULE_INIT_QOM);
            qemu_init_cpu_list();
            qemu_init_cpu_loop();
            cpu_exec_init_all();
            qdev_create_fake_machine();

            tcg_allowed = true;
            page_init();
            tb_htable_init();
            tcg_init(32 * MiB, 0, 1);
            tcg_prologue_init();
        }

        g_once_init_leave(&initialized, 1);
    }

    if (!tcg_enabled()) {
        error_setg(errp, "the unicorn backend requires initialized TCG support");
        return false;
    }

    return true;
}

UnicornBackend *unicorn_backend_new(const char *cpu_type, Error **errp)
{
    g_autoptr(Error) local_err = NULL;
    UnicornBackend *uc;
    Object *cpuobj;

    if (!cpu_type) {
        error_setg(errp, "a concrete CPU type is required");
        return NULL;
    }

    if (!unicorn_backend_initialize(&local_err)) {
        error_propagate(errp, g_steal_pointer(&local_err));
        return NULL;
    }

    uc = g_new0(UnicornBackend, 1);
    uc->ram_regions = g_ptr_array_new_with_free_func(g_free);
    uc->mmio_regions = g_ptr_array_new_with_free_func(g_free);

    memory_region_init(&uc->root, NULL, "unicorn-memory", UINT64_MAX);
    address_space_init(&uc->as, &uc->root, "unicorn-memory");

    cpuobj = object_new(cpu_type);
    object_property_set_link(cpuobj, "memory", OBJECT(&uc->root), &local_err);
    if (local_err) {
        error_propagate(errp, g_steal_pointer(&local_err));
        object_unref(cpuobj);
        unicorn_backend_free(uc);
        return NULL;
    }

    if (!qdev_realize(DEVICE(cpuobj), NULL, &local_err)) {
        error_propagate(errp, g_steal_pointer(&local_err));
        object_unref(cpuobj);
        unicorn_backend_free(uc);
        return NULL;
    }

    uc->cpu = CPU(cpuobj);
    unicorn_backend_reset(uc);
    return uc;
}

void unicorn_backend_free(UnicornBackend *uc)
{
    if (!uc) {
        return;
    }

    if (uc->cpu) {
        object_unparent(OBJECT(uc->cpu));
        object_unref(OBJECT(uc->cpu));
    }

    address_space_destroy(&uc->as);
    g_ptr_array_free(uc->mmio_regions, true);
    g_ptr_array_free(uc->ram_regions, true);
    g_free(uc);
}

bool unicorn_backend_map_ram(UnicornBackend *uc, const char *name,
                             hwaddr addr, uint64_t size, Error **errp)
{
    UnicornRAMRegion *region;

    if (!uc || !size) {
        error_setg(errp, "RAM mapping requires a backend and non-zero size");
        return false;
    }

    region = g_new0(UnicornRAMRegion, 1);
    if (!memory_region_init_ram_flags_nomigrate(&region->mr, NULL,
                                                name ?: "unicorn-ram",
                                                size, 0, errp)) {
        g_free(region);
        return false;
    }

    memory_region_add_subregion(&uc->root, addr, &region->mr);
    g_ptr_array_add(uc->ram_regions, region);
    return true;
}

bool unicorn_backend_map_mmio(UnicornBackend *uc, const char *name,
                              hwaddr addr, uint64_t size,
                              UnicornMMIOReadFunc read_fn,
                              UnicornMMIOWriteFunc write_fn,
                              void *opaque, Error **errp)
{
    UnicornMMIORegion *region;

    if (!uc || !size) {
        error_setg(errp, "MMIO mapping requires a backend and non-zero size");
        return false;
    }

    region = g_new0(UnicornMMIORegion, 1);
    region->read_fn = read_fn;
    region->write_fn = write_fn;
    region->opaque = opaque;

    memory_region_init_io(&region->mr, NULL, &unicorn_mmio_ops, region,
                          name ?: "unicorn-mmio", size);
    memory_region_add_subregion(&uc->root, addr, &region->mr);
    g_ptr_array_add(uc->mmio_regions, region);
    return true;
}

MemTxResult unicorn_backend_mem_read(UnicornBackend *uc, hwaddr addr,
                                     void *buf, hwaddr len)
{
    g_assert(uc);
    return address_space_read(&uc->as, addr, MEMTXATTRS_UNSPECIFIED, buf, len);
}

MemTxResult unicorn_backend_mem_write(UnicornBackend *uc, hwaddr addr,
                                      const void *buf, hwaddr len)
{
    g_assert(uc);
    return address_space_write(&uc->as, addr, MEMTXATTRS_UNSPECIFIED, buf, len);
}

void unicorn_backend_reset(UnicornBackend *uc)
{
    g_assert(uc);
    cpu_reset(uc->cpu);
    uc->stop_requested = false;
}

void unicorn_backend_set_pc(UnicornBackend *uc, vaddr addr)
{
    g_assert(uc);
    cpu_set_pc(uc->cpu, addr);
}

vaddr unicorn_backend_get_pc(UnicornBackend *uc)
{
    g_assert(uc);
    return cpu_get_pc(uc->cpu);
}

static UnicornRunResult unicorn_backend_translate_run_result(int cpu_exit)
{
    switch (cpu_exit) {
    case EXCP_HLT:
    case EXCP_HALTED:
        return UNICORN_RUN_HALTED;
    case EXCP_DEBUG:
        return UNICORN_RUN_BUDGET_EXHAUSTED;
    default:
        return UNICORN_RUN_EXCEPTION;
    }
}

UnicornRunResult unicorn_backend_run(UnicornBackend *uc,
                                     uint64_t max_instructions,
                                     int *cpu_exit)
{
    int ret = EXCP_DEBUG;
    uint64_t i;

    g_assert(uc);
    uc->stop_requested = false;

    if (max_instructions == 0) {
        cpu_exec_start(uc->cpu);
        ret = cpu_exec(uc->cpu);
        cpu_exec_end(uc->cpu);
        if (cpu_exit) {
            *cpu_exit = ret;
        }
        return unicorn_backend_translate_run_result(ret);
    }

    cpu_single_step(uc->cpu, SSTEP_ENABLE | SSTEP_NOIRQ | SSTEP_NOTIMER);
    for (i = 0; i < max_instructions; i++) {
        cpu_exec_start(uc->cpu);
        ret = cpu_exec(uc->cpu);
        cpu_exec_end(uc->cpu);

        if (uc->stop_requested || ret != EXCP_DEBUG) {
            break;
        }
    }
    cpu_single_step(uc->cpu, 0);

    if (cpu_exit) {
        *cpu_exit = ret;
    }

    if (uc->stop_requested) {
        return UNICORN_RUN_STOP_REQUESTED;
    }
    if (ret == EXCP_DEBUG && i == max_instructions) {
        return UNICORN_RUN_BUDGET_EXHAUSTED;
    }
    return unicorn_backend_translate_run_result(ret);
}

void unicorn_backend_stop(UnicornBackend *uc)
{
    g_assert(uc);
    uc->stop_requested = true;
    cpu_exit(uc->cpu);
}

CPUState *unicorn_backend_cpu(UnicornBackend *uc)
{
    return uc ? uc->cpu : NULL;
}

AddressSpace *unicorn_backend_address_space(UnicornBackend *uc)
{
    return uc ? &uc->as : NULL;
}

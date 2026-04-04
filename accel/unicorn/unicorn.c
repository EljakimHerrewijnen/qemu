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
#include "hw/core/boards.h"
#include "hw/core/sysbus.h"
#include "exec/cpu-common.h"
#include "system/address-spaces.h"
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
    AddressSpace *as_ptr;
    GPtrArray *ram_regions;
    GPtrArray *mmio_regions;
    bool stop_requested;
    bool machine_mode;
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
    Error *local_err = NULL;
    UnicornBackend *uc;
    Object *cpuobj;

    if (!cpu_type) {
        error_setg(errp, "a concrete CPU type is required");
        return NULL;
    }

    if (!unicorn_backend_initialize(&local_err)) {
        error_propagate(errp, local_err);
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
        error_propagate(errp, local_err);
        object_unref(cpuobj);
        unicorn_backend_free(uc);
        return NULL;
    }

    if (!qdev_realize(DEVICE(cpuobj), NULL, &local_err)) {
        error_propagate(errp, local_err);
        object_unref(cpuobj);
        unicorn_backend_free(uc);
        return NULL;
    }

    uc->cpu = CPU(cpuobj);
    uc->as_ptr = &uc->as;
    unicorn_backend_reset(uc);
    return uc;
}

void unicorn_backend_free(UnicornBackend *uc)
{
    if (!uc) {
        return;
    }

    if (!uc->machine_mode) {
        if (uc->cpu) {
            object_unparent(OBJECT(uc->cpu));
            object_unref(OBJECT(uc->cpu));
        }
        address_space_destroy(&uc->as);
    }

    g_ptr_array_free(uc->mmio_regions, true);
    g_ptr_array_free(uc->ram_regions, true);
    g_free(uc);
}

/*
 * Machine backend initialization helper.  Called once to set up the TCG
 * engine in system-emulation mode using a real machine type rather than the
 * fake machine container used by board mode.  Unlike unicorn_backend_initialize
 * this path must be taken before any board-mode backend is created because both
 * paths share global TCG/memory state.
 */
static bool unicorn_machine_backend_init(const char *machine_type,
                                         uint64_t ram_size,
                                         CPUState **out_cpu,
                                         Error **errp)
{
    Error *local_err = NULL;
    ObjectClass *mc_class;
    MachineClass *mc;
    MachineState *machine;
    g_autofree char *full_type = NULL;
    static gsize initialized;
    static const char *const containers[] = {
        "unattached",
        "peripheral",
        "peripheral-anon",
    };

    if (!g_once_init_enter(&initialized)) {
        error_setg(errp, "machine backend already initialized");
        return false;
    }

    module_call_init(MODULE_INIT_QOM);
    qemu_init_cpu_list();
    qemu_init_cpu_loop();
    cpu_exec_init_all();

    full_type = g_strconcat(machine_type, TYPE_MACHINE_SUFFIX, NULL);
    mc_class = object_class_by_name(full_type);
    if (!mc_class) {
        g_once_init_leave(&initialized, 1);
        error_setg(errp, "unknown machine type: '%s'", machine_type);
        return false;
    }
    mc = MACHINE_CLASS(mc_class);

    machine = MACHINE(object_new_with_class(mc_class));
    object_property_add_child(object_get_root(), "machine", OBJECT(machine));

    for (unsigned i = 0; i < ARRAY_SIZE(containers); i++) {
        object_property_add_new_container(OBJECT(machine), containers[i]);
    }
    object_property_add_child(machine_get_container("unattached"),
                              "sysbus", OBJECT(sysbus_get_default()));

    current_machine = machine;
    machine->ram_size = ram_size > 0 ? (ram_addr_t)ram_size
                                     : mc->default_ram_size;
    machine->cpu_type = machine_default_cpu_type(machine);

    tcg_allowed = true;
    page_init();
    tb_htable_init();
    tcg_init(32 * MiB, 0, 1);
    tcg_prologue_init();

    machine_run_board_init(machine, NULL, &local_err);
    g_once_init_leave(&initialized, 1);
    if (local_err) {
        error_propagate(errp, local_err);
        return false;
    }

    if (!first_cpu) {
        error_setg(errp, "machine '%s' did not create any CPUs", machine_type);
        return false;
    }

    *out_cpu = first_cpu;
    return true;
}

UnicornBackend *unicorn_backend_new_machine(const char *machine_type,
                                            uint64_t ram_size,
                                            Error **errp)
{
    Error *local_err = NULL;
    CPUState *cpu = NULL;
    UnicornBackend *uc;

    if (!machine_type) {
        error_setg(errp, "a machine type is required");
        return NULL;
    }

    if (!unicorn_machine_backend_init(machine_type, ram_size, &cpu,
                                      &local_err)) {
        error_propagate(errp, local_err);
        return NULL;
    }

    uc = g_new0(UnicornBackend, 1);
    uc->cpu = cpu;
    uc->as_ptr = &address_space_memory;
    uc->machine_mode = true;
    uc->ram_regions = g_ptr_array_new_with_free_func(g_free);
    uc->mmio_regions = g_ptr_array_new_with_free_func(g_free);
    uc->stop_requested = false;
    return uc;
}

bool unicorn_backend_map_ram(UnicornBackend *uc, const char *name,
                             hwaddr addr, uint64_t size, Error **errp)
{
    UnicornRAMRegion *region;
    MemoryRegion *parent;

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

    parent = uc->machine_mode ? get_system_memory() : &uc->root;
    memory_region_add_subregion(parent, addr, &region->mr);
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
    MemoryRegion *parent;

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
    parent = uc->machine_mode ? get_system_memory() : &uc->root;
    memory_region_add_subregion(parent, addr, &region->mr);
    g_ptr_array_add(uc->mmio_regions, region);
    return true;
}

MemTxResult unicorn_backend_mem_read(UnicornBackend *uc, hwaddr addr,
                                     void *buf, hwaddr len)
{
    g_assert(uc);
    return address_space_read(uc->as_ptr, addr, MEMTXATTRS_UNSPECIFIED,
                              buf, len);
}

MemTxResult unicorn_backend_mem_write(UnicornBackend *uc, hwaddr addr,
                                      const void *buf, hwaddr len)
{
    g_assert(uc);
    return address_space_write(uc->as_ptr, addr, MEMTXATTRS_UNSPECIFIED,
                               buf, len);
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
    if (!uc->cpu->cc->get_pc) {
        return 0;
    }
    return uc->cpu->cc->get_pc(uc->cpu);
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
    return uc ? uc->as_ptr : NULL;
}

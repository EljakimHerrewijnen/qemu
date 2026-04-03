/*
 * Minimal Hedgehog-like embedding backend
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/accel.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/target-info.h"
#include "qemu/units.h"
#include "qom/object.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/qdev.h"
#include "hw/core/cpu.h"
#include "exec/cpu-common.h"
#include "system/cpus.h"
#include "system/hedgehog-exec-hooks.h"
#include "system/memory.h"
#include "system/tcg.h"
#include "system/hedgehog-backend.h"
#include "tcg/startup.h"
#include "accel/tcg/internal-common.h"
#include "hedgehog-mmio-device.h"

typedef struct HedgehogRAMRegion {
    MemoryRegion mr;
} HedgehogRAMRegion;

typedef struct HedgehogMMIOMapping {
    HedgehogMMIODevice *dev;
} HedgehogMMIOMapping;

struct HedgehogBackend {
    CPUState *cpu;
    MemoryRegion root;
    AddressSpace as;
    GPtrArray *ram_regions;
    GPtrArray *mmio_mappings;
    bool stop_requested;
    bool invalid_mem_seen;
    HedgehogInvalidMemInfo invalid_mem_info;
};

static void hedgehog_backend_request_stop_from_hook(void *opaque,
                                                   HedgehogExecStopReason reason,
                                                   const HedgehogInvalidMemInfo *info)
{
    HedgehogBackend *uc = opaque;

    if (reason == HEDGEHOG_EXEC_STOP_INVALID_MEMORY) {
        uc->invalid_mem_seen = true;
        if (info) {
            uc->invalid_mem_info = *info;
        }
    }

    hedgehog_backend_stop(uc);
}

static void hedgehog_backend_create_machine_containers(Object *machine)
{
    static const char *const containers[] = {
        "unattached",
        "peripheral",
        "peripheral-anon",
    };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(containers); i++) {
        if (!object_resolve_path_component(machine, containers[i])) {
            object_property_add_new_container(machine, containers[i]);
        }
    }
}

static void hedgehog_backend_create_minimal_machine(void)
{
    Object *machine;
    ObjectClass *machine_class;

    machine = object_resolve_path_component(object_get_root(), "machine");
    if (machine) {
        if (!current_machine && object_dynamic_cast(machine, TYPE_MACHINE)) {
            current_machine = MACHINE(machine);
        }
        return;
    }

    machine_class = module_object_class_by_name(MACHINE_TYPE_NAME("none"));
    g_assert(machine_class);

    current_machine = MACHINE(object_new_with_class(machine_class));
    object_property_add_child(object_get_root(), "machine", OBJECT(current_machine));
    hedgehog_backend_create_machine_containers(OBJECT(current_machine));
}

static void hedgehog_backend_init_tcg_accel(void)
{
    AccelClass *ac;
    AccelState *accel;
    int ret;

    if (current_machine && current_machine->accelerator) {
        return;
    }

    if (!current_machine) {
        Object *machine = object_resolve_path_component(object_get_root(), "machine");

        if (machine && object_dynamic_cast(machine, TYPE_MACHINE)) {
            current_machine = MACHINE(machine);
        }
    }

    g_assert(current_machine);

    ac = accel_find("tcg");
    g_assert(ac);

    accel = ACCEL(object_new_with_class(OBJECT_CLASS(ac)));
    ret = accel_init_machine(accel, current_machine);
    g_assert(ret == 0);

    accel_init_interfaces(ACCEL_GET_CLASS(current_machine->accelerator));
}

bool hedgehog_backend_initialize(Error **errp)
{
    static gsize initialized;

    if (g_once_init_enter(&initialized)) {
        module_call_init(MODULE_INIT_QOM);

        if (object_resolve_path_component(object_get_root(), "machine") == NULL) {
            hedgehog_backend_create_minimal_machine();
            qemu_init_cpu_list();
            qemu_init_cpu_loop();
            cpu_exec_init_all();
            hedgehog_backend_init_tcg_accel();
        }

        g_once_init_leave(&initialized, 1);
    }

    if (!tcg_enabled()) {
        error_setg(errp, "the hedgehog backend requires initialized TCG support");
        return false;
    }

    return true;
}

HedgehogBackend *hedgehog_backend_new(const char *cpu_type, Error **errp)
{
    Error *local_err = NULL;
    HedgehogBackend *uc;
    Object *cpuobj;
    ObjectClass *cpu_class;

    BQL_LOCK_GUARD();

    if (!cpu_type) {
        error_setg(errp, "a concrete CPU type is required");
        return NULL;
    }

    if (!hedgehog_backend_initialize(&local_err)) {
        error_propagate(errp, local_err);
        return NULL;
    }

    uc = g_new0(HedgehogBackend, 1);
    uc->ram_regions = g_ptr_array_new();
    uc->mmio_mappings = g_ptr_array_new();

    memory_region_init(&uc->root, NULL, "hedgehog-memory", UINT64_MAX);
    address_space_init(&uc->as, &uc->root, "hedgehog-memory");

    cpu_class = module_object_class_by_name(cpu_type);
    if (!cpu_class) {
        cpu_class = cpu_class_by_name(target_cpu_type(), cpu_type);
    }
    if (!cpu_class) {
        error_setg(errp, "unknown cpu type '%s' for target '%s'",
                   cpu_type, target_cpu_type());
        hedgehog_backend_free(uc);
        return NULL;
    }
    if (object_class_is_abstract(cpu_class)) {
        error_setg(errp, "cpu type '%s' is abstract; use a concrete model", cpu_type);
        hedgehog_backend_free(uc);
        return NULL;
    }

    cpuobj = object_new_with_class(cpu_class);
    if (object_property_find(cpuobj, "apic-id")) {
        object_property_set_int(cpuobj, "apic-id", 0, &local_err);
        if (local_err) {
            error_propagate(errp, local_err);
            object_unref(cpuobj);
            hedgehog_backend_free(uc);
            return NULL;
        }
    }

    object_property_set_link(cpuobj, "memory", OBJECT(&uc->root), &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        object_unref(cpuobj);
        hedgehog_backend_free(uc);
        return NULL;
    }

    if (!qdev_realize(DEVICE(cpuobj), NULL, &local_err)) {
        error_propagate(errp, local_err);
        object_unref(cpuobj);
        hedgehog_backend_free(uc);
        return NULL;
    }

    uc->cpu = CPU(cpuobj);
    hedgehog_exec_hook_register_backend(uc->cpu, uc,
                                       hedgehog_backend_request_stop_from_hook,
                                       uc);
    hedgehog_backend_reset(uc);
    return uc;
}

void hedgehog_backend_free(HedgehogBackend *uc)
{
    guint i;

    BQL_LOCK_GUARD();

    if (!uc) {
        return;
    }

    if (uc->cpu) {
        hedgehog_exec_hook_unregister_backend(uc->cpu);
        cpu_exit(uc->cpu);
        if (DEVICE(uc->cpu)->realized) {
            qdev_unrealize(DEVICE(uc->cpu));
        }
        object_unref(OBJECT(uc->cpu));
        uc->cpu = NULL;
    }

    for (i = 0; i < uc->mmio_mappings->len; i++) {
        HedgehogMMIOMapping *mapping = g_ptr_array_index(uc->mmio_mappings, i);

        memory_region_del_subregion(&uc->root,
                                    hedgehog_mmio_device_region(mapping->dev));
        if (DEVICE(mapping->dev)->realized) {
            qdev_unrealize(DEVICE(mapping->dev));
        }
        object_unref(OBJECT(mapping->dev));
        g_free(mapping);
    }

    for (i = 0; i < uc->ram_regions->len; i++) {
        HedgehogRAMRegion *region = g_ptr_array_index(uc->ram_regions, i);

        memory_region_del_subregion(&uc->root, &region->mr);
        object_unparent(OBJECT(&region->mr));
        g_free(region);
    }

    address_space_destroy(&uc->as);
    object_unparent(OBJECT(&uc->root));

    g_ptr_array_free(uc->mmio_mappings, true);
    g_ptr_array_free(uc->ram_regions, true);
    g_free(uc);
}

bool hedgehog_backend_map_ram(HedgehogBackend *uc, const char *name,
                             hwaddr addr, uint64_t size, Error **errp)
{
    HedgehogRAMRegion *region;

    BQL_LOCK_GUARD();

    if (!uc || !size) {
        error_setg(errp, "RAM mapping requires a backend and non-zero size");
        return false;
    }

    region = g_new0(HedgehogRAMRegion, 1);
    if (!memory_region_init_ram_flags_nomigrate(&region->mr, NULL,
                                                name ?: "hedgehog-ram",
                                                size, 0, errp)) {
        g_free(region);
        return false;
    }

    memory_region_add_subregion(&uc->root, addr, &region->mr);
    g_ptr_array_add(uc->ram_regions, region);
    return true;
}

bool hedgehog_backend_map_mmio(HedgehogBackend *uc, const char *name,
                              hwaddr addr, uint64_t size,
                              HedgehogMMIOReadFunc read_fn,
                              HedgehogMMIOWriteFunc write_fn,
                              void *opaque, Error **errp)
{
    HedgehogMMIOMapping *mapping;
    Object *obj;

    BQL_LOCK_GUARD();

    if (!uc || !size) {
        error_setg(errp, "MMIO mapping requires a backend and non-zero size");
        return false;
    }

    obj = object_new(TYPE_HEDGEHOG_MMIO_DEVICE);
    hedgehog_mmio_device_configure(HEDGEHOG_MMIO_DEVICE(obj),
                                  name ?: "hedgehog-mmio",
                                  size, read_fn, write_fn, opaque);

    if (!qdev_realize(DEVICE(obj), NULL, errp)) {
        object_unref(obj);
        return false;
    }

    mapping = g_new0(HedgehogMMIOMapping, 1);
    mapping->dev = HEDGEHOG_MMIO_DEVICE(obj);
    memory_region_add_subregion(&uc->root, addr,
                                hedgehog_mmio_device_region(mapping->dev));
    g_ptr_array_add(uc->mmio_mappings, mapping);
    return true;
}

MemTxResult hedgehog_backend_mem_read(HedgehogBackend *uc, hwaddr addr,
                                     void *buf, hwaddr len)
{
    g_assert(uc);
    return address_space_read(&uc->as, addr, MEMTXATTRS_UNSPECIFIED, buf, len);
}

MemTxResult hedgehog_backend_mem_write(HedgehogBackend *uc, hwaddr addr,
                                      const void *buf, hwaddr len)
{
    g_assert(uc);
    return address_space_write(&uc->as, addr, MEMTXATTRS_UNSPECIFIED, buf, len);
}

int hedgehog_backend_reg_read(HedgehogBackend *uc, int regno,
                             uint8_t *buf, size_t buf_size,
                             Error **errp)
{
    CPUClass *cc;
    g_autoptr(GByteArray) reg = NULL;
    int reg_len;

    if (!uc || !buf) {
        error_setg(errp, "register read requires backend and output buffer");
        return -1;
    }
    if (regno < 0) {
        error_setg(errp, "register read requires non-negative register id");
        return -1;
    }

    cc = uc->cpu->cc;
    if (!cc->gdb_read_register) {
        error_setg(errp, "target does not expose gdb register read callback");
        return -1;
    }

    reg = g_byte_array_new();
    reg_len = cc->gdb_read_register(uc->cpu, reg, regno);
    if (reg_len <= 0 || reg->len < reg_len) {
        error_setg(errp, "failed to read register %d", regno);
        return -1;
    }
    if ((size_t)reg_len > buf_size) {
        error_setg(errp, "buffer too small for register %d (%d bytes needed)",
                   regno, reg_len);
        return -1;
    }

    memcpy(buf, reg->data, reg_len);
    return reg_len;
}

int hedgehog_backend_reg_write(HedgehogBackend *uc, int regno,
                              const uint8_t *buf, size_t buf_size,
                              Error **errp)
{
    CPUClass *cc;
    g_autofree uint8_t *tmp = NULL;
    int reg_len;

    if (!uc || !buf) {
        error_setg(errp, "register write requires backend and input buffer");
        return -1;
    }
    if (regno < 0) {
        error_setg(errp, "register write requires non-negative register id");
        return -1;
    }

    cc = uc->cpu->cc;
    if (!cc->gdb_write_register) {
        error_setg(errp, "target does not expose gdb register write callback");
        return -1;
    }

    tmp = g_memdup2(buf, buf_size);
    reg_len = cc->gdb_write_register(uc->cpu, tmp, regno);
    if (reg_len <= 0 || (size_t)reg_len > buf_size) {
        error_setg(errp, "failed to write register %d", regno);
        return -1;
    }

    return reg_len;
}

void hedgehog_backend_set_tb_hook(HedgehogBackend *uc,
                                 HedgehogExecHookFunc hook_fn,
                                 void *opaque)
{
    if (!uc) {
        return;
    }

    hedgehog_exec_hook_set_tb(uc, hook_fn, opaque);
}

void hedgehog_backend_set_insn_hook(HedgehogBackend *uc,
                                   HedgehogExecHookFunc hook_fn,
                                   void *opaque)
{
    if (!uc) {
        return;
    }

    hedgehog_exec_hook_set_insn(uc, hook_fn, opaque);
}

void hedgehog_backend_set_invalid_mem_hook(HedgehogBackend *uc,
                                          HedgehogInvalidMemHookFunc hook_fn,
                                          void *opaque)
{
    if (!uc) {
        return;
    }

    hedgehog_exec_hook_set_invalid(uc, hook_fn, opaque);
}

void hedgehog_backend_reset(HedgehogBackend *uc)
{
    g_assert(uc);
    cpu_reset(uc->cpu);
    uc->stop_requested = false;
    uc->invalid_mem_seen = false;
}

void hedgehog_backend_set_pc(HedgehogBackend *uc, vaddr addr)
{
    g_assert(uc);
    cpu_set_pc(uc->cpu, addr);
}

vaddr hedgehog_backend_get_pc(HedgehogBackend *uc)
{
    g_assert(uc);
    return uc->cpu->cc->get_pc(uc->cpu);
}

static HedgehogRunResult hedgehog_backend_translate_run_result(int cpu_exit)
{
    switch (cpu_exit) {
    case EXCP_HLT:
    case EXCP_HALTED:
        return HEDGEHOG_RUN_HALTED;
    case EXCP_DEBUG:
        return HEDGEHOG_RUN_BUDGET_EXHAUSTED;
    default:
        return HEDGEHOG_RUN_EXCEPTION;
    }
}

HedgehogRunResult hedgehog_backend_run(HedgehogBackend *uc,
                                     uint64_t max_instructions,
                                     int *cpu_exit)
{
    int ret = EXCP_DEBUG;
    uint64_t i;

    g_assert(uc);
    uc->stop_requested = false;
    uc->invalid_mem_seen = false;

    if (max_instructions == 0) {
        cpu_exec_start(uc->cpu);
        ret = cpu_exec(uc->cpu);
        cpu_exec_end(uc->cpu);
        if (cpu_exit) {
            *cpu_exit = ret;
        }
        return hedgehog_backend_translate_run_result(ret);
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

    if (uc->invalid_mem_seen) {
        return HEDGEHOG_RUN_INVALID_MEMORY;
    }
    if (uc->stop_requested) {
        return HEDGEHOG_RUN_STOP_REQUESTED;
    }
    if (ret == EXCP_DEBUG && i == max_instructions) {
        return HEDGEHOG_RUN_BUDGET_EXHAUSTED;
    }
    return hedgehog_backend_translate_run_result(ret);
}

void hedgehog_backend_stop(HedgehogBackend *uc)
{
    g_assert(uc);
    uc->stop_requested = true;
    cpu_exit(uc->cpu);
}

CPUState *hedgehog_backend_cpu(HedgehogBackend *uc)
{
    return uc ? uc->cpu : NULL;
}

AddressSpace *hedgehog_backend_address_space(HedgehogBackend *uc)
{
    return uc ? &uc->as : NULL;
}

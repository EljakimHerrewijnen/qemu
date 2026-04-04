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
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/tcg.h"
#include "system/hedgehog-backend.h"
#include "tcg/startup.h"
#include "accel/tcg/internal-common.h"
#include "hedgehog-mmio-device.h"

typedef struct HedgehogInitState {
    bool initialized;
    bool board_initialized;
    bool board_backend_active;
    char *machine_type;
} HedgehogInitState;

static GMutex hedgehog_init_lock;
static HedgehogInitState hedgehog_init_state;
static gsize hedgehog_qom_initialized;

typedef struct HedgehogRAMRegion {
    MemoryRegion mr;
} HedgehogRAMRegion;

typedef struct HedgehogMMIOMapping {
    HedgehogMMIODevice *dev;
} HedgehogMMIOMapping;

struct HedgehogBackend {
    CPUState *cpu;
    AddressSpace *active_as;
    bool board_backed;
    bool owns_cpu;
    bool owns_address_space;
    bool owns_memory_root;
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

    if (!object_resolve_path_component(machine_get_container("unattached"),
                                       "sysbus")) {
        object_property_add_child(machine_get_container("unattached"),
                                  "sysbus", OBJECT(sysbus_get_default()));
    }
}

static char *hedgehog_backend_canonicalize_machine_type(const char *machine_type)
{
    const char *type = machine_type && machine_type[0] ? machine_type : "none";
    size_t suffix_len = strlen(TYPE_MACHINE_SUFFIX);
    size_t type_len = strlen(type);

    if (type_len > suffix_len &&
        g_str_has_suffix(type, TYPE_MACHINE_SUFFIX)) {
        return g_strndup(type, type_len - suffix_len);
    }

    return g_strdup(type);
}

static char *hedgehog_backend_machine_type_from_obj(Object *obj)
{
    const char *qom_type = object_get_typename(obj);

    return hedgehog_backend_canonicalize_machine_type(qom_type);
}

static bool hedgehog_backend_create_machine(const char *machine_type,
                                            Error **errp)
{
    g_autofree char *requested = NULL;
    g_autofree char *qom_type = NULL;
    g_autofree char *existing = NULL;
    Object *machine;
    ObjectClass *machine_class;

    requested = hedgehog_backend_canonicalize_machine_type(machine_type);
    machine = object_resolve_path_component(object_get_root(), "machine");

    if (machine) {
        if (!object_dynamic_cast(machine, TYPE_MACHINE)) {
            error_setg(errp, "existing /machine object is not a MachineState");
            return false;
        }

        if (!current_machine) {
            current_machine = MACHINE(machine);
        }

        hedgehog_backend_create_machine_containers(machine);

        existing = hedgehog_backend_machine_type_from_obj(machine);
        if (g_strcmp0(existing, requested) != 0) {
            error_setg(errp,
                       "requested machine '%s' but existing machine is '%s'",
                       requested, existing);
            return false;
        }
        return true;
    }

    qom_type = g_strconcat(requested, TYPE_MACHINE_SUFFIX, NULL);
    machine_class = module_object_class_by_name(qom_type);
    if (!machine_class) {
        error_setg(errp, "unknown machine type '%s'", requested);
        return false;
    }

    object_set_machine_compat_props(MACHINE_CLASS(machine_class)->compat_props);

    current_machine = MACHINE(object_new_with_class(machine_class));
    object_property_add_child(object_get_root(), "machine", OBJECT(current_machine));
    hedgehog_backend_create_machine_containers(OBJECT(current_machine));

    return true;
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

static void hedgehog_backend_advance_machine_phase(MachineInitPhase phase)
{
    if (!phase_check(phase)) {
        phase_advance(phase);
    }
}

static void hedgehog_backend_advance_machine_phases(void)
{
    hedgehog_backend_advance_machine_phase(PHASE_MACHINE_CREATED);
    hedgehog_backend_advance_machine_phase(PHASE_ACCEL_CREATED);
    hedgehog_backend_advance_machine_phase(PHASE_LATE_BACKENDS_CREATED);
}

static bool hedgehog_backend_is_board_machine(const char *machine_type)
{
    g_autofree char *canonical = hedgehog_backend_canonicalize_machine_type(machine_type);

    return strcmp(canonical, "none") != 0;
}

static CPUState *hedgehog_backend_first_realized_cpu(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        if (DEVICE(cpu)->realized) {
            return cpu;
        }
    }

    return NULL;
}

static ObjectClass *hedgehog_backend_lookup_cpu_class(const char *cpu_type,
                                                      Error **errp)
{
    ObjectClass *cpu_class;

    cpu_class = module_object_class_by_name(cpu_type);
    if (!cpu_class) {
        cpu_class = cpu_class_by_name(target_cpu_type(), cpu_type);
    }
    if (!cpu_class) {
        error_setg(errp, "unknown cpu type '%s' for target '%s'",
                   cpu_type, target_cpu_type());
        return NULL;
    }
    if (object_class_is_abstract(cpu_class)) {
        error_setg(errp, "cpu type '%s' is abstract; use a concrete model",
                   cpu_type);
        return NULL;
    }

    return cpu_class;
}

static bool hedgehog_backend_realize_board_machine(const char *cpu_type,
                                                   Error **errp)
{
    const char *default_cpu_type;
    CPUState *cpu;
    ObjectClass *cpu_class;
    Error *local_err = NULL;

    if (!current_machine) {
        error_setg(errp, "failed to realize board machine: no current machine");
        return false;
    }

    if (!current_machine->cpu_type) {
        default_cpu_type = machine_default_cpu_type(current_machine);
        if (default_cpu_type) {
            current_machine->cpu_type = default_cpu_type;
        } else if (cpu_type && cpu_type[0]) {
            cpu_class = hedgehog_backend_lookup_cpu_class(cpu_type, errp);
            if (!cpu_class) {
                return false;
            }
            current_machine->cpu_type = object_class_get_name(cpu_class);
        } else {
            error_setg(errp,
                       "machine '%s' does not provide a default CPU type",
                       object_get_typename(OBJECT(current_machine)));
            return false;
        }
    }

    machine_run_board_init(current_machine, NULL, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return false;
    }

    cpu = hedgehog_backend_first_realized_cpu();
    if (!cpu) {
        error_setg(errp, "machine '%s' realized without a CPU",
                   object_get_typename(OBJECT(current_machine)));
        return false;
    }

    return true;
}

bool hedgehog_backend_initialize(Error **errp)
{
    return hedgehog_backend_initialize_for_machine(NULL, errp);
}

bool hedgehog_backend_initialize_for_machine(const char *machine_type,
                                             Error **errp)
{
    g_autofree char *requested = hedgehog_backend_canonicalize_machine_type(machine_type);

    g_mutex_lock(&hedgehog_init_lock);

    if (hedgehog_init_state.initialized) {
        if (g_strcmp0(requested, hedgehog_init_state.machine_type) != 0) {
            error_setg(errp,
                       "hedgehog backend is already initialized for machine '%s' "
                       "and cannot switch to '%s'",
                       hedgehog_init_state.machine_type, requested);
            g_mutex_unlock(&hedgehog_init_lock);
            return false;
        }
        g_mutex_unlock(&hedgehog_init_lock);
        return true;
    }

    if (g_once_init_enter(&hedgehog_qom_initialized)) {
        module_call_init(MODULE_INIT_QOM);
        g_once_init_leave(&hedgehog_qom_initialized, 1);
    }

    if (!hedgehog_backend_create_machine(requested, errp)) {
        g_mutex_unlock(&hedgehog_init_lock);
        return false;
    }

    qemu_init_cpu_list();
    qemu_init_cpu_loop();
    cpu_exec_init_all();
    hedgehog_backend_init_tcg_accel();
    hedgehog_backend_advance_machine_phases();

    if (!tcg_enabled()) {
        error_setg(errp, "the hedgehog backend requires initialized TCG support");
        g_mutex_unlock(&hedgehog_init_lock);
        return false;
    }

    hedgehog_init_state.initialized = true;
    hedgehog_init_state.machine_type = g_steal_pointer(&requested);
    g_mutex_unlock(&hedgehog_init_lock);

    return true;
}

HedgehogBackend *hedgehog_backend_new(const char *cpu_type, Error **errp)
{
    return hedgehog_backend_new_with_machine(cpu_type, NULL, errp);
}

HedgehogBackend *hedgehog_backend_new_with_machine(const char *cpu_type,
                                                   const char *machine_type,
                                                   Error **errp)
{
    Error *local_err = NULL;
    g_autofree char *requested_machine =
        hedgehog_backend_canonicalize_machine_type(machine_type);
    bool board_backed = hedgehog_backend_is_board_machine(requested_machine);
    bool board_initialized = false;
    CPUState *cpu = NULL;
    HedgehogBackend *uc;
    Object *cpuobj;
    ObjectClass *cpu_class;

    BQL_LOCK_GUARD();

    if (!cpu_type && !board_backed) {
        error_setg(errp, "a concrete CPU type is required");
        return NULL;
    }

    if (!hedgehog_backend_initialize_for_machine(machine_type, &local_err)) {
        error_propagate(errp, local_err);
        return NULL;
    }

    if (board_backed) {
        g_mutex_lock(&hedgehog_init_lock);
        if (hedgehog_init_state.board_backend_active) {
            error_setg(errp,
                       "only one board-backed hedgehog backend can be active at a time");
            g_mutex_unlock(&hedgehog_init_lock);
            return NULL;
        }
        board_initialized = hedgehog_init_state.board_initialized;
        g_mutex_unlock(&hedgehog_init_lock);

        if (!board_initialized) {
            if (!hedgehog_backend_realize_board_machine(cpu_type, &local_err)) {
                error_propagate(errp, local_err);
                return NULL;
            }

            g_mutex_lock(&hedgehog_init_lock);
            hedgehog_init_state.board_initialized = true;
            g_mutex_unlock(&hedgehog_init_lock);
        }

        cpu = hedgehog_backend_first_realized_cpu();
        if (!cpu) {
            error_setg(errp, "failed to find a realized board CPU");
            return NULL;
        }
    }

    uc = g_new0(HedgehogBackend, 1);
    uc->board_backed = board_backed;
    uc->ram_regions = g_ptr_array_new();
    uc->mmio_mappings = g_ptr_array_new();

    if (board_backed) {
        uc->cpu = cpu;
        uc->active_as = &address_space_memory;
    } else {
        memory_region_init(&uc->root, NULL, "hedgehog-memory", UINT64_MAX);
        uc->owns_memory_root = true;

        address_space_init(&uc->as, &uc->root, "hedgehog-memory");
        uc->owns_address_space = true;
        uc->active_as = &uc->as;

        cpu_class = hedgehog_backend_lookup_cpu_class(cpu_type, errp);
        if (!cpu_class) {
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
        uc->owns_cpu = true;
    }

    hedgehog_exec_hook_register_backend(uc->cpu, uc,
                                       hedgehog_backend_request_stop_from_hook,
                                       uc);

    if (board_backed) {
        g_mutex_lock(&hedgehog_init_lock);
        hedgehog_init_state.board_backend_active = true;
        g_mutex_unlock(&hedgehog_init_lock);
    }

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
        if (uc->owns_cpu && DEVICE(uc->cpu)->realized) {
            qdev_unrealize(DEVICE(uc->cpu));
        }
        if (uc->owns_cpu) {
            object_unref(OBJECT(uc->cpu));
        }
        uc->cpu = NULL;
    }

    for (i = 0; i < uc->mmio_mappings->len; i++) {
        HedgehogMMIOMapping *mapping = g_ptr_array_index(uc->mmio_mappings, i);

        if (uc->owns_memory_root) {
            memory_region_del_subregion(&uc->root,
                                        hedgehog_mmio_device_region(mapping->dev));
        }
        if (DEVICE(mapping->dev)->realized) {
            qdev_unrealize(DEVICE(mapping->dev));
        }
        object_unref(OBJECT(mapping->dev));
        g_free(mapping);
    }

    for (i = 0; i < uc->ram_regions->len; i++) {
        HedgehogRAMRegion *region = g_ptr_array_index(uc->ram_regions, i);

        if (uc->owns_memory_root) {
            memory_region_del_subregion(&uc->root, &region->mr);
        }
        object_unparent(OBJECT(&region->mr));
        g_free(region);
    }

    if (uc->owns_address_space) {
        address_space_destroy(&uc->as);
    }
    if (uc->owns_memory_root) {
        object_unparent(OBJECT(&uc->root));
    }

    g_ptr_array_free(uc->mmio_mappings, true);
    g_ptr_array_free(uc->ram_regions, true);

    if (uc->board_backed) {
        g_mutex_lock(&hedgehog_init_lock);
        hedgehog_init_state.board_backend_active = false;
        g_mutex_unlock(&hedgehog_init_lock);
    }

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

    if (uc->board_backed) {
        error_setg(errp,
                   "RAM mapping is not supported for board-backed hedgehog backends");
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

    if (uc->board_backed) {
        error_setg(errp,
                   "MMIO mapping is not supported for board-backed hedgehog backends");
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
    g_assert(uc->active_as);
    return address_space_read(uc->active_as, addr, MEMTXATTRS_UNSPECIFIED,
                              buf, len);
}

MemTxResult hedgehog_backend_mem_write(HedgehogBackend *uc, hwaddr addr,
                                      const void *buf, hwaddr len)
{
    g_assert(uc);
    g_assert(uc->active_as);
    return address_space_write(uc->active_as, addr, MEMTXATTRS_UNSPECIFIED,
                               buf, len);
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
    return uc ? uc->active_as : NULL;
}


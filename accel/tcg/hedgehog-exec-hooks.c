/*
 * Hedgehog backend execution hook registry
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/core/cpu.h"
#include "system/hedgehog-exec-hooks.h"

typedef struct HedgehogExecHookEntry {
    CPUState *cpu;
    HedgehogBackend *uc;
    HedgehogExecStopFunc stop_fn;
    void *stop_opaque;
    HedgehogExecHookFunc tb_hook;
    void *tb_opaque;
    HedgehogExecHookFunc insn_hook;
    void *insn_opaque;
    HedgehogInvalidInsnHookFunc invalid_insn_hook;
    void *invalid_insn_opaque;
    HedgehogInvalidInsnInfoFunc invalid_insn_info;
    HedgehogInvalidMemHookFunc invalid_hook;
    void *invalid_opaque;
    bool invalid_mem_diagnostic_capture;
    bool invalid_mem_diagnostic_valid;
    HedgehogInvalidMemInfo invalid_mem_diagnostic;
    bool direct_run_active;
} HedgehogExecHookEntry;

static GHashTable *hedgehog_cpu_hooks;
static GHashTable *hedgehog_backend_hooks;
static GMutex hedgehog_hooks_lock;
static gsize hedgehog_hooks_initialized;

static void hedgehog_exec_hooks_init_once(void)
{
    hedgehog_cpu_hooks = g_hash_table_new(g_direct_hash, g_direct_equal);
    hedgehog_backend_hooks = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_mutex_init(&hedgehog_hooks_lock);
}

static void hedgehog_exec_hooks_ensure_init(void)
{
    if (g_once_init_enter(&hedgehog_hooks_initialized)) {
        hedgehog_exec_hooks_init_once();
        g_once_init_leave(&hedgehog_hooks_initialized, 1);
    }
}

void hedgehog_exec_hook_register_backend(CPUState *cpu, HedgehogBackend *uc,
                                        HedgehogExecStopFunc stop_fn,
                                        void *stop_opaque,
                                        HedgehogInvalidInsnInfoFunc info_fn)
{
    HedgehogExecHookEntry *entry;

    if (!cpu || !uc) {
        return;
    }

    hedgehog_exec_hooks_ensure_init();
    g_mutex_lock(&hedgehog_hooks_lock);

    entry = g_hash_table_lookup(hedgehog_backend_hooks, uc);
    if (!entry) {
        entry = g_new0(HedgehogExecHookEntry, 1);
        entry->uc = uc;
    } else if (entry->cpu) {
        g_hash_table_remove(hedgehog_cpu_hooks, entry->cpu);
    }

    entry->cpu = cpu;
    entry->stop_fn = stop_fn;
    entry->stop_opaque = stop_opaque;
    entry->invalid_insn_info = info_fn;

    g_hash_table_insert(hedgehog_cpu_hooks, cpu, entry);
    g_hash_table_insert(hedgehog_backend_hooks, uc, entry);

    g_mutex_unlock(&hedgehog_hooks_lock);
}

void hedgehog_exec_hook_unregister_backend(CPUState *cpu)
{
    HedgehogExecHookEntry *entry;

    if (!cpu) {
        return;
    }

    hedgehog_exec_hooks_ensure_init();
    g_mutex_lock(&hedgehog_hooks_lock);

    entry = g_hash_table_lookup(hedgehog_cpu_hooks, cpu);
    if (entry) {
        g_hash_table_remove(hedgehog_cpu_hooks, cpu);
        g_hash_table_remove(hedgehog_backend_hooks, entry->uc);
        g_free(entry);
    }

    g_mutex_unlock(&hedgehog_hooks_lock);
}

void hedgehog_exec_hook_set_direct_run(CPUState *cpu, bool active)
{
    HedgehogExecHookEntry *entry;

    if (!cpu) {
        return;
    }

    hedgehog_exec_hooks_ensure_init();
    g_mutex_lock(&hedgehog_hooks_lock);
    entry = g_hash_table_lookup(hedgehog_cpu_hooks, cpu);
    if (entry) {
        entry->direct_run_active = active;
    }
    g_mutex_unlock(&hedgehog_hooks_lock);
}

bool hedgehog_exec_hook_direct_run_active(CPUState *cpu)
{
    HedgehogExecHookEntry *entry;
    bool active = false;

    if (!cpu) {
        return false;
    }

    hedgehog_exec_hooks_ensure_init();
    g_mutex_lock(&hedgehog_hooks_lock);
    entry = g_hash_table_lookup(hedgehog_cpu_hooks, cpu);
    if (entry) {
        active = entry->direct_run_active;
    }
    g_mutex_unlock(&hedgehog_hooks_lock);
    return active;
}

void hedgehog_exec_hook_set_tb(HedgehogBackend *uc,
                              HedgehogExecHookFunc hook_fn,
                              void *opaque)
{
    HedgehogExecHookEntry *entry;

    if (!uc) {
        return;
    }

    hedgehog_exec_hooks_ensure_init();
    g_mutex_lock(&hedgehog_hooks_lock);

    entry = g_hash_table_lookup(hedgehog_backend_hooks, uc);
    if (entry) {
        entry->tb_hook = hook_fn;
        entry->tb_opaque = opaque;
    }

    g_mutex_unlock(&hedgehog_hooks_lock);
}

void hedgehog_exec_hook_set_insn(HedgehogBackend *uc,
                                HedgehogExecHookFunc hook_fn,
                                void *opaque)
{
    HedgehogExecHookEntry *entry;

    if (!uc) {
        return;
    }

    hedgehog_exec_hooks_ensure_init();
    g_mutex_lock(&hedgehog_hooks_lock);

    entry = g_hash_table_lookup(hedgehog_backend_hooks, uc);
    if (entry) {
        entry->insn_hook = hook_fn;
        entry->insn_opaque = opaque;
    }

    g_mutex_unlock(&hedgehog_hooks_lock);
}

void hedgehog_exec_hook_set_invalid_insn(
    HedgehogBackend *uc,
    HedgehogInvalidInsnHookFunc hook_fn,
    void *opaque)
{
    HedgehogExecHookEntry *entry;

    if (!uc) {
        return;
    }

    hedgehog_exec_hooks_ensure_init();
    g_mutex_lock(&hedgehog_hooks_lock);

    entry = g_hash_table_lookup(hedgehog_backend_hooks, uc);
    if (entry) {
        entry->invalid_insn_hook = hook_fn;
        entry->invalid_insn_opaque = opaque;
    }

    g_mutex_unlock(&hedgehog_hooks_lock);
}

void hedgehog_exec_hook_set_invalid(HedgehogBackend *uc,
                                   HedgehogInvalidMemHookFunc hook_fn,
                                   void *opaque)
{
    HedgehogExecHookEntry *entry;

    if (!uc) {
        return;
    }

    hedgehog_exec_hooks_ensure_init();
    g_mutex_lock(&hedgehog_hooks_lock);

    entry = g_hash_table_lookup(hedgehog_backend_hooks, uc);
    if (entry) {
        entry->invalid_hook = hook_fn;
        entry->invalid_opaque = opaque;
    }

    g_mutex_unlock(&hedgehog_hooks_lock);
}

void hedgehog_exec_hook_set_invalid_mem_diagnostic_capture(
    HedgehogBackend *uc, bool enabled)
{
    HedgehogExecHookEntry *entry;

    if (!uc) {
        return;
    }

    hedgehog_exec_hooks_ensure_init();
    g_mutex_lock(&hedgehog_hooks_lock);
    entry = g_hash_table_lookup(hedgehog_backend_hooks, uc);
    if (entry) {
        entry->invalid_mem_diagnostic_capture = enabled;
        entry->invalid_mem_diagnostic_valid = false;
        memset(&entry->invalid_mem_diagnostic, 0,
               sizeof(entry->invalid_mem_diagnostic));
    }
    g_mutex_unlock(&hedgehog_hooks_lock);
}

bool hedgehog_exec_hook_invalid_mem_diagnostic_capture_enabled(CPUState *cpu)
{
    HedgehogExecHookEntry *entry;
    bool enabled = false;

    if (!cpu) {
        return false;
    }

    hedgehog_exec_hooks_ensure_init();
    g_mutex_lock(&hedgehog_hooks_lock);
    entry = g_hash_table_lookup(hedgehog_cpu_hooks, cpu);
    if (entry) {
        enabled = entry->invalid_mem_diagnostic_capture;
    }
    g_mutex_unlock(&hedgehog_hooks_lock);
    return enabled;
}

void hedgehog_exec_hook_clear_invalid_mem_diagnostic(HedgehogBackend *uc)
{
    HedgehogExecHookEntry *entry;

    if (!uc) {
        return;
    }

    hedgehog_exec_hooks_ensure_init();
    g_mutex_lock(&hedgehog_hooks_lock);
    entry = g_hash_table_lookup(hedgehog_backend_hooks, uc);
    if (entry) {
        entry->invalid_mem_diagnostic_valid = false;
        memset(&entry->invalid_mem_diagnostic, 0,
               sizeof(entry->invalid_mem_diagnostic));
    }
    g_mutex_unlock(&hedgehog_hooks_lock);
}

bool hedgehog_exec_hook_get_last_invalid_mem_diagnostic(
    HedgehogBackend *uc, HedgehogInvalidMemInfo *info)
{
    HedgehogExecHookEntry *entry;
    bool available = false;

    if (!uc || !info) {
        return false;
    }

    hedgehog_exec_hooks_ensure_init();
    g_mutex_lock(&hedgehog_hooks_lock);
    entry = g_hash_table_lookup(hedgehog_backend_hooks, uc);
    if (entry && entry->invalid_mem_diagnostic_valid) {
        *info = entry->invalid_mem_diagnostic;
        available = true;
    }
    g_mutex_unlock(&hedgehog_hooks_lock);
    return available;
}

static HedgehogBackend *hedgehog_exec_hook_backend_for_cpu(CPUState *cpu)
{
    HedgehogExecHookEntry *entry;
    HedgehogBackend *uc = NULL;

    if (!cpu) {
        return NULL;
    }
    hedgehog_exec_hooks_ensure_init();
    g_mutex_lock(&hedgehog_hooks_lock);
    entry = g_hash_table_lookup(hedgehog_cpu_hooks, cpu);
    if (entry && entry->direct_run_active) {
        uc = entry->uc;
    }
    g_mutex_unlock(&hedgehog_hooks_lock);
    return uc;
}

void hedgehog_exec_hook_system_call_request(
    CPUState *cpu, HedgehogSystemCallKind kind, vaddr pc,
    unsigned source_el, unsigned immediate, const uint64_t x[8])
{
    HedgehogBackend *uc = hedgehog_exec_hook_backend_for_cpu(cpu);

    if (uc) {
        hedgehog_backend_report_system_call_request(uc, kind, pc, source_el,
                                                    immediate, x);
    }
}

void hedgehog_exec_hook_system_call_route(
    CPUState *cpu, HedgehogSystemCallKind kind, vaddr return_pc,
    unsigned target_el)
{
    HedgehogBackend *uc = hedgehog_exec_hook_backend_for_cpu(cpu);

    if (uc) {
        hedgehog_backend_report_system_call_route(uc, kind, return_pc,
                                                  target_el);
    }
}

void hedgehog_exec_hook_system_call_complete(
    CPUState *cpu, vaddr return_pc, unsigned exception_el,
    unsigned return_el, const uint64_t x[8])
{
    HedgehogBackend *uc = hedgehog_exec_hook_backend_for_cpu(cpu);

    if (uc) {
        hedgehog_backend_report_system_call_complete(uc, return_pc,
                                                     exception_el, return_el,
                                                     x);
    }
}

void hedgehog_exec_hook_cpu_wait(
    CPUState *cpu, HedgehogCPUWaitKind kind, vaddr pc,
    unsigned current_el)
{
    HedgehogBackend *uc = hedgehog_exec_hook_backend_for_cpu(cpu);

    if (uc) {
        hedgehog_backend_report_cpu_wait(uc, kind, pc, current_el);
    }
}

static bool hedgehog_exec_hook_dispatch(CPUState *cpu, vaddr pc, bool insn_hook)
{
    HedgehogExecHookEntry *entry;
    HedgehogExecHookFunc hook_fn = NULL;
    HedgehogBackend *uc = NULL;
    HedgehogExecStopFunc stop_fn = NULL;
    void *hook_opaque = NULL;
    void *stop_opaque = NULL;

    hedgehog_exec_hooks_ensure_init();
    g_mutex_lock(&hedgehog_hooks_lock);

    entry = g_hash_table_lookup(hedgehog_cpu_hooks, cpu);
    if (entry) {
        uc = entry->uc;
        stop_fn = entry->stop_fn;
        stop_opaque = entry->stop_opaque;
        if (insn_hook) {
            hook_fn = entry->insn_hook;
            hook_opaque = entry->insn_opaque;
        } else {
            hook_fn = entry->tb_hook;
            hook_opaque = entry->tb_opaque;
        }
    }

    g_mutex_unlock(&hedgehog_hooks_lock);

    if (!hook_fn || !uc) {
        return false;
    }

    if (!hook_fn(uc, pc, hook_opaque)) {
        return false;
    }

    if (stop_fn) {
        stop_fn(stop_opaque, HEDGEHOG_EXEC_STOP_REQUESTED, NULL);
    } else {
        cpu_exit(cpu);
    }
    return true;
}

bool hedgehog_exec_hook_invalid(CPUState *cpu,
                                const HedgehogInvalidMemInfo *info)
{
    HedgehogExecHookEntry *entry;
    HedgehogInvalidMemHookFunc hook_fn = NULL;
    HedgehogBackend *uc = NULL;
    HedgehogExecStopFunc stop_fn = NULL;
    void *hook_opaque = NULL;
    void *stop_opaque = NULL;

    if (!info) {
        return false;
    }

    hedgehog_exec_hooks_ensure_init();
    g_mutex_lock(&hedgehog_hooks_lock);

    entry = g_hash_table_lookup(hedgehog_cpu_hooks, cpu);
    if (entry) {
        uc = entry->uc;
        hook_fn = entry->invalid_hook;
        hook_opaque = entry->invalid_opaque;
        stop_fn = entry->stop_fn;
        stop_opaque = entry->stop_opaque;
        if (entry->invalid_mem_diagnostic_capture) {
            entry->invalid_mem_diagnostic = *info;
            entry->invalid_mem_diagnostic_valid = true;
        }
    }

    g_mutex_unlock(&hedgehog_hooks_lock);

    if (!hook_fn || !uc) {
        return false;
    }

    if (!hook_fn(uc, info->addr, info->size, info->access_type,
                 info->response, hook_opaque)) {
        return false;
    }

    if (stop_fn) {
        stop_fn(stop_opaque, HEDGEHOG_EXEC_STOP_INVALID_MEMORY, info);
    } else {
        cpu_exit(cpu);
    }
    return true;
}

HedgehogInvalidInsnDisposition hedgehog_exec_hook_invalid_insn(
    CPUState *cpu,
    vaddr *next_pc)
{
    HedgehogExecHookEntry *entry;
    HedgehogInvalidInsnHookFunc hook_fn = NULL;
    HedgehogInvalidInsnInfoFunc info_fn = NULL;
    HedgehogExecStopFunc stop_fn = NULL;
    HedgehogBackend *uc = NULL;
    void *hook_opaque = NULL;
    void *stop_opaque = NULL;
    HedgehogInvalidInsnInfo info;
    HedgehogInvalidInsnDisposition disposition;

    hedgehog_exec_hooks_ensure_init();
    g_mutex_lock(&hedgehog_hooks_lock);

    entry = g_hash_table_lookup(hedgehog_cpu_hooks, cpu);
    if (entry) {
        uc = entry->uc;
        hook_fn = entry->invalid_insn_hook;
        hook_opaque = entry->invalid_insn_opaque;
        info_fn = entry->invalid_insn_info;
        stop_fn = entry->stop_fn;
        stop_opaque = entry->stop_opaque;
    }

    g_mutex_unlock(&hedgehog_hooks_lock);

    if (!uc || !hook_fn || !info_fn ||
        !info_fn(cpu, cpu->exception_index, &info)) {
        return HEDGEHOG_INVALID_INSN_PASS;
    }

    *next_pc = info.pc + info.size;
    disposition = hook_fn(uc, &info, next_pc, hook_opaque);
    if (disposition == HEDGEHOG_INVALID_INSN_PASS ||
        disposition == HEDGEHOG_INVALID_INSN_CONTINUE) {
        return disposition;
    }

    if (stop_fn) {
        stop_fn(stop_opaque, HEDGEHOG_EXEC_STOP_REQUESTED, NULL);
    } else {
        cpu_exit(cpu);
    }
    return HEDGEHOG_INVALID_INSN_STOP;
}

bool hedgehog_exec_hook_tb_enter(CPUState *cpu, vaddr pc)
{
    return hedgehog_exec_hook_dispatch(cpu, pc, false);
}

bool hedgehog_exec_hook_insn(CPUState *cpu, vaddr pc)
{
    return hedgehog_exec_hook_dispatch(cpu, pc, true);
}

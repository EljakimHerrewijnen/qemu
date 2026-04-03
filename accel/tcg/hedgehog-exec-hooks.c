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
    HedgehogInvalidMemHookFunc invalid_hook;
    void *invalid_opaque;
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
                                        void *stop_opaque)
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

static HedgehogMemAccessType hedgehog_access_type_from_mmu(MMUAccessType type)
{
    switch (type) {
    case MMU_DATA_STORE:
        return HEDGEHOG_MEM_ACCESS_WRITE;
    case MMU_INST_FETCH:
        return HEDGEHOG_MEM_ACCESS_FETCH;
    case MMU_DATA_LOAD:
    default:
        return HEDGEHOG_MEM_ACCESS_READ;
    }
}

bool hedgehog_exec_hook_invalid(CPUState *cpu, vaddr addr,
                               unsigned size,
                               MMUAccessType access_type,
                               MemTxResult response)
{
    HedgehogExecHookEntry *entry;
    HedgehogInvalidMemHookFunc hook_fn = NULL;
    HedgehogBackend *uc = NULL;
    HedgehogExecStopFunc stop_fn = NULL;
    void *hook_opaque = NULL;
    void *stop_opaque = NULL;
    HedgehogInvalidMemInfo info;

    hedgehog_exec_hooks_ensure_init();
    g_mutex_lock(&hedgehog_hooks_lock);

    entry = g_hash_table_lookup(hedgehog_cpu_hooks, cpu);
    if (entry) {
        uc = entry->uc;
        hook_fn = entry->invalid_hook;
        hook_opaque = entry->invalid_opaque;
        stop_fn = entry->stop_fn;
        stop_opaque = entry->stop_opaque;
    }

    g_mutex_unlock(&hedgehog_hooks_lock);

    if (!hook_fn || !uc) {
        return false;
    }

    info.addr = addr;
    info.size = size;
    info.access_type = hedgehog_access_type_from_mmu(access_type);
    info.response = response;

    if (!hook_fn(uc, addr, size, info.access_type, response, hook_opaque)) {
        return false;
    }

    if (stop_fn) {
        stop_fn(stop_opaque, HEDGEHOG_EXEC_STOP_INVALID_MEMORY, &info);
    } else {
        cpu_exit(cpu);
    }
    return true;
}

bool hedgehog_exec_hook_tb_enter(CPUState *cpu, vaddr pc)
{
    return hedgehog_exec_hook_dispatch(cpu, pc, false);
}

bool hedgehog_exec_hook_insn(CPUState *cpu, vaddr pc)
{
    return hedgehog_exec_hook_dispatch(cpu, pc, true);
}
/*
 * Hedgehog backend execution hook registry
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SYSTEM_HEDGEHOG_EXEC_HOOKS_H
#define SYSTEM_HEDGEHOG_EXEC_HOOKS_H

#include "exec/mmu-access-type.h"
#include "system/hedgehog-backend.h"

typedef enum HedgehogExecStopReason {
    HEDGEHOG_EXEC_STOP_REQUESTED = 0,
    HEDGEHOG_EXEC_STOP_INVALID_MEMORY,
} HedgehogExecStopReason;

typedef struct HedgehogInvalidMemInfo {
    vaddr addr;
    unsigned size;
    HedgehogMemAccessType access_type;
    MemTxResult response;
} HedgehogInvalidMemInfo;

typedef void (*HedgehogExecStopFunc)(void *opaque,
                                    HedgehogExecStopReason reason,
                                    const HedgehogInvalidMemInfo *info);

void hedgehog_exec_hook_register_backend(CPUState *cpu, HedgehogBackend *uc,
                                        HedgehogExecStopFunc stop_fn,
                                        void *stop_opaque);
void hedgehog_exec_hook_unregister_backend(CPUState *cpu);

void hedgehog_exec_hook_set_tb(HedgehogBackend *uc,
                              HedgehogExecHookFunc hook_fn,
                              void *opaque);
void hedgehog_exec_hook_set_insn(HedgehogBackend *uc,
                                HedgehogExecHookFunc hook_fn,
                                void *opaque);
void hedgehog_exec_hook_set_invalid(HedgehogBackend *uc,
                                   HedgehogInvalidMemHookFunc hook_fn,
                                   void *opaque);

bool hedgehog_exec_hook_tb_enter(CPUState *cpu, vaddr pc);
bool hedgehog_exec_hook_insn(CPUState *cpu, vaddr pc);
bool hedgehog_exec_hook_invalid(CPUState *cpu, vaddr addr,
                               unsigned size,
                               MMUAccessType access_type,
                               MemTxResult response);

#endif
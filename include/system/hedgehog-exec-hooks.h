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

typedef void (*HedgehogExecStopFunc)(void *opaque,
                                    HedgehogExecStopReason reason,
                                    const HedgehogInvalidMemInfo *info);

typedef bool (*HedgehogInvalidInsnInfoFunc)(
    CPUState *cpu,
    int exception_index,
    HedgehogInvalidInsnInfo *info);

void hedgehog_exec_hook_register_backend(CPUState *cpu, HedgehogBackend *uc,
                                        HedgehogExecStopFunc stop_fn,
                                        void *stop_opaque,
                                        HedgehogInvalidInsnInfoFunc info_fn);
void hedgehog_exec_hook_unregister_backend(CPUState *cpu);

void hedgehog_exec_hook_set_direct_run(CPUState *cpu, bool active);
bool hedgehog_exec_hook_direct_run_active(CPUState *cpu);

void hedgehog_exec_hook_set_tb(HedgehogBackend *uc,
                              HedgehogExecHookFunc hook_fn,
                              void *opaque);
void hedgehog_exec_hook_set_insn(HedgehogBackend *uc,
                                HedgehogExecHookFunc hook_fn,
                                void *opaque);
void hedgehog_exec_hook_set_invalid_insn(
    HedgehogBackend *uc,
    HedgehogInvalidInsnHookFunc hook_fn,
    void *opaque);
void hedgehog_exec_hook_set_invalid(HedgehogBackend *uc,
                                   HedgehogInvalidMemHookFunc hook_fn,
                                   void *opaque);
void hedgehog_exec_hook_set_invalid_mem_diagnostic_capture(
    HedgehogBackend *uc, bool enabled);
bool hedgehog_exec_hook_invalid_mem_diagnostic_capture_enabled(CPUState *cpu);
void hedgehog_exec_hook_clear_invalid_mem_diagnostic(HedgehogBackend *uc);
bool hedgehog_exec_hook_get_last_invalid_mem_diagnostic(
    HedgehogBackend *uc, HedgehogInvalidMemInfo *info);

/* ARM helpers use these to locate their standalone backend after decoding a
 * call. The backend queues host observation for delivery outside BQL. */
void hedgehog_exec_hook_system_call_request(
    CPUState *cpu, HedgehogSystemCallKind kind, vaddr pc,
    unsigned source_el, unsigned immediate, const uint64_t x[8]);
void hedgehog_exec_hook_system_call_route(
    CPUState *cpu, HedgehogSystemCallKind kind, vaddr return_pc,
    unsigned target_el);
void hedgehog_exec_hook_system_call_complete(
    CPUState *cpu, vaddr return_pc, unsigned exception_el,
    unsigned return_el, const uint64_t x[8]);
void hedgehog_exec_hook_cpu_wait(
    CPUState *cpu, HedgehogCPUWaitKind kind, vaddr pc,
    unsigned current_el);

bool hedgehog_exec_hook_tb_enter(CPUState *cpu, vaddr pc);
bool hedgehog_exec_hook_insn(CPUState *cpu, vaddr pc);
HedgehogInvalidInsnDisposition hedgehog_exec_hook_invalid_insn(
    CPUState *cpu,
    vaddr *next_pc);
bool hedgehog_exec_hook_invalid(CPUState *cpu,
                                const HedgehogInvalidMemInfo *info);

#endif

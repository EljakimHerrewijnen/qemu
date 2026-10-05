/*
 * Hedgehog target architecture adapter
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ACCEL_HEDGEHOG_INTERNAL_H
#define ACCEL_HEDGEHOG_INTERNAL_H

#include "hw/core/cpu.h"
#include "system/hedgehog-backend.h"

typedef struct HedgehogArchOps {
    bool (*get_invalid_insn)(CPUState *cpu, int exception_index,
                             HedgehogInvalidInsnInfo *info);
    bool (*apply_aarch64_reset_state)(CPUState *cpu, unsigned current_el,
                                      bool secure, Error **errp);
    bool (*add_aarch64_cp_reg)(CPUState *cpu, unsigned opc0,
                               unsigned opc1, unsigned crn, unsigned crm,
                               unsigned opc2, unsigned min_el,
                               bool readable, bool writable,
                               uint64_t reset_value, Error **errp);
    bool (*set_aarch64_cp_reg_value)(CPUState *cpu, unsigned opc0,
                                     unsigned opc1, unsigned crn,
                                     unsigned crm, unsigned opc2,
                                     uint64_t reset_value, uint64_t value,
                                     Error **errp);
    void (*release_cpu)(CPUState *cpu);
    bool (*arm_diagnostics)(CPUState *cpu,
                            HedgehogArmDiagnostics *diagnostics,
                            Error **errp);
    bool (*restore_arm_generic_timers)(
        CPUState *cpu, const HedgehogArmGenericTimerState *state,
        Error **errp);
} HedgehogArchOps;

const HedgehogArchOps *hedgehog_arch_ops(void);

#endif

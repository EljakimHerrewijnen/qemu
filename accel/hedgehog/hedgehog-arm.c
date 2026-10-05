/*
 * ARM target adapter for the Hedgehog embedding backend
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "qemu/atomic.h"
#include "qemu/timer.h"
#include "system/cpu-timers.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "target/arm/cpregs.h"
#include "target/arm/internals.h"

#include "hedgehog-internal.h"

typedef struct HedgehogArmCPRegState {
    uint32_t key;
    uint64_t reset_value;
    uint64_t value;
} HedgehogArmCPRegState;

static GHashTable *hedgehog_arm_cpreg_banks;

static GHashTable *hedgehog_arm_cpreg_bank(ARMCPU *cpu, bool create)
{
    GHashTable *bank;

    if (!hedgehog_arm_cpreg_banks) {
        if (!create) {
            return NULL;
        }
        hedgehog_arm_cpreg_banks = g_hash_table_new_full(
            g_direct_hash, g_direct_equal, NULL,
            (GDestroyNotify)g_hash_table_unref);
    }

    bank = g_hash_table_lookup(hedgehog_arm_cpreg_banks, cpu);
    if (!bank && create) {
        bank = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                     NULL, g_free);
        g_hash_table_insert(hedgehog_arm_cpreg_banks, cpu, bank);
    }
    return bank;
}

static uint32_t hedgehog_arm_cpreg_key(const ARMCPRegInfo *ri)
{
    return ENCODE_AA64_CP_REG(ri->opc0, ri->opc1, ri->crn, ri->crm,
                              ri->opc2);
}

static HedgehogArmCPRegState *hedgehog_arm_cpreg_state(
    CPUARMState *env, const ARMCPRegInfo *ri)
{
    GHashTable *bank = hedgehog_arm_cpreg_bank(env_archcpu(env), false);

    return bank ? g_hash_table_lookup(
                      bank, GUINT_TO_POINTER(hedgehog_arm_cpreg_key(ri)))
                : NULL;
}

static void hedgehog_arm_sync_cpreg_semantics(ARMCPU *cpu, uint32_t key,
                                               uint64_t value)
{
    CPUARMState *env = &cpu->env;

    if (key == ENCODE_AA64_CP_REG(3, 0, 0, 0, 5)) {
        cpu->mp_affinity = value;
    } else if (key == ENCODE_AA64_CP_REG(3, 3, 0, 0, 1)) {
        cpu->ctr = value;
    } else if (key == ENCODE_AA64_CP_REG(3, 3, 14, 0, 0)) {
        cpu->gt_cntfrq_hz = value;
        env->cp15.c14_cntfrq = value;
        /* Host setup/reset only: counter and timer conversions must agree.
         * Guest CNTFRQ writes change the register, not the physical clock.
         * Reset also cancels timers in gt_timer_reset; cancel here before
         * rescaling so CPREG reset iteration order cannot retain old units.
         */
        for (unsigned i = 0; i < ARRAY_SIZE(cpu->gt_timer); i++) {
            if (cpu->gt_timer[i]) {
                timer_del(cpu->gt_timer[i]);
                cpu->gt_timer[i]->scale = gt_cntfrq_period_ns(cpu);
            }
        }
    }
}

static uint64_t hedgehog_arm_cpreg_read(CPUARMState *env,
                                         const ARMCPRegInfo *ri)
{
    HedgehogArmCPRegState *state = hedgehog_arm_cpreg_state(env, ri);

    g_assert(state);
    return state->value;
}

static void hedgehog_arm_cpreg_write(CPUARMState *env,
                                      const ARMCPRegInfo *ri,
                                      uint64_t value)
{
    HedgehogArmCPRegState *state = hedgehog_arm_cpreg_state(env, ri);

    g_assert(state);
    state->value = value;
}

static void hedgehog_arm_cpreg_reset(CPUARMState *env,
                                      const ARMCPRegInfo *ri)
{
    HedgehogArmCPRegState *state = hedgehog_arm_cpreg_state(env, ri);

    g_assert(state);
    state->value = state->reset_value;
    hedgehog_arm_sync_cpreg_semantics(env_archcpu(env), state->key,
                                      state->reset_value);
}

static bool hedgehog_arm_validate_cpreg_tuple(unsigned opc0, unsigned opc1,
                                               unsigned crn, unsigned crm,
                                               unsigned opc2, Error **errp)
{
    if (opc0 > 3 || opc1 > 7 || crn > 15 || crm > 15 || opc2 > 7) {
        error_setg(errp,
                   "invalid AArch64 CPREG tuple (%u,%u,%u,%u,%u)",
                   opc0, opc1, crn, crm, opc2);
        return false;
    }
    return true;
}

static CPAccessRights hedgehog_arm_cpreg_access(unsigned min_el,
                                                 bool readable,
                                                 bool writable)
{
    static const CPAccessRights read_access[] = {
        PL0_R, PL1_R, PL2_R, PL3_R,
    };
    static const CPAccessRights write_access[] = {
        PL0_W, PL1_W, PL2_W, PL3_W,
    };

    return (readable ? read_access[min_el] : 0) |
           (writable ? write_access[min_el] : 0);
}

static bool hedgehog_arm_define_cpreg(CPUState *cpu, unsigned opc0,
                                       unsigned opc1, unsigned crn,
                                       unsigned crm, unsigned opc2,
                                       CPAccessRights access,
                                       uint64_t reset_value, uint64_t value,
                                       bool override_existing, Error **errp)
{
    ARMCPU *arm_cpu;
    CPUARMState *env;
    const ARMCPRegInfo *existing;
    HedgehogArmCPRegState *state;
    GHashTable *bank;
    uint32_t key;
    g_autofree char *name = NULL;
    ARMCPRegInfo reg = { 0 };

    if (!object_dynamic_cast(OBJECT(cpu), TYPE_ARM_CPU)) {
        error_setg(errp, "AArch64 CPREG configuration requires an ARM CPU");
        return false;
    }
    arm_cpu = ARM_CPU(cpu);
    env = &arm_cpu->env;
    if (!arm_feature(env, ARM_FEATURE_AARCH64) || !env->aarch64) {
        error_setg(errp, "AArch64 CPREG configuration requires an AArch64 CPU");
        return false;
    }
    if (!hedgehog_arm_validate_cpreg_tuple(opc0, opc1, crn, crm, opc2,
                                           errp)) {
        return false;
    }

    key = ENCODE_AA64_CP_REG(opc0, opc1, crn, crm, opc2);
    if (key == ENCODE_AA64_CP_REG(3, 3, 14, 0, 0) &&
        (reset_value == 0 || value == 0)) {
        error_setg(errp, "AArch64 host CNTFRQ frequency must be nonzero");
        return false;
    }
    existing = get_arm_cp_reginfo(arm_cpu->cp_regs, key);
    if ((override_existing && !existing) ||
        (!override_existing && existing)) {
        if (override_existing) {
            error_setg(errp,
                       "AArch64 CPREG tuple (%u,%u,%u,%u,%u) is not defined",
                       opc0, opc1, crn, crm, opc2);
        } else {
            error_setg(errp,
                       "AArch64 CPREG tuple (%u,%u,%u,%u,%u) is already defined",
                       opc0, opc1, crn, crm, opc2);
        }
        return false;
    }

    bank = hedgehog_arm_cpreg_bank(arm_cpu, true);
    state = g_new0(HedgehogArmCPRegState, 1);
    state->key = key;
    state->reset_value = reset_value;
    state->value = value;
    g_hash_table_replace(bank, GUINT_TO_POINTER(key), state);

    name = g_strdup_printf("hedgehog-aarch64-cpreg-s%u_%u_c%u_c%u_%u",
                           opc0, opc1, crn, crm, opc2);
    reg.name = name;
    reg.state = ARM_CP_STATE_AA64;
    reg.opc0 = opc0;
    reg.opc1 = opc1;
    reg.crn = crn;
    reg.crm = crm;
    reg.opc2 = opc2;
    reg.access = override_existing ? existing->access : access;
    reg.accessfn = override_existing ? existing->accessfn : NULL;
    reg.fgt = override_existing ? existing->fgt : 0;
    reg.type = ARM_CP_NO_RAW |
               (override_existing ? ARM_CP_OVERRIDE : 0);
    reg.readfn = hedgehog_arm_cpreg_read;
    reg.writefn = hedgehog_arm_cpreg_write;
    reg.resetfn = hedgehog_arm_cpreg_reset;
    define_one_arm_cp_reg(arm_cpu, &reg);
    hedgehog_arm_sync_cpreg_semantics(arm_cpu, key, value);
    return true;
}

static bool hedgehog_arm_add_aarch64_cp_reg(CPUState *cpu, unsigned opc0,
                                             unsigned opc1, unsigned crn,
                                             unsigned crm, unsigned opc2,
                                             unsigned min_el, bool readable,
                                             bool writable,
                                             uint64_t reset_value,
                                             Error **errp)
{
    if (min_el > 3 || (!readable && !writable)) {
        error_setg(errp,
                   "AArch64 CPREG access requires EL0..EL3 and read or write access");
        return false;
    }
    return hedgehog_arm_define_cpreg(
        cpu, opc0, opc1, crn, crm, opc2,
        hedgehog_arm_cpreg_access(min_el, readable, writable),
        reset_value, reset_value, false, errp);
}

static bool hedgehog_arm_set_aarch64_cp_reg_value(
    CPUState *cpu, unsigned opc0, unsigned opc1, unsigned crn,
    unsigned crm, unsigned opc2, uint64_t reset_value, uint64_t value,
    Error **errp)
{
    return hedgehog_arm_define_cpreg(cpu, opc0, opc1, crn, crm, opc2,
                                     0, reset_value, value, true, errp);
}

static void hedgehog_arm_release_cpu(CPUState *cpu)
{
    if (!hedgehog_arm_cpreg_banks ||
        !object_dynamic_cast(OBJECT(cpu), TYPE_ARM_CPU)) {
        return;
    }
    g_hash_table_remove(hedgehog_arm_cpreg_banks, ARM_CPU(cpu));
    if (g_hash_table_size(hedgehog_arm_cpreg_banks) == 0) {
        g_clear_pointer(&hedgehog_arm_cpreg_banks, g_hash_table_unref);
    }
}

static bool hedgehog_arm_get_invalid_insn(CPUState *cpu, int exception_index,
                                           HedgehogInvalidInsnInfo *info)
{
    ARMCPU *arm_cpu;
    CPUARMState *env;

    if (exception_index != EXCP_UDEF ||
        !object_dynamic_cast(OBJECT(cpu), TYPE_ARM_CPU)) {
        return false;
    }

    arm_cpu = ARM_CPU(cpu);
    env = &arm_cpu->env;
    if (!is_a64(env)) {
        return false;
    }

    memset(info, 0, sizeof(*info));
    info->pc = cpu->cc->get_pc(cpu);
    info->syndrome = env->exception.syndrome;
    info->exception_index = exception_index;
    info->size = 4;
    if (cpu_memory_rw_debug(cpu, info->pc, info->bytes,
                            info->size, false) == 0) {
        info->bytes_len = info->size;
    }
    return true;
}

static bool hedgehog_arm_apply_aarch64_reset_state(CPUState *cpu,
                                                    unsigned current_el,
                                                    bool secure,
                                                    Error **errp)
{
    ARMCPU *arm_cpu;
    CPUARMState *env;

    if (!object_dynamic_cast(OBJECT(cpu), TYPE_ARM_CPU)) {
        error_setg(errp, "AArch64 reset state requires an ARM CPU");
        return false;
    }

    arm_cpu = ARM_CPU(cpu);
    env = &arm_cpu->env;
    if (!arm_feature(env, ARM_FEATURE_AARCH64) || !env->aarch64) {
        error_setg(errp, "AArch64 reset state requires an AArch64 CPU");
        return false;
    }
    if (current_el == 3 && !arm_feature(env, ARM_FEATURE_EL3)) {
        error_setg(errp, "CPU does not implement EL3");
        return false;
    }
    if (current_el == 2 && !arm_feature(env, ARM_FEATURE_EL2)) {
        error_setg(errp, "CPU does not implement EL2");
        return false;
    }

    env->pstate = aarch64_pstate_mode(current_el, true);
    if (secure) {
        env->cp15.scr_el3 &= ~SCR_NS;
    } else {
        env->cp15.scr_el3 |= SCR_NS;
    }
    arm_rebuild_hflags(env);
    return true;
}

static bool hedgehog_arm_diagnostics(CPUState *cpu,
                                     HedgehogArmDiagnostics *diagnostics,
                                     Error **errp)
{
    ARMCPU *arm_cpu;
    CPUARMState *env;
    unsigned timer_index;

    if (!object_dynamic_cast(OBJECT(cpu), TYPE_ARM_CPU)) {
        error_setg(errp, "ARM diagnostics require an ARM CPU");
        return false;
    }

    arm_cpu = ARM_CPU(cpu);
    env = &arm_cpu->env;

    diagnostics->pc = is_a64(env) ? env->pc : env->regs[15];
    diagnostics->pstate = pstate_read(env);
    diagnostics->scr_el3 = env->cp15.scr_el3;
    diagnostics->hcr_el2 = env->cp15.hcr_el2;
    diagnostics->hcr_el2_eff = arm_hcr_el2_eff(env);
    diagnostics->elr_el1 = env->elr_el[1];
    diagnostics->elr_el2 = env->elr_el[2];
    diagnostics->elr_el3 = env->elr_el[3];
    diagnostics->spsr_el1 = env->banked_spsr[aarch64_banked_spsr_index(1)];
    diagnostics->spsr_el2 = env->banked_spsr[aarch64_banked_spsr_index(2)];
    diagnostics->spsr_el3 = env->banked_spsr[aarch64_banked_spsr_index(3)];
    diagnostics->esr_el1 = env->cp15.esr_el[1];
    diagnostics->esr_el2 = env->cp15.esr_el[2];
    diagnostics->esr_el3 = env->cp15.esr_el[3];
    diagnostics->far_el1 = env->cp15.far_el[1];
    diagnostics->far_el2 = env->cp15.far_el[2];
    diagnostics->far_el3 = env->cp15.far_el[3];
    diagnostics->exception_syndrome = env->exception.syndrome;
    diagnostics->exception_vaddress = env->exception.vaddress;
    diagnostics->current_el = arm_current_el(env);
    diagnostics->cpsr = cpsr_read(env);
    diagnostics->interrupt_request = qatomic_read(&cpu->interrupt_request);
    diagnostics->halted = cpu->halted;
    diagnostics->exit_request = qatomic_read(&cpu->exit_request);
    diagnostics->exception_target_el = env->exception.target_el;
    diagnostics->exception_index = cpu->exception_index;
    /*
     * This is deliberately a state snapshot, not a timer service point:
     * reading the architectural counter and c14 records must neither recalc
     * a deadline nor change an output/IRQ level.  QEMU keeps the computed
     * ISTATUS bit in ctl bit 2; enabled and IMASK are ctl bits 0 and 1.
     */
    g_assert(NUM_GTIMERS == HEDGEHOG_ARM_GENERIC_TIMER_COUNT);
    diagnostics->generic_timer_count = NUM_GTIMERS;
    diagnostics->generic_timer_counter = gt_get_countervalue(env);
    for (timer_index = 0; timer_index < NUM_GTIMERS; timer_index++) {
        uint64_t ctl = env->cp15.c14_timer[timer_index].ctl;

        diagnostics->generic_timer_cval[timer_index] =
            env->cp15.c14_timer[timer_index].cval;
        diagnostics->generic_timer_ctl[timer_index] = ctl;
        diagnostics->generic_timer_enabled[timer_index] = ctl & 1;
        diagnostics->generic_timer_imask[timer_index] = (ctl >> 1) & 1;
        diagnostics->generic_timer_istatus[timer_index] = (ctl >> 2) & 1;
    }
    return true;
}

static bool hedgehog_arm_restore_generic_timers(
    CPUState *cpu, const HedgehogArmGenericTimerState *state,
    Error **errp)
{
    ARMCPU *arm_cpu;
    CPUARMState *env;
    unsigned timer_index;

    if (!object_dynamic_cast(OBJECT(cpu), TYPE_ARM_CPU) || !state) {
        error_setg(errp, "ARM generic-timer restore requires ARM CPU state");
        return false;
    }
    if (state->virtual_clock_ns < 0) {
        error_setg(errp, "ARM generic-timer restore has a negative virtual clock");
        return false;
    }

    arm_cpu = ARM_CPU(cpu);
    env = &arm_cpu->env;
    /* This API is setup/restore-only; BQL is held by the backend wrapper. */
    cpu_set_clock(state->virtual_clock_ns);
    for (timer_index = 0; timer_index < NUM_GTIMERS; timer_index++) {
        gt_restore_timer(env, timer_index,
                         state->generic_timer_cval[timer_index],
                         state->generic_timer_ctl[timer_index]);
    }
    return true;
}

const HedgehogArchOps *hedgehog_arch_ops(void)
{
    static const HedgehogArchOps ops = {
        .get_invalid_insn = hedgehog_arm_get_invalid_insn,
        .apply_aarch64_reset_state = hedgehog_arm_apply_aarch64_reset_state,
        .add_aarch64_cp_reg = hedgehog_arm_add_aarch64_cp_reg,
        .set_aarch64_cp_reg_value = hedgehog_arm_set_aarch64_cp_reg_value,
        .release_cpu = hedgehog_arm_release_cpu,
        .arm_diagnostics = hedgehog_arm_diagnostics,
        .restore_arm_generic_timers = hedgehog_arm_restore_generic_timers,
    };

    return &ops;
}

/*
 * Minimal Hedgehog-like embedding backend
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SYSTEM_HEDGEHOG_BACKEND_H
#define SYSTEM_HEDGEHOG_BACKEND_H

#include "qapi/error.h"
#include "exec/hwaddr.h"
#include "exec/vaddr.h"
#include "exec/memattrs.h"
#include "system/memory.h"
#include <stddef.h>

typedef struct CPUState CPUState;
typedef struct AddressSpace AddressSpace;
typedef struct HedgehogBackend HedgehogBackend;

/* ARM's generic timer target has seven architectural timer views.  This
 * fixed-size read-only diagnostic record intentionally mirrors that target
 * contract without exposing the mutable QEMUTimer objects themselves. */
#define HEDGEHOG_ARM_GENERIC_TIMER_COUNT 7

/*
 * A read-only record of an AArch64 SVC, HVC, or SMC that executed on a
 * standalone CPU. Request records carry instruction inputs before routing.
 * Complete records are emitted only after an ERET from the exception level
 * that actually took the call, and carry x0..x7 after that return. Sequence
 * is observer correlation only.
 */
typedef enum HedgehogSystemCallKind {
    HEDGEHOG_SYSTEM_CALL_SVC = 1,
    HEDGEHOG_SYSTEM_CALL_HVC,
    HEDGEHOG_SYSTEM_CALL_SMC,
} HedgehogSystemCallKind;

typedef enum HedgehogSystemCallEventPhase {
    HEDGEHOG_SYSTEM_CALL_EVENT_REQUEST = 1,
    HEDGEHOG_SYSTEM_CALL_EVENT_COMPLETE,
} HedgehogSystemCallEventPhase;

typedef struct HedgehogSystemCallEvent {
    uint64_t sequence;
    uint64_t pc;
    uint64_t return_pc;
    uint64_t x[8];
    uint32_t source_el;
    uint32_t target_el;
    uint32_t return_el;
    uint32_t immediate;
    uint32_t kind;
    uint32_t phase;
} HedgehogSystemCallEvent;

typedef void (*HedgehogSystemCallObserverFunc)(
    void *opaque, const HedgehogSystemCallEvent *event);

/* Setup-only standalone observer. Delivery happens on the direct-run caller
 * outside BQL. A bounded queue may drop its oldest unread records. */
bool hedgehog_backend_connect_system_call_observer(
    HedgehogBackend *uc, HedgehogSystemCallObserverFunc callback,
    void *opaque, Error **errp);

/* Target adapters report already executing AArch64 instructions through these
 * queueing entry points. They never invoke host callbacks in ARM helper code. */
void hedgehog_backend_report_system_call_request(
    HedgehogBackend *uc, HedgehogSystemCallKind kind, vaddr pc,
    unsigned source_el, unsigned immediate, const uint64_t x[8]);
void hedgehog_backend_report_system_call_route(
    HedgehogBackend *uc, HedgehogSystemCallKind kind, vaddr return_pc,
    unsigned target_el);
void hedgehog_backend_report_system_call_complete(
    HedgehogBackend *uc, vaddr return_pc, unsigned exception_el,
    unsigned return_el, const uint64_t x[8]);

/* A read-only record emitted only when the target accepts an architectural
 * wait. It never supplies or modifies guest architectural state. */
typedef enum HedgehogCPUWaitKind {
    HEDGEHOG_CPU_WAIT_WFI = 1,
} HedgehogCPUWaitKind;

typedef struct HedgehogCPUWaitEvent {
    uint64_t pc;
    uint32_t current_el;
    uint32_t kind;
} HedgehogCPUWaitEvent;

typedef void (*HedgehogCPUWaitObserverFunc)(
    void *opaque, const HedgehogCPUWaitEvent *event);

/* Setup-only standalone observer. It runs on the direct-run caller outside
 * BQL and is observational only. */
bool hedgehog_backend_connect_cpu_wait_observer(
    HedgehogBackend *uc, HedgehogCPUWaitObserverFunc callback,
    void *opaque, Error **errp);

/* Target helpers report an accepted wait through this queueing entry point;
 * they never invoke host callbacks while executing guest instructions. */
void hedgehog_backend_report_cpu_wait(
    HedgehogBackend *uc, HedgehogCPUWaitKind kind, vaddr pc,
    unsigned current_el);

typedef void (*HedgehogCPUResetObserverFunc)(void *opaque);

/* Setup-only standalone observer emitted after a queued architectural CPU
 * reset has completed. It runs on the direct-run caller outside BQL and is a
 * notification only: it neither exposes nor modifies guest state. */
bool hedgehog_backend_connect_cpu_reset_observer(
    HedgehogBackend *uc, HedgehogCPUResetObserverFunc callback,
    void *opaque, Error **errp);

typedef void (*HedgehogVirtualTimerFunc)(void *opaque);
bool hedgehog_backend_connect_virtual_timer(HedgehogBackend *uc,
                                             HedgehogVirtualTimerFunc callback,
                                             void *opaque, Error **errp);
bool hedgehog_backend_arm_virtual_timer(HedgehogBackend *uc, int64_t deadline_ns);

typedef void (*HedgehogCPUOutputFunc)(void *opaque, unsigned index, bool level);
bool hedgehog_backend_connect_cpu_output(HedgehogBackend *uc, unsigned index,
                                         HedgehogCPUOutputFunc callback,
                                         void *opaque, Error **errp);


typedef uint64_t (*HedgehogMMIOReadFunc)(void *opaque, hwaddr addr,
                                        unsigned size);
typedef void (*HedgehogMMIOWriteFunc)(void *opaque, hwaddr addr,
                                     uint64_t value, unsigned size);
typedef bool (*HedgehogExecHookFunc)(HedgehogBackend *uc, vaddr pc,
                                    void *opaque);

#define HEDGEHOG_INVALID_INSN_MAX_BYTES 16

typedef struct HedgehogInvalidInsnInfo {
    vaddr pc;
    uint64_t syndrome;
    int32_t exception_index;
    uint8_t size;
    uint8_t bytes_len;
    uint8_t bytes[HEDGEHOG_INVALID_INSN_MAX_BYTES];
} HedgehogInvalidInsnInfo;

typedef enum HedgehogInvalidInsnDisposition {
    HEDGEHOG_INVALID_INSN_PASS = 0,
    HEDGEHOG_INVALID_INSN_CONTINUE,
    HEDGEHOG_INVALID_INSN_STOP,
} HedgehogInvalidInsnDisposition;

typedef HedgehogInvalidInsnDisposition (*HedgehogInvalidInsnHookFunc)(
    HedgehogBackend *uc,
    const HedgehogInvalidInsnInfo *info,
    vaddr *next_pc,
    void *opaque);

typedef enum HedgehogMemAccessType {
    HEDGEHOG_MEM_ACCESS_READ = 0,
    HEDGEHOG_MEM_ACCESS_WRITE,
    HEDGEHOG_MEM_ACCESS_FETCH,
} HedgehogMemAccessType;

/*
 * Bounded, post-failure information for a CPU memory transaction.
 *
 * ``addr`` is always the fault VA. Translation and MemoryRegion fields are
 * valid only when a TLB entry reached a memory transaction. A failed page
 * table walk leaves both validity flags clear. ``region_base`` is relative to
 * the active address space. The fixed name buffer retains no QOM pointer.
 */
#define HEDGEHOG_INVALID_MEM_REGION_NAME_MAX 96

typedef struct HedgehogInvalidMemInfo {
    vaddr addr;
    hwaddr physical_address;
    hwaddr region_base;
    unsigned size;
    int32_t mmu_idx;
    HedgehogMemAccessType access_type;
    MemTxResult response;
    bool translation_valid;
    bool region_valid;
    char region_name[HEDGEHOG_INVALID_MEM_REGION_NAME_MAX];
} HedgehogInvalidMemInfo;

typedef bool (*HedgehogInvalidMemHookFunc)(HedgehogBackend *uc,
                                          vaddr addr,
                                          unsigned size,
                                          HedgehogMemAccessType access_type,
                                          MemTxResult response,
                                          void *opaque);

typedef struct HedgehogArmDiagnostics {
    uint64_t pc;
    uint64_t pstate;
    uint64_t scr_el3;
    uint64_t hcr_el2;
    uint64_t hcr_el2_eff;
    uint64_t elr_el1;
    uint64_t elr_el2;
    uint64_t elr_el3;
    uint64_t spsr_el1;
    uint64_t spsr_el2;
    uint64_t spsr_el3;
    uint64_t esr_el1;
    uint64_t esr_el2;
    uint64_t esr_el3;
    uint64_t far_el1;
    uint64_t far_el2;
    uint64_t far_el3;
    uint64_t exception_syndrome;
    uint64_t exception_vaddress;
    uint32_t current_el;
    uint32_t cpsr;
    uint32_t interrupt_request;
    uint32_t halted;
    uint32_t exit_request;
    uint32_t exception_target_el;
    int32_t exception_index;
    uint32_t generic_timer_count;
    uint64_t generic_timer_counter;
    uint64_t generic_timer_cval[HEDGEHOG_ARM_GENERIC_TIMER_COUNT];
    uint64_t generic_timer_ctl[HEDGEHOG_ARM_GENERIC_TIMER_COUNT];
    uint32_t generic_timer_enabled[HEDGEHOG_ARM_GENERIC_TIMER_COUNT];
    uint32_t generic_timer_imask[HEDGEHOG_ARM_GENERIC_TIMER_COUNT];
    uint32_t generic_timer_istatus[HEDGEHOG_ARM_GENERIC_TIMER_COUNT];
} HedgehogArmDiagnostics;

/*
 * Restorable generic-timer state. The virtual clock is captured separately
 * from the visible tick counter so its sub-tick phase is retained. Timer
 * control bits 0..1 are restored; bit 2 is recomputed by QEMU from cval.
 */
typedef struct HedgehogArmGenericTimerState {
    int64_t virtual_clock_ns;
    uint64_t generic_timer_cval[HEDGEHOG_ARM_GENERIC_TIMER_COUNT];
    uint64_t generic_timer_ctl[HEDGEHOG_ARM_GENERIC_TIMER_COUNT];
} HedgehogArmGenericTimerState;

typedef enum HedgehogRunResult {
    HEDGEHOG_RUN_BUDGET_EXHAUSTED = 0,
    HEDGEHOG_RUN_STOP_REQUESTED,
    HEDGEHOG_RUN_HALTED,
    HEDGEHOG_RUN_EXCEPTION,
    HEDGEHOG_RUN_INVALID_MEMORY,
} HedgehogRunResult;

bool hedgehog_backend_initialize(Error **errp);
bool hedgehog_backend_initialize_for_machine(const char *machine_type,
                                             Error **errp);

bool hedgehog_backend_chardev_add(const char *id, const char *uri,
                                  Error **errp);
bool hedgehog_backend_bind_property(const char *object_path,
                                    const char *property,
                                    const char *value,
                                    Error **errp);
bool hedgehog_backend_chardev_attach_serial(int index, const char *id,
                                            Error **errp);
int hedgehog_backend_chardev_get_endpoint(const char *id, char *buf,
                                          size_t buf_size, Error **errp);
int hedgehog_backend_poll_events(bool blocking, Error **errp);
int64_t hedgehog_backend_virtual_clock_ns(void);

HedgehogBackend *hedgehog_backend_new(const char *cpu_type, Error **errp);
HedgehogBackend *hedgehog_backend_new_with_machine(const char *cpu_type,
                                                   const char *machine_type,
                                                   Error **errp);
void hedgehog_backend_free(HedgehogBackend *uc);

bool hedgehog_backend_map_ram(HedgehogBackend *uc, const char *name,
                             hwaddr addr, uint64_t size, Error **errp);
bool hedgehog_backend_map_ram_ptr(HedgehogBackend *uc, const char *name,
                                 hwaddr addr, uint64_t size, void *ptr,
                                 Error **errp);
bool hedgehog_backend_map_mmio(HedgehogBackend *uc, const char *name,
                              hwaddr addr, uint64_t size,
                              HedgehogMMIOReadFunc read_fn,
                              HedgehogMMIOWriteFunc write_fn,
                              void *opaque, Error **errp);
bool hedgehog_backend_mem_unmap(HedgehogBackend *uc, hwaddr addr,
                               uint64_t size, Error **errp);

MemTxResult hedgehog_backend_mem_read(HedgehogBackend *uc, hwaddr addr,
                                     void *buf, hwaddr len);
MemTxResult hedgehog_backend_mem_write(HedgehogBackend *uc, hwaddr addr,
                                      const void *buf, hwaddr len);

int hedgehog_backend_reg_read(HedgehogBackend *uc, int regno,
                             uint8_t *buf, size_t buf_size,
                             Error **errp);
int hedgehog_backend_reg_write(HedgehogBackend *uc, int regno,
                              const uint8_t *buf, size_t buf_size,
                              Error **errp);

void hedgehog_backend_set_tb_hook(HedgehogBackend *uc,
                                 HedgehogExecHookFunc hook_fn,
                                 void *opaque);
void hedgehog_backend_set_insn_hook(HedgehogBackend *uc,
                                   HedgehogExecHookFunc hook_fn,
                                   void *opaque);
void hedgehog_backend_set_invalid_insn_hook(
    HedgehogBackend *uc,
    HedgehogInvalidInsnHookFunc hook_fn,
    void *opaque);
void hedgehog_backend_set_invalid_mem_hook(HedgehogBackend *uc,
                                          HedgehogInvalidMemHookFunc hook_fn,
                                          void *opaque);

/* Setup-only, host-observational invalid-memory diagnostics. */
bool hedgehog_backend_set_invalid_mem_diagnostic_capture(
    HedgehogBackend *uc, bool enabled, Error **errp);
/* Returns false until a diagnostic has been captured for the current run. */
bool hedgehog_backend_get_last_invalid_mem_diagnostic(
    HedgehogBackend *uc, HedgehogInvalidMemInfo *info);

void hedgehog_backend_reset(HedgehogBackend *uc);
/* Queue a standalone architectural reset for the direct-run boundary.  This
 * API never assigns guest PC, register, or memory state. */
bool hedgehog_backend_request_cpu_reset(HedgehogBackend *uc, Error **errp);
void hedgehog_backend_reset_stop(HedgehogBackend *uc);
void hedgehog_backend_set_pc(HedgehogBackend *uc, vaddr addr);
vaddr hedgehog_backend_get_pc(HedgehogBackend *uc);

HedgehogRunResult hedgehog_backend_run(HedgehogBackend *uc,
                                     uint64_t max_instructions,
                                     int *cpu_exit);
void hedgehog_backend_stop(HedgehogBackend *uc);
bool hedgehog_backend_set_hard_interrupt(HedgehogBackend *uc,
                                         bool asserted,
                                         Error **errp);

bool hedgehog_backend_set_aarch64_reset_state(HedgehogBackend *uc,
                                              unsigned current_el,
                                              bool secure,
                                              Error **errp);
bool hedgehog_backend_add_aarch64_cp_reg(HedgehogBackend *uc,
                                         unsigned opc0,
                                         unsigned opc1,
                                         unsigned crn,
                                         unsigned crm,
                                         unsigned opc2,
                                         unsigned min_el,
                                         bool readable,
                                         bool writable,
                                         uint64_t reset_value,
                                         Error **errp);
bool hedgehog_backend_set_aarch64_cp_reg_value(HedgehogBackend *uc,
                                               unsigned opc0,
                                               unsigned opc1,
                                               unsigned crn,
                                               unsigned crm,
                                               unsigned opc2,
                                               uint64_t reset_value,
                                               uint64_t value,
                                               Error **errp);
bool hedgehog_backend_arm_diagnostics(HedgehogBackend *uc,
                                      HedgehogArmDiagnostics *diagnostics,
                                      Error **errp);
bool hedgehog_backend_restore_arm_generic_timers(
    HedgehogBackend *uc, const HedgehogArmGenericTimerState *state,
    Error **errp);

CPUState *hedgehog_backend_cpu(HedgehogBackend *uc);
AddressSpace *hedgehog_backend_address_space(HedgehogBackend *uc);

#endif

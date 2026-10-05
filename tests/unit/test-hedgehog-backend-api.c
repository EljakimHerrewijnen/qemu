#include "qemu/osdep.h"
#include "system/hedgehog-backend.h"
#include "system/hedgehog-exec-hooks.h"

typedef int hedgehog_reg_read_sig(HedgehogBackend *uc, int regno,
                                 uint8_t *buf, size_t buf_size,
                                 Error **errp);
typedef int hedgehog_reg_write_sig(HedgehogBackend *uc, int regno,
                                  const uint8_t *buf, size_t buf_size,
                                  Error **errp);
typedef bool hedgehog_map_ram_ptr_sig(HedgehogBackend *uc, const char *name,
                                      hwaddr addr, uint64_t size, void *ptr,
                                      Error **errp);
typedef bool hedgehog_chardev_add_sig(const char *id, const char *uri,
                                      Error **errp);
typedef bool hedgehog_bind_property_sig(const char *object_path,
                                        const char *property,
                                        const char *value,
                                        Error **errp);
typedef bool hedgehog_chardev_attach_serial_sig(int index, const char *id,
                                                Error **errp);
typedef int hedgehog_chardev_get_endpoint_sig(const char *id, char *buf,
                                              size_t buf_size, Error **errp);
typedef int hedgehog_poll_events_sig(bool blocking, Error **errp);
typedef HedgehogBackend *hedgehog_new_with_machine_sig(const char *cpu_type,
                                                      const char *machine_type,
                                                      Error **errp);
typedef void hedgehog_stop_sig(HedgehogBackend *uc);
typedef void hedgehog_reset_stop_sig(HedgehogBackend *uc);
typedef bool hedgehog_set_hard_interrupt_sig(HedgehogBackend *uc,
                                             bool asserted,
                                             Error **errp);
typedef void hedgehog_set_invalid_insn_hook_sig(
    HedgehogBackend *uc,
    HedgehogInvalidInsnHookFunc hook_fn,
    void *opaque);
typedef bool hedgehog_set_invalid_mem_diagnostic_capture_sig(
    HedgehogBackend *uc, bool enabled, Error **errp);
typedef bool hedgehog_get_last_invalid_mem_diagnostic_sig(
    HedgehogBackend *uc, HedgehogInvalidMemInfo *info);
typedef bool hedgehog_set_aarch64_reset_state_sig(HedgehogBackend *uc,
                                                  unsigned current_el,
                                                  bool secure,
                                                  Error **errp);
typedef bool hedgehog_add_aarch64_cp_reg_sig(
    HedgehogBackend *uc, unsigned opc0, unsigned opc1, unsigned crn,
    unsigned crm, unsigned opc2, unsigned min_el, bool readable,
    bool writable, uint64_t reset_value, Error **errp);
typedef bool hedgehog_set_aarch64_cp_reg_value_sig(
    HedgehogBackend *uc, unsigned opc0, unsigned opc1, unsigned crn,
    unsigned crm, unsigned opc2, uint64_t reset_value, uint64_t value,
    Error **errp);
typedef bool hedgehog_arm_diagnostics_sig(HedgehogBackend *uc,
                                          HedgehogArmDiagnostics *diagnostics,
                                          Error **errp);
typedef bool hedgehog_connect_system_call_observer_sig(
    HedgehogBackend *uc, HedgehogSystemCallObserverFunc callback,
    void *opaque, Error **errp);
typedef bool hedgehog_connect_cpu_wait_observer_sig(
    HedgehogBackend *uc, HedgehogCPUWaitObserverFunc callback,
    void *opaque, Error **errp);
typedef bool hedgehog_connect_cpu_reset_observer_sig(
    HedgehogBackend *uc, HedgehogCPUResetObserverFunc callback,
    void *opaque, Error **errp);
typedef bool hedgehog_request_cpu_reset_sig(HedgehogBackend *uc,
                                            Error **errp);

_Static_assert(__builtin_types_compatible_p(__typeof__(hedgehog_backend_reg_read),
                                            hedgehog_reg_read_sig),
               "hedgehog_backend_reg_read signature mismatch");
_Static_assert(__builtin_types_compatible_p(__typeof__(hedgehog_backend_reg_write),
                                            hedgehog_reg_write_sig),
               "hedgehog_backend_reg_write signature mismatch");
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(hedgehog_backend_map_ram_ptr),
                   hedgehog_map_ram_ptr_sig),
               "hedgehog_backend_map_ram_ptr signature mismatch");
_Static_assert(__builtin_types_compatible_p(__typeof__(hedgehog_backend_chardev_add),
                                            hedgehog_chardev_add_sig),
               "hedgehog_backend_chardev_add signature mismatch");
_Static_assert(__builtin_types_compatible_p(__typeof__(hedgehog_backend_bind_property),
                                            hedgehog_bind_property_sig),
               "hedgehog_backend_bind_property signature mismatch");
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(hedgehog_backend_chardev_attach_serial),
                   hedgehog_chardev_attach_serial_sig),
               "hedgehog_backend_chardev_attach_serial signature mismatch");
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(hedgehog_backend_chardev_get_endpoint),
                   hedgehog_chardev_get_endpoint_sig),
               "hedgehog_backend_chardev_get_endpoint signature mismatch");
_Static_assert(__builtin_types_compatible_p(__typeof__(hedgehog_backend_poll_events),
                                            hedgehog_poll_events_sig),
               "hedgehog_backend_poll_events signature mismatch");
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(hedgehog_backend_new_with_machine),
                   hedgehog_new_with_machine_sig),
               "hedgehog_backend_new_with_machine signature mismatch");
_Static_assert(__builtin_types_compatible_p(__typeof__(hedgehog_backend_stop),
                                            hedgehog_stop_sig),
               "hedgehog_backend_stop signature mismatch");
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(hedgehog_backend_reset_stop),
                   hedgehog_reset_stop_sig),
               "hedgehog_backend_reset_stop signature mismatch");
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(hedgehog_backend_set_hard_interrupt),
                   hedgehog_set_hard_interrupt_sig),
               "hedgehog_backend_set_hard_interrupt signature mismatch");
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(hedgehog_backend_set_invalid_insn_hook),
                   hedgehog_set_invalid_insn_hook_sig),
               "hedgehog_backend_set_invalid_insn_hook signature mismatch");
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(hedgehog_backend_set_invalid_mem_diagnostic_capture),
                   hedgehog_set_invalid_mem_diagnostic_capture_sig),
               "hedgehog_backend_set_invalid_mem_diagnostic_capture signature mismatch");
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(hedgehog_backend_get_last_invalid_mem_diagnostic),
                   hedgehog_get_last_invalid_mem_diagnostic_sig),
               "hedgehog_backend_get_last_invalid_mem_diagnostic signature mismatch");
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(hedgehog_backend_set_aarch64_reset_state),
                   hedgehog_set_aarch64_reset_state_sig),
               "hedgehog_backend_set_aarch64_reset_state signature mismatch");
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(hedgehog_backend_add_aarch64_cp_reg),
                   hedgehog_add_aarch64_cp_reg_sig),
               "hedgehog_backend_add_aarch64_cp_reg signature mismatch");
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(hedgehog_backend_set_aarch64_cp_reg_value),
                   hedgehog_set_aarch64_cp_reg_value_sig),
               "hedgehog_backend_set_aarch64_cp_reg_value signature mismatch");
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(hedgehog_backend_arm_diagnostics),
                   hedgehog_arm_diagnostics_sig),
               "hedgehog_backend_arm_diagnostics signature mismatch");
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(hedgehog_backend_connect_system_call_observer),
                   hedgehog_connect_system_call_observer_sig),
               "hedgehog_backend_connect_system_call_observer signature mismatch");
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(hedgehog_backend_connect_cpu_wait_observer),
                   hedgehog_connect_cpu_wait_observer_sig),
               "hedgehog_backend_connect_cpu_wait_observer signature mismatch");
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(hedgehog_backend_connect_cpu_reset_observer),
                   hedgehog_connect_cpu_reset_observer_sig),
               "hedgehog_backend_connect_cpu_reset_observer signature mismatch");
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(hedgehog_backend_request_cpu_reset),
                   hedgehog_request_cpu_reset_sig),
               "hedgehog_backend_request_cpu_reset signature mismatch");
_Static_assert(offsetof(HedgehogArmDiagnostics, pc) == 0,
               "HedgehogArmDiagnostics.pc must stay first");
_Static_assert(sizeof(HedgehogArmDiagnostics) >= 392,
               "HedgehogArmDiagnostics unexpectedly small");
_Static_assert(offsetof(HedgehogArmDiagnostics, generic_timer_counter) >
               offsetof(HedgehogArmDiagnostics, exception_index),
               "generic timer diagnostics must append to CPU diagnostics");
_Static_assert(offsetof(HedgehogInvalidMemInfo, addr) == 0,
               "HedgehogInvalidMemInfo.addr must stay first");
_Static_assert(offsetof(HedgehogInvalidMemInfo, region_name) >
               offsetof(HedgehogInvalidMemInfo, region_valid),
               "HedgehogInvalidMemInfo validity fields must precede name");

static bool noop_exec_hook(HedgehogBackend *uc, vaddr pc, void *opaque)
{
    return uc != NULL || pc != 0 || opaque != NULL;
}

static bool noop_invalid_hook(HedgehogBackend *uc,
                              vaddr addr,
                              unsigned size,
                              HedgehogMemAccessType access_type,
                              MemTxResult response,
                              void *opaque)
{
    return uc != NULL || addr != 0 || size != 0 ||
           access_type != HEDGEHOG_MEM_ACCESS_READ ||
           response != MEMTX_OK || opaque != NULL;
}

static HedgehogInvalidInsnDisposition noop_invalid_insn_hook(
    HedgehogBackend *uc,
    const HedgehogInvalidInsnInfo *info,
    vaddr *next_pc,
    void *opaque)
{
    if (uc && info && next_pc && opaque) {
        *next_pc = info->pc + info->size;
        return HEDGEHOG_INVALID_INSN_CONTINUE;
    }
    return HEDGEHOG_INVALID_INSN_PASS;
}

static void noop_system_call_observer(
    void *opaque, const HedgehogSystemCallEvent *event)
{
    g_assert_true(opaque == NULL);
    g_assert_nonnull(event);
}

static void noop_cpu_wait_observer(
    void *opaque, const HedgehogCPUWaitEvent *event)
{
    g_assert_true(opaque == NULL);
    g_assert_nonnull(event);
}

static void noop_cpu_reset_observer(void *opaque)
{
    g_assert_true(opaque == NULL);
}

static void test_hedgehog_backend_api_constants(void)
{
    g_assert_cmpint(HEDGEHOG_RUN_BUDGET_EXHAUSTED, ==, 0);
    g_assert_cmpint(HEDGEHOG_RUN_STOP_REQUESTED, ==, 1);
    g_assert_cmpint(HEDGEHOG_RUN_HALTED, ==, 2);
    g_assert_cmpint(HEDGEHOG_RUN_EXCEPTION, ==, 3);
    g_assert_cmpint(HEDGEHOG_RUN_INVALID_MEMORY, ==, 4);

    g_assert_cmpint(HEDGEHOG_MEM_ACCESS_READ, ==, 0);
    g_assert_cmpint(HEDGEHOG_MEM_ACCESS_WRITE, ==, 1);
    g_assert_cmpint(HEDGEHOG_MEM_ACCESS_FETCH, ==, 2);

    g_assert_cmpint(HEDGEHOG_EXEC_STOP_REQUESTED, ==, 0);
    g_assert_cmpint(HEDGEHOG_EXEC_STOP_INVALID_MEMORY, ==, 1);

    g_assert_cmpint(HEDGEHOG_INVALID_INSN_PASS, ==, 0);
    g_assert_cmpint(HEDGEHOG_INVALID_INSN_CONTINUE, ==, 1);
    g_assert_cmpint(HEDGEHOG_INVALID_INSN_STOP, ==, 2);

    g_assert_cmpint(HEDGEHOG_SYSTEM_CALL_SVC, ==, 1);
    g_assert_cmpint(HEDGEHOG_SYSTEM_CALL_HVC, ==, 2);
    g_assert_cmpint(HEDGEHOG_SYSTEM_CALL_SMC, ==, 3);
    g_assert_cmpint(HEDGEHOG_SYSTEM_CALL_EVENT_REQUEST, ==, 1);
    g_assert_cmpint(HEDGEHOG_SYSTEM_CALL_EVENT_COMPLETE, ==, 2);
    g_assert_cmpint(HEDGEHOG_CPU_WAIT_WFI, ==, 1);
}

static void test_hedgehog_backend_api_types(void)
{
    HedgehogExecHookFunc exec_hook = noop_exec_hook;
    HedgehogInvalidMemHookFunc invalid_hook = noop_invalid_hook;
    HedgehogInvalidInsnHookFunc invalid_insn_hook = noop_invalid_insn_hook;
    HedgehogSystemCallObserverFunc observer = noop_system_call_observer;
    HedgehogCPUWaitObserverFunc cpu_wait_observer = noop_cpu_wait_observer;
    HedgehogCPUResetObserverFunc cpu_reset_observer = noop_cpu_reset_observer;
    HedgehogSystemCallEvent system_call = {
        .kind = HEDGEHOG_SYSTEM_CALL_SVC,
        .target_el = UINT32_MAX,
        .phase = HEDGEHOG_SYSTEM_CALL_EVENT_REQUEST,
    };
    HedgehogInvalidMemInfo info = {
        .addr = 0x1000,
        .physical_address = 0x2000,
        .region_base = 0,
        .size = 4,
        .mmu_idx = 3,
        .access_type = HEDGEHOG_MEM_ACCESS_FETCH,
        .response = MEMTX_DECODE_ERROR,
        .translation_valid = true,
        .region_valid = true,
        .region_name = "unassigned",
    };
    HedgehogCPUWaitEvent cpu_wait = {
        .pc = 0x1234,
        .current_el = 3,
        .kind = HEDGEHOG_CPU_WAIT_WFI,
    };

    g_assert_true(exec_hook != NULL);
    g_assert_true(invalid_hook != NULL);
    g_assert_true(invalid_insn_hook != NULL);
    g_assert_true(observer != NULL);
    g_assert_true(cpu_wait_observer != NULL);
    g_assert_true(cpu_reset_observer != NULL);
    g_assert_cmpint(system_call.kind, ==, HEDGEHOG_SYSTEM_CALL_SVC);
    g_assert_cmpuint(system_call.target_el, ==, UINT32_MAX);
    g_assert_cmpint(system_call.phase, ==, HEDGEHOG_SYSTEM_CALL_EVENT_REQUEST);
    g_assert_cmphex(info.addr, ==, 0x1000);
    g_assert_cmphex(info.physical_address, ==, 0x2000);
    g_assert_cmphex(info.region_base, ==, 0);
    g_assert_cmpuint(info.size, ==, 4);
    g_assert_cmpint(info.mmu_idx, ==, 3);
    g_assert_cmpint(info.access_type, ==, HEDGEHOG_MEM_ACCESS_FETCH);
    g_assert_cmpint(info.response, ==, MEMTX_DECODE_ERROR);
    g_assert_true(info.translation_valid);
    g_assert_true(info.region_valid);
    g_assert_cmpstr(info.region_name, ==, "unassigned");
    g_assert_cmphex(cpu_wait.pc, ==, 0x1234);
    g_assert_cmpuint(cpu_wait.current_el, ==, 3);
    g_assert_cmpint(cpu_wait.kind, ==, HEDGEHOG_CPU_WAIT_WFI);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/hedgehog-backend/api/constants",
                    test_hedgehog_backend_api_constants);
    g_test_add_func("/hedgehog-backend/api/types",
                    test_hedgehog_backend_api_types);

    return g_test_run();
}

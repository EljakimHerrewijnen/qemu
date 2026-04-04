#include "qemu/osdep.h"
#include "system/hedgehog-backend.h"
#include "system/hedgehog-exec-hooks.h"

typedef int hedgehog_reg_read_sig(HedgehogBackend *uc, int regno,
                                 uint8_t *buf, size_t buf_size,
                                 Error **errp);
typedef int hedgehog_reg_write_sig(HedgehogBackend *uc, int regno,
                                  const uint8_t *buf, size_t buf_size,
                                  Error **errp);
typedef HedgehogBackend *hedgehog_new_with_machine_sig(const char *cpu_type,
                                                      const char *machine_type,
                                                      Error **errp);

_Static_assert(__builtin_types_compatible_p(__typeof__(hedgehog_backend_reg_read),
                                            hedgehog_reg_read_sig),
               "hedgehog_backend_reg_read signature mismatch");
_Static_assert(__builtin_types_compatible_p(__typeof__(hedgehog_backend_reg_write),
                                            hedgehog_reg_write_sig),
               "hedgehog_backend_reg_write signature mismatch");
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(hedgehog_backend_new_with_machine),
                   hedgehog_new_with_machine_sig),
               "hedgehog_backend_new_with_machine signature mismatch");

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
}

static void test_hedgehog_backend_api_types(void)
{
    HedgehogExecHookFunc exec_hook = noop_exec_hook;
    HedgehogInvalidMemHookFunc invalid_hook = noop_invalid_hook;
    HedgehogInvalidMemInfo info = {
        .addr = 0x1000,
        .size = 4,
        .access_type = HEDGEHOG_MEM_ACCESS_FETCH,
        .response = MEMTX_DECODE_ERROR,
    };

    g_assert_true(exec_hook != NULL);
    g_assert_true(invalid_hook != NULL);
    g_assert_cmphex(info.addr, ==, 0x1000);
    g_assert_cmpuint(info.size, ==, 4);
    g_assert_cmpint(info.access_type, ==, HEDGEHOG_MEM_ACCESS_FETCH);
    g_assert_cmpint(info.response, ==, MEMTX_DECODE_ERROR);
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

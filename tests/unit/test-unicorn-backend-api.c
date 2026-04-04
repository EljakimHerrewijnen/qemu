#include "qemu/osdep.h"
#include "system/unicorn-backend.h"

static void test_unicorn_backend_api_constants(void)
{
    g_assert_cmpint(UNICORN_RUN_BUDGET_EXHAUSTED, ==, 0);
    g_assert_cmpint(UNICORN_RUN_STOP_REQUESTED, ==, 1);
    g_assert_cmpint(UNICORN_RUN_HALTED, ==, 2);
    g_assert_cmpint(UNICORN_RUN_EXCEPTION, ==, 3);
}

/*
 * Verify that the machine-mode creation function signature is present and
 * correctly typed.  This does not call the function; it only takes its address
 * so the linker can confirm the symbol exists in the API header.
 */
static void test_unicorn_backend_api_machine_mode_symbol(void)
{
    UnicornBackend *(*fn)(const char *, uint64_t, Error **) =
        unicorn_backend_new_machine;
    g_assert_nonnull((void *)fn);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/unicorn-backend/api/constants",
                    test_unicorn_backend_api_constants);
    g_test_add_func("/unicorn-backend/api/machine-mode-symbol",
                    test_unicorn_backend_api_machine_mode_symbol);

    return g_test_run();
}

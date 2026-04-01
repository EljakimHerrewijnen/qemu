#include "qemu/osdep.h"
#include "system/unicorn-backend.h"

static void test_unicorn_backend_api_constants(void)
{
    g_assert_cmpint(UNICORN_RUN_BUDGET_EXHAUSTED, ==, 0);
    g_assert_cmpint(UNICORN_RUN_STOP_REQUESTED, ==, 1);
    g_assert_cmpint(UNICORN_RUN_HALTED, ==, 2);
    g_assert_cmpint(UNICORN_RUN_EXCEPTION, ==, 3);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/unicorn-backend/api/constants",
                    test_unicorn_backend_api_constants);

    return g_test_run();
}

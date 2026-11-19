#include "oracis/host/runtime_bridge.h"

#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "compiled_bank_link_smoke: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    if (oracis_host_runtime_initialize() != ORACIS_HOST_RUNTIME_OK) {
        return fail("host runtime initialization failed");
    }

    if (oracis_host_runtime_register_compiled_banks() !=
            ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_has_program_banks() != 1) {
        (void)oracis_host_runtime_shutdown();
        return fail("compiled ARM9/ARM7 tables were not registered");
    }

    if (oracis_host_runtime_program_loaded() != 0 ||
        oracis_host_runtime_run_round(1u, nullptr, nullptr) !=
            ORACIS_HOST_RUNTIME_NO_PROGRAM_LOADED ||
        oracis_host_runtime_global_bus_cycles() != 0u) {
        (void)oracis_host_runtime_shutdown();
        return fail("host executed compiled banks before loading program images");
    }

    if (oracis_host_runtime_shutdown() != ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_has_program_banks() != 0) {
        return fail("compiled bank tables were not unregistered at shutdown");
    }

    std::puts("optional compiled bank link/registration smoke test passed");
    return 0;
}

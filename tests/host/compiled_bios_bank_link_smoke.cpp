#include "oracis/host/runtime_bridge.h"

#include <cstdio>

int main()
{
    if (oracis_host_runtime_initialize() != ORACIS_HOST_RUNTIME_OK) {
        std::fprintf(stderr, "oracis_bios_bank_link_smoke: machine initialization failed\\n");
        return 1;
    }
    const int registration = oracis_host_runtime_register_user_bios_banks();
    if ((registration != ORACIS_HOST_RUNTIME_OK &&
         registration != ORACIS_HOST_RUNTIME_BANKS_ALREADY_REGISTERED) ||
        oracis_host_runtime_has_program_banks() != 1) {
        std::fprintf(stderr,
            "oracis_bios_bank_link_smoke: BIOS bank registration failed (bridge=%d)\\n",
            registration);
        (void)oracis_host_runtime_shutdown();
        return 1;
    }
    if (oracis_host_runtime_shutdown() != ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_has_program_banks() != 0) {
        std::fprintf(stderr, "oracis_bios_bank_link_smoke: shutdown did not clear registry\\n");
        return 1;
    }
    std::puts("user BIOS C-bank link/register/unregister smoke passed");
    return 0;
}

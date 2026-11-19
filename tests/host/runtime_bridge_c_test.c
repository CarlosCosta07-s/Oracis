#include "oracis/host/runtime_bridge.h"
#include "oracis/memory/guest_memory_abi.h"

#include <stdint.h>
#include <stdio.h>

int main(void)
{
    /* Host input can arrive before NdsMachine binds its keypad device. */
    oracis_host_set_keypad_state(UINT16_C(1));

    if (oracis_host_runtime_is_initialized() != 0) {
        fprintf(stderr, "host runtime was initialized before the test\n");
        return 1;
    }

    if (oracis_host_runtime_initialize() != ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_is_initialized() != 1 ||
        oracis_host_runtime_global_bus_cycles() != UINT64_C(0)) {
        fprintf(stderr, "C host bridge failed to initialize NdsMachine\n");
        return 1;
    }

    if (oracis_host_runtime_initialize() !=
        ORACIS_HOST_RUNTIME_ALREADY_INITIALIZED) {
        fprintf(stderr, "C host bridge accepted a duplicate initialization\n");
        return 1;
    }

    if (oracis_host_runtime_shutdown() != ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_is_initialized() != 0 ||
        oracis_host_runtime_last_machine_status() != 0) {
        fprintf(stderr, "C host bridge failed to shut down NdsMachine\n");
        return 1;
    }

    if (oracis_host_runtime_shutdown() != ORACIS_HOST_RUNTIME_OK) {
        fprintf(stderr, "C host bridge shutdown was not idempotent\n");
        return 1;
    }

    oracis_host_set_keypad_state(0u);
    puts("C host runtime bridge lifecycle/keypad tests passed");
    return 0;
}

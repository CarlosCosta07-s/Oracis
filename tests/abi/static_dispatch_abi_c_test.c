#include "oracis/recomp/static_dispatch_abi.h"

#include <stdint.h>
#include <stdio.h>

static unsigned g_calls = 0u;

static void generated_bank_body(void)
{
    ++g_calls;
}

int main(void)
{
    ArmCpuState arm9 = {0};
    ArmCpuState arm7 = {0};
    arm9.cpsr = UINT32_C(0x1F);
    arm7.cpsr = UINT32_C(0x1F);

    if (!oracis_arm_runtime_bind_cpus(&arm9, &arm7)) {
        fputs("C99 dispatch ABI: CPU bind failed\n", stderr);
        return 1;
    }

    static const NdsDispatchEntry rows[] = {
        {0x02008000u, 0u, generated_bank_body, NULL},
    };
    if (oracis_static_dispatch_register_bank(
            NDS_ARM9, rows, 1u, "c99-bank") != 0) {
        fputs("C99 dispatch ABI: registration failed\n", stderr);
        return 1;
    }

    uint32_t status = UINT32_MAX;
    uint32_t count = 0u;
    const NdsDispatchEntry *inactive = NULL;
    const NdsDispatchEntry *entry = oracis_static_dispatch_lookup(
        NDS_ARM9, 0x02008000u, arm9.cpsr, &status, &count, &inactive);
    if (entry != &rows[0] || status != 0u || count != 1u || inactive != NULL) {
        fputs("C99 dispatch ABI: lookup failed\n", stderr);
        return 1;
    }

    runtime_dispatch(0x02008000u);
    if (g_calls != 1u) {
        fputs("C99 dispatch ABI: runtime dispatch failed\n", stderr);
        return 1;
    }

    if (!oracis_static_dispatch_unregister_bank(NDS_ARM9, rows, 1u)) {
        fputs("C99 dispatch ABI: unregister failed\n", stderr);
        return 1;
    }
    oracis_static_dispatch_clear();
    puts("C99 static dispatch ABI test passed");
    return 0;
}

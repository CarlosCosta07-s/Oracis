#include "oracis/recomp/runtime_arm.h"

#include <stdio.h>

#ifndef NDS_STATIC_CPU
#error NDS_STATIC_CPU must be defined by the bank build
#endif

#if NDS_STATIC_CPU == 0
#define ORACIS_EXPECTED_CPU NDS_ARM9
#elif NDS_STATIC_CPU == 1
#define ORACIS_EXPECTED_CPU NDS_ARM7
#else
#error NDS_STATIC_CPU must be 0 (ARM9) or 1 (ARM7)
#endif

int main(void)
{
    if (g_nds_active != ORACIS_EXPECTED_CPU) {
        fputs("NDS_STATIC_CPU did not select the expected core\n", stderr);
        return 1;
    }
    return 0;
}

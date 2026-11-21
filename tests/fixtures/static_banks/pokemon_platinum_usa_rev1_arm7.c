#include "oracis/recomp/runtime_arm.h"
#include "pokemon_platinum_usa_rev1_arm7.h"

void oracis_fixture_arm7(void)
{
    bus_write_u32(UINT32_C(0x02002004), UINT32_C(0xA7A70001));
    runtime_tick(1u);
}

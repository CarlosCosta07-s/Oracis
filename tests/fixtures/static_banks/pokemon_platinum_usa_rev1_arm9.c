#include "oracis/recomp/runtime_arm.h"
#include "pokemon_platinum_usa_rev1_arm9.h"

void oracis_fixture_arm9(void)
{
    bus_write_u32(UINT32_C(0x02002000), UINT32_C(0xA9A90001));
    runtime_tick(1u);
}

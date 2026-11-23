#include "oracis/recomp/runtime_arm.h"
#include "pokemon_platinum_usa_rev1_arm9.h"

/*
 * Synthetic linked-bank callsite. This exercises the same C ABI used by
 * emitted banks without including any game-derived code or data.
 */
static NdsLinkSlot g_fixture_arm9_leaf_slot = {
    0,
    0u,
    UINT32_C(0x02001004),
    0
};

void oracis_fixture_arm9_leaf(void)
{
    bus_write_u32(UINT32_C(0x02002008), UINT32_C(0xA9A90002));
}

void oracis_fixture_arm9(void)
{
    runtime_link_call(&g_fixture_arm9_leaf_slot);
    bus_write_u32(UINT32_C(0x02002000), UINT32_C(0xA9A90001));
    runtime_tick(1u);
}

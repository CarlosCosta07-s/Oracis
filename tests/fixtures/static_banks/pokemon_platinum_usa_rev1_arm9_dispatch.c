#include "oracis/recomp/runtime_arm.h"
#include "pokemon_platinum_usa_rev1_arm9.h"

const NdsDispatchEntry g_dispatch_pokemon_platinum_usa_rev1_arm9[] = {
    {UINT32_C(0x02001000), 0u, &oracis_fixture_arm9, 0}
};
const unsigned g_dispatch_pokemon_platinum_usa_rev1_arm9_len = 1u;

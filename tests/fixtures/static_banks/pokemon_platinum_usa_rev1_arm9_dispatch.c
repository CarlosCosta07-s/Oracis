#include "oracis/recomp/runtime_arm.h"
#include "pokemon_platinum_usa_rev1_arm9.h"

extern const NdsStaticValidation g_fixture_arm9_leaf_validation;

const NdsDispatchEntry g_dispatch_pokemon_platinum_usa_rev1_arm9[] = {
    {UINT32_C(0x02001000), 0u, &oracis_fixture_arm9, 0},
    {UINT32_C(0x02001004), 0u, &oracis_fixture_arm9_leaf,
        &g_fixture_arm9_leaf_validation}
};
const unsigned g_dispatch_pokemon_platinum_usa_rev1_arm9_len = 2u;

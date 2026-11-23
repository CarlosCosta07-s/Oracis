#include "oracis/recomp/runtime_arm.h"
#include "pokemon_platinum_usa_rev1_arm9.h"

/*
 * Synthetic linked-bank callsite with a real GuestMemory-backed validation
 * row. The test bank contains no game-derived code or data.
 */
static NdsLinkSlot g_fixture_arm9_leaf_slot = {
    0,
    0u,
    UINT32_C(0x02001004),
    0
};

static uint32_t g_fixture_arm9_leaf_calls = 0u;
static const uint8_t g_fixture_arm9_leaf_expected[] = {
    UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0xA0), UINT8_C(0xE1)
};

const NdsStaticValidation g_fixture_arm9_leaf_validation = {
    UINT32_C(0x02001004),
    (uint32_t)sizeof(g_fixture_arm9_leaf_expected),
    g_fixture_arm9_leaf_expected,
    0,
    0u
};

void oracis_fixture_arm9_leaf(void)
{
    /* Host-only counter: do not mutate GuestMemory before cache assertions. */
    ++g_fixture_arm9_leaf_calls;
}

void oracis_fixture_arm9(void)
{
    g_fixture_arm9_leaf_calls = 0u;
    runtime_link_call(&g_fixture_arm9_leaf_slot);

    const uint32_t inline_before_mutation =
        (uint32_t)runtime_inline_leaf_admit(
            &g_fixture_arm9_leaf_slot, &oracis_fixture_arm9_leaf);
    if (inline_before_mutation != 0u) {
        oracis_fixture_arm9_leaf();
    }

    /* An unrelated guest write must invalidate every cached link globally. */
    bus_write_u8(UINT32_C(0x0200200C), UINT8_C(0x5A));

    const uint32_t inline_after_mutation =
        (uint32_t)runtime_inline_leaf_admit(
            &g_fixture_arm9_leaf_slot, &oracis_fixture_arm9_leaf);
    if (inline_after_mutation != 0u) {
        oracis_fixture_arm9_leaf();
    } else {
        runtime_link_call(&g_fixture_arm9_leaf_slot);
    }

    bus_write_u32(UINT32_C(0x02002008), g_fixture_arm9_leaf_calls);
    bus_write_u32(
        UINT32_C(0x02002010),
        inline_before_mutation == 1u && inline_after_mutation == 0u
            ? UINT32_C(0xA9A90003)
            : UINT32_C(0xA9A9FFFF));
    bus_write_u32(UINT32_C(0x02002000), UINT32_C(0xA9A90001));
    runtime_tick(1u);
}

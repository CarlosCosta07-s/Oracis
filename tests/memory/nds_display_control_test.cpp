#include "oracis/memory/nds_display_control.hpp"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_display_control_test: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    using oracis::memory::NdsDisplayControl;
    using oracis::memory::Status;

    NdsDisplayControl display;
    if (display.install() != Status::Ok || !display.installed() ||
        display.install() != Status::InvalidRegion) {
        return fail("display register device initialization failed");
    }

    constexpr uint32_t engine_a_dispcnt = UINT32_C(0x04000000);
    constexpr uint32_t engine_b_dispcnt = UINT32_C(0x04001000);
    constexpr uint32_t engine_a_bg0 = UINT32_C(0x04000008);
    constexpr uint32_t engine_b_bg0 = UINT32_C(0x04001008);
    constexpr uint32_t engine_a_config = UINT32_C(0xD1234567);
    constexpr uint32_t engine_b_config = UINT32_C(0xFFFFFFFF);

    if (display.write(
            NDS_ARM9, engine_a_dispcnt, 4u, engine_a_config) != Status::Ok ||
        display.read(NDS_ARM9, engine_a_dispcnt, 4u).value !=
            engine_a_config) {
        return fail("Engine A DISPCNT did not retain all 32 configuration bits");
    }

    if (display.write(
            NDS_ARM9, engine_b_dispcnt, 4u, engine_b_config) != Status::Ok ||
        display.read(NDS_ARM9, engine_b_dispcnt, 4u).value !=
            NdsDisplayControl::EngineBDispcntSupportedMask) {
        return fail("Engine B DISPCNT did not clear Engine-A-only fields");
    }

    if (display.read(NDS_ARM9, engine_a_dispcnt, 4u).value !=
            engine_a_config) {
        return fail("Engine A state was changed by Engine B configuration");
    }

    constexpr uint16_t engine_a_bg_values[] = {
        UINT16_C(0xD012), UINT16_C(0xA345),
        UINT16_C(0xB678), UINT16_C(0xC9AB)
    };
    constexpr uint16_t engine_b_bg_values[] = {
        UINT16_C(0x1234), UINT16_C(0x5678),
        UINT16_C(0x9ABC), UINT16_C(0xDEF0)
    };
    for (unsigned bg = 0u; bg < NdsDisplayControl::BgCount; ++bg) {
        const uint32_t a_address = engine_a_bg0 + bg * 2u;
        const uint32_t b_address = engine_b_bg0 + bg * 2u;
        if (display.write(
                NDS_ARM9, a_address, 2u, engine_a_bg_values[bg]) != Status::Ok ||
            display.write(
                NDS_ARM9, b_address, 2u, engine_b_bg_values[bg]) != Status::Ok ||
            display.read(NDS_ARM9, a_address, 2u).value !=
                engine_a_bg_values[bg] ||
            display.read(NDS_ARM9, b_address, 2u).value !=
                engine_b_bg_values[bg]) {
            return fail("BG0CNT-BG3CNT engine-specific storage failed");
        }
    }

    /* Aligned STR/LDR to BG0CNT + BG1CNT packs two adjacent halfwords. */
    constexpr uint32_t packed_bg_pair = UINT32_C(0xABCD1234);
    if (display.write(
            NDS_ARM9, engine_a_bg0, 4u, packed_bg_pair) != Status::Ok ||
        display.read(NDS_ARM9, engine_a_bg0, 4u).value != packed_bg_pair ||
        display.read(NDS_ARM9, engine_a_bg0, 2u).value !=
            UINT16_C(0x1234) ||
        display.read(NDS_ARM9, engine_a_bg0 + 2u, 2u).value !=
            UINT16_C(0xABCD)) {
        return fail("packed 32-bit BG register pair access failed");
    }

    if (display.read(NDS_ARM9, engine_a_dispcnt, 2u).status !=
            Status::InvalidWidth ||
        display.write(
            NDS_ARM9, engine_a_dispcnt, 2u, UINT32_C(0x1234)) !=
            Status::InvalidWidth ||
        display.read(NDS_ARM9, engine_a_bg0, 4u).status !=
            Status::Ok ||
        display.read(NDS_ARM9, engine_a_bg0 + 2u, 4u).status !=
            Status::InvalidWidth ||
        display.write(
            NDS_ARM9, engine_a_bg0 + 2u, 4u,
            UINT32_C(0x12345678)) != Status::InvalidWidth ||
        display.write(
            NDS_ARM9, engine_a_bg0, 1u, UINT32_C(0xFF)) !=
            Status::InvalidWidth) {
        return fail("invalid display register widths or alignment were accepted");
    }

    if (display.read(NDS_ARM7, engine_a_dispcnt, 4u).status !=
            Status::Unmapped ||
        display.write(
            NDS_ARM7, engine_a_bg0, 2u, UINT32_C(0xFFFF)) !=
            Status::Unmapped ||
        display.read(
            static_cast<NdsCpu>(9), engine_a_dispcnt, 4u).status !=
            Status::InvalidCpu ||
        display.read(NDS_ARM9, UINT32_C(0x04000004), 2u).status !=
            Status::Unmapped ||
        display.read(NDS_ARM9, UINT32_C(0x04001010), 2u).status !=
            Status::Unmapped) {
        return fail("CPU visibility or out-of-scope display register routing failed");
    }

    std::puts("NDS Engine A/B DISPCNT and BG0CNT-BG3CNT register storage passed");
    return 0;
}

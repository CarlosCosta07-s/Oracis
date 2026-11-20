#include "oracis/memory/nds_display_control.hpp"
#include "oracis/memory/nds_interrupt_control.hpp"
#include "oracis/memory/guest_memory.hpp"

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
    using oracis::memory::GuestMemory;
    using oracis::memory::NdsDisplayControl;
    using oracis::memory::NdsInterruptControl;
    using oracis::memory::Status;

    GuestMemory memory;
    NdsInterruptControl interrupts(memory);
    NdsDisplayControl display(interrupts);
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
        display.read(NDS_ARM9, NdsDisplayControl::DispstatAddress, 4u).status !=
            Status::InvalidWidth ||
        display.read(NDS_ARM9, UINT32_C(0x04001010), 2u).status !=
            Status::PermissionDenied ||
        display.read(NDS_ARM9, UINT32_C(0x0400101F), 2u).status !=
            Status::InvalidWidth) {
        return fail("CPU visibility or out-of-scope display register routing failed");
    }


    constexpr uint16_t dispstat_irq_enable = UINT16_C(0x0038);
    constexpr uint16_t line_one_compare = UINT16_C(0x0100);
    if (display.write(
            NDS_ARM9, NdsDisplayControl::DispstatAddress, 2u,
            dispstat_irq_enable | line_one_compare) != Status::Ok ||
        display.write(
            NDS_ARM7, NdsDisplayControl::DispstatAddress, 2u,
            dispstat_irq_enable | line_one_compare) != Status::Ok ||
        display.read(NDS_ARM9, NdsDisplayControl::DispstatAddress, 2u).value !=
            (dispstat_irq_enable | line_one_compare) ||
        display.read(NDS_ARM7, NdsDisplayControl::DispstatAddress, 2u).value !=
            (dispstat_irq_enable | line_one_compare)) {
        return fail("DISPSTAT IRQ enables and 9-bit VCOUNT compare did not persist per CPU");
    }


    /*
     * BGxHOFS/BGxVOFS are write-only 16-bit registers. Only bits 0..8 are
     * implemented; the other bits are ignored on writes.
     */
    constexpr uint32_t engine_a_scroll_base = UINT32_C(0x04000010);
    constexpr uint32_t engine_b_scroll_base = UINT32_C(0x04001010);
    if (display.write(
            NDS_ARM9, engine_a_scroll_base, 2u, UINT16_C(0xFFFF)) != Status::Ok ||
        display.write(
            NDS_ARM9, engine_a_scroll_base + 2u, 2u, UINT16_C(0x1234)) != Status::Ok ||
        display.bg_scroll(0u, 0u).horizontal != UINT16_C(0x01FF) ||
        display.bg_scroll(0u, 0u).vertical != UINT16_C(0x0034) ||
        display.read(NDS_ARM9, engine_a_scroll_base, 2u).status !=
            Status::PermissionDenied ||
        display.read(NDS_ARM9, engine_a_scroll_base + 2u, 2u).status !=
            Status::PermissionDenied) {
        return fail("BG0 horizontal/vertical scroll masking or write-only semantics failed");
    }

    if (display.write(
            NDS_ARM9, engine_b_scroll_base + 8u, 2u,
            UINT16_C(0xABCD)) != Status::Ok ||
        display.write(
            NDS_ARM9, engine_b_scroll_base + 10u, 2u,
            UINT16_C(0xFFFF)) != Status::Ok ||
        display.bg_scroll(1u, 2u).horizontal != UINT16_C(0x01CD) ||
        display.bg_scroll(1u, 2u).vertical != UINT16_C(0x01FF) ||
        display.bg_scroll(0u, 2u).horizontal != 0u ||
        display.write(
            NDS_ARM9, engine_a_scroll_base + 2u, 4u,
            UINT32_C(0x12345678)) != Status::InvalidWidth ||
        display.write(
            NDS_ARM9, engine_a_scroll_base, 1u,
            UINT32_C(0x12)) != Status::InvalidWidth ||
        display.read(
            NDS_ARM9, engine_b_scroll_base + 8u, 2u).status !=
            Status::PermissionDenied ||
        display.write(
            NDS_ARM7, engine_a_scroll_base, 2u,
            UINT16_C(0x1234)) != Status::Unmapped) {
        return fail("BG scroll 16-bit width, engine isolation or CPU permissions failed");
    }


    /*
     * BG2/BG3 affine registers are write-only. PA/PB/PC/PD use signed 8.8
     * coefficients (16-bit); X/Y are signed 19.8 values in 28 valid bits.
     */
    constexpr uint32_t engine_a_affine = UINT32_C(0x04000020);
    constexpr uint32_t engine_b_affine = UINT32_C(0x04001020);
    if (display.write(
            NDS_ARM9, engine_a_affine, 2u, UINT16_C(0xFF00)) != Status::Ok ||
        display.write(
            NDS_ARM9, engine_a_affine + 2u, 2u, UINT16_C(0x0080)) != Status::Ok ||
        display.write(
            NDS_ARM9, engine_a_affine + 4u, 2u, UINT16_C(0xFF80)) != Status::Ok ||
        display.write(
            NDS_ARM9, engine_a_affine + 6u, 2u, UINT16_C(0x0100)) != Status::Ok ||
        display.write(
            NDS_ARM9, engine_a_affine + 8u, 4u,
            UINT32_C(0xF8765432)) != Status::Ok ||
        display.write(
            NDS_ARM9, engine_a_affine + 12u, 4u,
            UINT32_C(0x01234567)) != Status::Ok) {
        return fail("Engine A BG2 affine register writes failed");
    }
    const auto engine_a_bg2_affine = display.bg_affine(0u, 2u);
    if (engine_a_bg2_affine.pa != -256 ||
        engine_a_bg2_affine.pb != 128 ||
        engine_a_bg2_affine.pc != -128 ||
        engine_a_bg2_affine.pd != 256 ||
        engine_a_bg2_affine.reference_x != -126458830 ||
        engine_a_bg2_affine.reference_y != INT32_C(0x01234567) ||
        display.read(NDS_ARM9, engine_a_affine, 2u).status !=
            Status::PermissionDenied ||
        display.read(NDS_ARM9, engine_a_affine + 8u, 4u).status !=
            Status::PermissionDenied) {
        return fail("Engine A BG2 affine signed values or write-only semantics failed");
    }

    if (display.write(
            NDS_ARM9, engine_b_affine + 16u, 2u,
            UINT16_C(0x0200)) != Status::Ok ||
        display.write(
            NDS_ARM9, engine_b_affine + 22u, 2u,
            UINT16_C(0xFE00)) != Status::Ok ||
        display.write(
            NDS_ARM9, engine_b_affine + 24u, 4u,
            UINT32_C(0x08000000)) != Status::Ok ||
        display.write(
            NDS_ARM9, engine_b_affine + 28u, 4u,
            UINT32_C(0xF0000001)) != Status::Ok) {
        return fail("Engine B BG3 affine register writes failed");
    }
    const auto engine_b_bg3_affine = display.bg_affine(1u, 3u);
    if (engine_b_bg3_affine.pa != 512 ||
        engine_b_bg3_affine.pd != -512 ||
        engine_b_bg3_affine.reference_x != -INT32_C(0x08000000) ||
        engine_b_bg3_affine.reference_y != 1 ||
        display.bg_affine(0u, 3u).pa != 0 ||
        display.bg_affine(1u, 2u).pa != 0 ||
        display.write(
            NDS_ARM9, engine_a_affine, 4u, UINT32_C(0x12345678)) !=
            Status::InvalidWidth ||
        display.write(
            NDS_ARM9, engine_a_affine + 8u, 2u, UINT16_C(0x1234)) !=
            Status::InvalidWidth ||
        display.write(
            NDS_ARM9, engine_a_affine + 10u, 4u,
            UINT32_C(0x12345678)) != Status::InvalidWidth ||
        display.read(
            NDS_ARM9, engine_b_affine + 24u, 4u).status !=
            Status::PermissionDenied ||
        display.write(
            NDS_ARM7, engine_a_affine, 2u, UINT16_C(0x1234)) !=
            Status::Unmapped) {
        return fail("Engine B BG3 affine masking, width, isolation or CPU permission failed");
    }

    /* The NDS9 and NDS7 HBlank flags have distinct phase lengths. */
    display.advance_bus_cycles(NdsDisplayControl::Arm9HblankStartCycle - 1u);
    if ((display.read(
            NDS_ARM9, NdsDisplayControl::DispstatAddress, 2u).value & 2u) != 0u ||
        (display.read(
            NDS_ARM7, NdsDisplayControl::DispstatAddress, 2u).value & 2u) != 0u ||
        interrupts.pending_enabled(NDS_ARM9) != 0u ||
        interrupts.pending_enabled(NDS_ARM7) != 0u) {
        return fail("HBlank asserted before its hardware timing threshold");
    }
    display.advance_bus_cycles(1u);
    if ((display.read(
            NDS_ARM9, NdsDisplayControl::DispstatAddress, 2u).value & 2u) == 0u ||
        (display.read(
            NDS_ARM7, NdsDisplayControl::DispstatAddress, 2u).value & 2u) != 0u ||
        interrupts.read(NDS_ARM9, NdsInterruptControl::IfAddress, 4u).value !=
            (UINT32_C(1) << 1u) ||
        interrupts.read(NDS_ARM7, NdsInterruptControl::IfAddress, 4u).value != 0u) {
        return fail("ARM9 HBlank phase or IRQ threshold was incorrect");
    }
    display.advance_bus_cycles(
        NdsDisplayControl::Arm7HblankStartCycle -
        NdsDisplayControl::Arm9HblankStartCycle);
    if ((display.read(
            NDS_ARM7, NdsDisplayControl::DispstatAddress, 2u).value & 2u) == 0u ||
        interrupts.read(NDS_ARM7, NdsInterruptControl::IfAddress, 4u).value !=
            (UINT32_C(1) << 1u)) {
        return fail("ARM7 HBlank phase or IRQ threshold was incorrect");
    }

    display.advance_bus_cycles(
        NdsDisplayControl::CyclesPerScanline -
        NdsDisplayControl::Arm7HblankStartCycle);
    if (display.current_scanline() != 1u ||
        (display.read(
            NDS_ARM9, NdsDisplayControl::DispstatAddress, 2u).value & 4u) == 0u ||
        (display.read(
            NDS_ARM7, NdsDisplayControl::DispstatAddress, 2u).value & 4u) == 0u ||
        interrupts.read(NDS_ARM9, NdsInterruptControl::IfAddress, 4u).value !=
            UINT32_C(6) ||
        interrupts.read(NDS_ARM7, NdsInterruptControl::IfAddress, 4u).value !=
            UINT32_C(6)) {
        return fail("VCOUNT line transition or compare IRQ was not raised");
    }

    /* VBlank begins on line 192 and is absent from the final line 262. */
    display.advance_bus_cycles(
        static_cast<uint64_t>(NdsDisplayControl::CyclesPerScanline) * 191u);
    if (display.current_scanline() != 192u ||
        (display.read(
            NDS_ARM9, NdsDisplayControl::DispstatAddress, 2u).value & 1u) == 0u ||
        (display.read(
            NDS_ARM7, NdsDisplayControl::DispstatAddress, 2u).value & 1u) == 0u ||
        (interrupts.read(
            NDS_ARM9, NdsInterruptControl::IfAddress, 4u).value & 1u) == 0u ||
        (interrupts.read(
            NDS_ARM7, NdsInterruptControl::IfAddress, 4u).value & 1u) == 0u) {
        return fail("VBlank status/IRQ did not begin on scanline 192");
    }
    display.advance_bus_cycles(
        static_cast<uint64_t>(NdsDisplayControl::CyclesPerScanline) * 70u);
    if (display.current_scanline() != 262u ||
        (display.read(
            NDS_ARM9, NdsDisplayControl::DispstatAddress, 2u).value & 1u) != 0u ||
        (display.read(
            NDS_ARM7, NdsDisplayControl::DispstatAddress, 2u).value & 1u) != 0u) {
        return fail("VBlank flag remained set on final scanline 262");
    }

    /* VCOUNT synchronization writes are queued and applied one line later. */
    display.advance_bus_cycles(NdsDisplayControl::CyclesPerScanline);
    display.advance_bus_cycles(
        static_cast<uint64_t>(NdsDisplayControl::CyclesPerScanline) * 202u);
    if (display.current_scanline() != 202u ||
        display.write(
            NDS_ARM9, NdsDisplayControl::VcountAddress, 2u,
            UINT16_C(210)) != Status::Ok ||
        display.read(NDS_ARM9, NdsDisplayControl::VcountAddress, 2u).value !=
            202u) {
        return fail("VCOUNT synchronization write was not queued in its safe window");
    }
    display.advance_bus_cycles(NdsDisplayControl::CyclesPerScanline);
    if (display.current_scanline() != 210u ||
        display.read(NDS_ARM7, NdsDisplayControl::VcountAddress, 2u).value !=
            210u) {
        return fail("queued VCOUNT write was not applied at the next scanline");
    }
    display.advance_bus_cycles(
        static_cast<uint64_t>(NdsDisplayControl::CyclesPerScanline) * 3u);
    if (display.current_scanline() != 213u ||
        interrupts.write(
            NDS_ARM9, NdsInterruptControl::IfAddress, 4u, UINT32_C(1)) != Status::Ok ||
        interrupts.write(
            NDS_ARM7, NdsInterruptControl::IfAddress, 4u, UINT32_C(1)) != Status::Ok ||
        display.write(
            NDS_ARM7, NdsDisplayControl::VcountAddress, 2u,
            UINT16_C(200)) != Status::Ok ||
        display.current_scanline() != 213u) {
        return fail("VCOUNT write outside the recommended sync window was rejected or applied mid-line");
    }
    display.advance_bus_cycles(NdsDisplayControl::CyclesPerScanline);
    if (display.current_scanline() != 200u ||
        (display.read(
            NDS_ARM9, NdsDisplayControl::DispstatAddress, 2u).value & 1u) == 0u ||
        (interrupts.read(
            NDS_ARM9, NdsInterruptControl::IfAddress, 4u).value & 1u) != 0u ||
        (interrupts.read(
            NDS_ARM7, NdsInterruptControl::IfAddress, 4u).value & 1u) != 0u) {
        return fail("VCOUNT jump into VBlank did not update status without fabricating a line-192 IRQ");
    }
    if (display.write(
            NDS_ARM9, NdsDisplayControl::VcountAddress, 2u,
            UINT16_C(300)) != Status::PermissionDenied) {
        return fail("VCOUNT accepted a scanline outside the modeled 0..262 range");
    }

    std::puts("NDS Engine A/B registers, DISPSTAT/VCOUNT scanline timing and IRQs passed");
    return 0;
}

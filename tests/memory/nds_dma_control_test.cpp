#include "oracis/memory/guest_memory_abi.h"
#include "oracis/memory/nds_dma_control.hpp"
#include "oracis/memory/nds_display_control.hpp"
#include "oracis/memory/nds_interrupt_control.hpp"
#include "oracis/memory/nds_external_memory_control.hpp"
#include "oracis/recomp/runtime_arm.h"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_dma_control_test: %s\n", message);
    return 1;
}

constexpr uint32_t Dma0Irq = UINT32_C(1) << 8u;
constexpr uint32_t Dma1Irq = UINT32_C(1) << 9u;

uint32_t source_reg(unsigned channel)
{
    return oracis::memory::NdsDmaControl::FirstRegisterAddress +
        static_cast<uint32_t>(channel) * 12u;
}

uint32_t destination_reg(unsigned channel)
{
    return source_reg(channel) + 4u;
}

uint32_t control_reg(unsigned channel)
{
    return source_reg(channel) + 8u;
}

} // namespace

int main()
{
    using oracis::memory::GuestMemory;
    using oracis::memory::NdsDmaControl;
    using oracis::memory::NdsDisplayControl;
    using oracis::memory::NdsInterruptControl;
    using oracis::memory::NdsExternalMemoryControl;
    using oracis::memory::Status;

    GuestMemory memory;
    if (memory.map_ram(
            UINT32_C(0x02000000), UINT32_C(0x00020000),
            oracis::memory::Readable | oracis::memory::Writable,
            "dma-test-ram") != Status::Ok) {
        return fail("could not map DMA test RAM");
    }
    NdsInterruptControl interrupts(memory);
    NdsExternalMemoryControl external_memory;
    NdsDmaControl dma(memory, interrupts);
    dma.attach_external_memory_control(external_memory);
    oracis::memory::NdsDisplayControl display(interrupts, &dma);
    if (display.install() != Status::Ok) {
        return fail("display event source installation failed");
    }
    dma.attach_display_control(display);

    ArmCpuState arm9{};
    ArmCpuState arm7{};
    arm9.cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm7.cpsr = ORACIS_ARM_MODE_SYSTEM;
    if (!oracis_arm_runtime_bind_cpus(&arm9, &arm7) ||
        !oracis_bus_bind_memory(&memory) ||
        !oracis_bus_bind_external_memory_control(&external_memory) ||
        !oracis_bus_bind_interrupt_control(&interrupts) ||
        !oracis_bus_bind_dma_control(&dma)) {
        return fail("DMA bus binding failed");
    }
    if (oracis_bus_bind_dma_control(&dma) ||
        oracis_bus_unbind_interrupt_control(&interrupts) ||
        oracis_bus_unbind_memory(&memory)) {
        return fail("duplicate DMA binding or premature dependency teardown succeeded");
    }

    constexpr uint32_t arm9_source = UINT32_C(0x02000000);
    constexpr uint32_t arm9_destination = UINT32_C(0x02000100);
    const uint32_t words[] = {
        UINT32_C(0x11223344), UINT32_C(0xAABBCCDD),
        UINT32_C(0x0BADF00D), UINT32_C(0x55AA55AA)
    };
    for (unsigned index = 0u; index < 4u; ++index) {
        if (memory.write(NDS_ARM9, arm9_source + index * 4u,
                4u, words[index]) != Status::Ok) {
            return fail("could not seed ARM9 DMA source words");
        }
    }
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 selection failed");
    }
    bus_write_u32(NdsInterruptControl::IeAddress, Dma0Irq);
    bus_write_u32(source_reg(0u), arm9_source);
    bus_write_u32(destination_reg(0u), arm9_destination);
    bus_write_u32(control_reg(0u),
        UINT32_C(4) | NdsDmaControl::WordTransferMask |
        NdsDmaControl::IrqEnableMask | NdsDmaControl::EnableMask);

    for (unsigned index = 0u; index < 4u; ++index) {
        if (memory.read(NDS_ARM9, arm9_destination + index * 4u, 4u).value !=
            words[index]) {
            return fail("ARM9 immediate 32-bit transfer copied incorrect data");
        }
    }
    if (bus_read_u32(source_reg(0u)) != arm9_source ||
        bus_read_u32(destination_reg(0u)) != arm9_destination ||
        (bus_read_u32(control_reg(0u)) & NdsDmaControl::EnableMask) != 0u ||
        bus_read_u32(NdsInterruptControl::IfAddress) != Dma0Irq) {
        return fail("ARM9 DMA completion state, addresses, or IRQ was incorrect");
    }

    /*
     * Platinum probes the empty GBA Slot-2 ROM through DMA1. The first
     * halfword uses the 10-cycle nonsequential pattern (Addr/2 | FE08h);
     * subsequent halfwords use sequential Addr/2 values.
     */
    constexpr uint32_t gba_slot_source = UINT32_C(0x08000080);
    constexpr uint32_t gba_slot_destination = UINT32_C(0x02000400);
    if (external_memory.write(NDS_ARM9, NdsExternalMemoryControl::Address,
            2u, 0u) != Status::Ok) {
        return fail("could not seed default EXMEMCNT Slot-2 ownership/timing");
    }
    bus_write_u32(source_reg(1u), gba_slot_source);
    bus_write_u32(destination_reg(1u), gba_slot_destination);
    bus_write_u32(control_reg(1u),
        UINT32_C(2) | NdsDmaControl::WordTransferMask |
        NdsDmaControl::EnableMask);
    if (memory.read(NDS_ARM9, gba_slot_destination, 4u).value !=
            UINT32_C(0x0041FE48) ||
        memory.read(NDS_ARM9, gba_slot_destination + 4u, 4u).value !=
            UINT32_C(0x00430042) ||
        (bus_read_u32(control_reg(1u)) & NdsDmaControl::EnableMask) != 0u ||
        memory.read(NDS_ARM9, gba_slot_source, 4u).status != Status::Unmapped) {
        return fail("DMA1 did not transfer the empty Slot-2 open-bus sequence");
    }

    /* Exact observed Platinum transfer: DMA1, 32 halfwords, control 80000020h. */
    constexpr uint32_t gba_probe_destination = UINT32_C(0x02000600);
    bus_write_u32(source_reg(1u), gba_slot_source);
    bus_write_u32(destination_reg(1u), gba_probe_destination);
    bus_write_u32(control_reg(1u), UINT32_C(0x80000020));
    for (uint32_t index = 0u; index < 32u; ++index) {
        const uint32_t expected = index == 0u
            ? UINT32_C(0xFE48)
            : UINT32_C(0x0040) + index;
        if (memory.read(NDS_ARM9, gba_probe_destination + index * 2u,
                2u).value != expected) {
            return fail("DMA1 32-halfword Slot-2 probe sequence was incorrect");
        }
    }
    if ((bus_read_u32(control_reg(1u)) & NdsDmaControl::EnableMask) != 0u) {
        return fail("DMA1 32-halfword Slot-2 probe did not complete");
    }

    /* ARM7 has its own channel bank and halfword transfers by default. */
    constexpr uint32_t arm7_source = UINT32_C(0x02000200);
    constexpr uint32_t arm7_destination = UINT32_C(0x02000300);
    for (uint32_t index = 0u; index < 3u; ++index) {
        if (memory.write(NDS_ARM7, arm7_source + index * 2u,
                2u, UINT32_C(0xA100) + index) != Status::Ok) {
            return fail("could not seed ARM7 halfword source");
        }
    }
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7)) {
        return fail("ARM7 selection failed");
    }
    bus_write_u32(NdsInterruptControl::IeAddress, Dma1Irq);
    bus_write_u32(source_reg(1u), arm7_source);
    bus_write_u32(destination_reg(1u), arm7_destination);
    bus_write_u16(control_reg(1u), UINT16_C(3));
    bus_write_u16(control_reg(1u) + 2u,
        static_cast<uint16_t>((NdsDmaControl::IrqEnableMask |
            NdsDmaControl::EnableMask) >> 16u));
    for (uint32_t index = 0u; index < 3u; ++index) {
        if (memory.read(NDS_ARM7, arm7_destination + index * 2u, 2u).value !=
            UINT32_C(0xA100) + index) {
            return fail("ARM7 immediate 16-bit transfer copied incorrect data");
        }
    }
    if (bus_read_u32(source_reg(1u)) != arm7_source ||
        bus_read_u32(destination_reg(1u)) != arm7_destination ||
        (bus_read_u32(control_reg(1u)) & NdsDmaControl::EnableMask) != 0u ||
        bus_read_u32(NdsInterruptControl::IfAddress) != Dma1Irq) {
        return fail("ARM7 DMA split control write or IRQ completion failed");
    }

    /* Fixed source repeats one unit; address decrement walks from high to low. */
    constexpr uint32_t fill_source = UINT32_C(0x02000400);
    constexpr uint32_t fill_destination = UINT32_C(0x02000500);
    if (memory.write(NDS_ARM9, fill_source, 2u, UINT16_C(0xBEEF)) !=
            Status::Ok || !oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("could not prepare fixed-source DMA test");
    }
    bus_write_u32(source_reg(2u), fill_source);
    bus_write_u32(destination_reg(2u), fill_destination);
    bus_write_u32(control_reg(2u),
        UINT32_C(3) |
        (UINT32_C(2) << NdsDmaControl::SourceControlShift) |
        NdsDmaControl::EnableMask);
    for (uint32_t index = 0u; index < 3u; ++index) {
        if (memory.read(NDS_ARM9, fill_destination + index * 2u, 2u).value !=
            UINT32_C(0xBEEF)) {
            return fail("fixed-source DMA did not repeat the source unit");
        }
    }

    constexpr uint32_t descending_source = UINT32_C(0x0200060C);
    constexpr uint32_t descending_destination = UINT32_C(0x0200070C);
    const uint32_t descending_words[] = {
        UINT32_C(0x01020304), UINT32_C(0x05060708),
        UINT32_C(0x090A0B0C), UINT32_C(0x0D0E0F10)
    };
    for (uint32_t index = 0u; index < 4u; ++index) {
        if (memory.write(NDS_ARM9,
                descending_source - index * 4u, 4u,
                descending_words[3u - index]) != Status::Ok) {
            return fail("could not seed decrementing DMA source");
        }
    }
    bus_write_u32(source_reg(2u), descending_source);
    bus_write_u32(destination_reg(2u), descending_destination);
    bus_write_u32(control_reg(2u),
        UINT32_C(4) | NdsDmaControl::WordTransferMask |
        (UINT32_C(1) << NdsDmaControl::SourceControlShift) |
        (UINT32_C(1) << NdsDmaControl::DestinationControlShift) |
        NdsDmaControl::EnableMask);
    if (memory.read(NDS_ARM9, descending_destination, 4u).value !=
            descending_words[3] ||
        memory.read(NDS_ARM9, descending_destination - 4u, 4u).value !=
            descending_words[2] ||
        bus_read_u32(source_reg(2u)) != descending_source ||
        bus_read_u32(destination_reg(2u)) != descending_destination) {
        return fail("decrementing source/destination transfer failed");
    }

    /* ARM9 DMAxFILL is a fixed source port for 16-bit and 32-bit fills. */
    constexpr uint32_t arm9_fill0 = NdsDmaControl::FirstFillAddress;
    constexpr uint32_t fill_destination32 = UINT32_C(0x02000A80);
    constexpr uint32_t fill_destination16 = UINT32_C(0x02000AA0);
    const uint32_t fill_word32 = UINT32_C(0x89ABCDEF);
    const uint32_t fill_word16 = UINT32_C(0x1234ABCD);
    bus_write_u32(arm9_fill0, fill_word32);
    bus_write_u32(source_reg(0u), arm9_fill0);
    bus_write_u32(destination_reg(0u), fill_destination32);
    bus_write_u32(control_reg(0u),
        UINT32_C(2) | NdsDmaControl::WordTransferMask |
        (UINT32_C(2) << NdsDmaControl::SourceControlShift) |
        NdsDmaControl::EnableMask);
    if (bus_read_u32(arm9_fill0) != fill_word32 ||
        memory.read(NDS_ARM9, fill_destination32, 4u).value != fill_word32 ||
        memory.read(NDS_ARM9, fill_destination32 + 4u, 4u).value != fill_word32 ||
        bus_read_u32(source_reg(0u)) != arm9_fill0 ||
        bus_read_u32(destination_reg(0u)) != fill_destination32) {
        return fail("ARM9 32-bit DMAxFILL transfer or programmed registers failed");
    }

    const uint32_t arm9_fill1 = NdsDmaControl::FirstFillAddress + 4u;
    bus_write_u32(arm9_fill1, fill_word16);
    bus_write_u32(source_reg(1u), arm9_fill1);
    bus_write_u32(destination_reg(1u), fill_destination16);
    bus_write_u32(control_reg(1u),
        UINT32_C(3) |
        (UINT32_C(2) << NdsDmaControl::SourceControlShift) |
        NdsDmaControl::EnableMask);
    for (uint32_t index = 0u; index < 3u; ++index) {
        if (memory.read(NDS_ARM9, fill_destination16 + index * 2u, 2u).value !=
            UINT32_C(0xABCD)) {
            return fail("ARM9 16-bit DMAxFILL did not repeat the low halfword");
        }
    }
    if (bus_read_u32(arm9_fill1) != fill_word16 ||
        bus_read_u32(source_reg(1u)) != arm9_fill1 ||
        bus_read_u32(destination_reg(1u)) != fill_destination16) {
        return fail("ARM9 DMAxFILL register was not preserved after transfer");
    }

    const uint32_t arm9_fill3 =
        NdsDmaControl::FirstFillAddress + UINT32_C(12);
    bus_write_u32(arm9_fill3, UINT32_C(0x0F1E2D3C));
    if (bus_read_u32(arm9_fill3) != UINT32_C(0x0F1E2D3C) ||
        dma.read(NDS_ARM9, arm9_fill0, 2u).status != Status::InvalidWidth) {
        return fail("DMA3FILL boundary or 32-bit register width validation failed");
    }
    if (dma.read(NDS_ARM7, arm9_fill0, 4u).status != Status::Unmapped ||
        dma.write(NDS_ARM7, arm9_fill0, 4u, UINT32_C(0)) != Status::Unmapped) {
        return fail("ARM7 incorrectly exposed the ARM9-only DMAxFILL registers");
    }

    /* VBlank timing arms the channel without firing before a display event. */
    constexpr uint32_t deferred_source = UINT32_C(0x02000800);
    constexpr uint32_t deferred_destination = UINT32_C(0x02000900);
    if (memory.write(NDS_ARM9, deferred_source, 4u, UINT32_C(0xCAFE1234)) !=
        Status::Ok) {
        return fail("could not seed deferred DMA source");
    }
    bus_write_u32(source_reg(3u), deferred_source);
    bus_write_u32(destination_reg(3u), deferred_destination);
    bus_write_u32(control_reg(3u),
        UINT32_C(1) | (UINT32_C(1) << 27u) |
        NdsDmaControl::WordTransferMask | NdsDmaControl::IrqEnableMask |
        NdsDmaControl::EnableMask);
    if (memory.read(NDS_ARM9, deferred_destination, 4u).value != 0u ||
        (bus_read_u32(control_reg(3u)) & NdsDmaControl::EnableMask) == 0u) {
        return fail("VBlank DMA did not stay armed without an event");
    }
    bus_write_u32(NdsInterruptControl::IeAddress, UINT32_C(1) << 11u);
    display.advance_bus_cycles(
        static_cast<uint64_t>(NdsDisplayControl::VisibleScanlines) *
        NdsDisplayControl::CyclesPerScanline);
    if (memory.read(NDS_ARM9, deferred_destination, 4u).value !=
            UINT32_C(0xCAFE1234) ||
        (bus_read_u32(control_reg(3u)) & NdsDmaControl::EnableMask) != 0u ||
        bus_read_u32(source_reg(3u)) != deferred_source ||
        bus_read_u32(destination_reg(3u)) != deferred_destination ||
        (bus_read_u32(NdsInterruptControl::IfAddress) &
            (UINT32_C(1) << 11u)) == 0u) {
        return fail("VBlank event did not execute one-shot DMA and request IRQ");
    }

    /* ARM9 HBlank DMA repeats once per visible line and reloads destination. */
    display.reset_timing();
    constexpr uint32_t hblank_source = UINT32_C(0x02000B00);
    constexpr uint32_t hblank_destination = UINT32_C(0x02000C00);
    if (memory.write(NDS_ARM9, hblank_source, 4u,
            UINT32_C(0x13572468)) != Status::Ok ||
        memory.write(NDS_ARM9, hblank_source + 4u, 4u,
            UINT32_C(0x24681357)) != Status::Ok) {
        return fail("could not seed HBlank DMA source");
    }
    bus_write_u32(source_reg(1u), hblank_source);
    bus_write_u32(destination_reg(1u), hblank_destination);
    bus_write_u32(control_reg(1u),
        UINT32_C(1) | (UINT32_C(2) << 27u) |
        NdsDmaControl::RepeatMask | NdsDmaControl::WordTransferMask |
        (UINT32_C(3) << NdsDmaControl::DestinationControlShift) |
        NdsDmaControl::EnableMask);
    display.advance_bus_cycles(NdsDisplayControl::Arm9HblankStartCycle);
    if (memory.read(NDS_ARM9, hblank_destination, 4u).value !=
            UINT32_C(0x13572468) ||
        (bus_read_u32(control_reg(1u)) & NdsDmaControl::EnableMask) == 0u) {
        return fail("first HBlank DMA repeat did not execute");
    }
    display.advance_bus_cycles(NdsDisplayControl::CyclesPerScanline);
    if (memory.read(NDS_ARM9, hblank_destination, 4u).value !=
            UINT32_C(0x24681357) ||
        bus_read_u32(source_reg(1u)) != hblank_source ||
        bus_read_u32(destination_reg(1u)) != hblank_destination ||
        (bus_read_u32(control_reg(1u)) & NdsDmaControl::EnableMask) == 0u) {
        return fail("repeating HBlank DMA did not advance source and reload destination");
    }
    bus_write_u16(control_reg(1u) + 2u, 0u);

    /* A fresh enable edge reloads the internal source/destination from SAD/DAD. */
    const uint32_t hblank_repeat_control =
        UINT32_C(1) | (UINT32_C(2) << 27u) |
        NdsDmaControl::RepeatMask | NdsDmaControl::WordTransferMask |
        (UINT32_C(3) << NdsDmaControl::DestinationControlShift) |
        NdsDmaControl::EnableMask;
    bus_write_u32(control_reg(1u), hblank_repeat_control);
    /* The current line has passed HBlank; wait until the next line's event. */
    display.advance_bus_cycles(NdsDisplayControl::CyclesPerScanline);
    if (memory.read(NDS_ARM9, hblank_destination, 4u).value !=
            UINT32_C(0x13572468) ||
        bus_read_u32(source_reg(1u)) != hblank_source ||
        bus_read_u32(destination_reg(1u)) != hblank_destination) {
        return fail("re-enabling DMA did not reload the programmed SAD/DAD pointers");
    }
    bus_write_u16(control_reg(1u) + 2u, 0u);

    /* Arm an HBlank transfer only after entering VBlank; it must remain pending. */
    display.reset_timing();
    display.advance_bus_cycles(
        static_cast<uint64_t>(NdsDisplayControl::VisibleScanlines) *
        NdsDisplayControl::CyclesPerScanline);
    constexpr uint32_t paused_destination = UINT32_C(0x02000D00);
    if (memory.write(NDS_ARM9, paused_destination, 4u,
            UINT32_C(0xDEADBEEF)) != Status::Ok) {
        return fail("could not seed paused HBlank destination");
    }
    bus_write_u32(source_reg(2u), hblank_source);
    bus_write_u32(destination_reg(2u), paused_destination);
    bus_write_u32(control_reg(2u),
        UINT32_C(1) | (UINT32_C(2) << 27u) |
        NdsDmaControl::WordTransferMask | NdsDmaControl::EnableMask);
    display.advance_bus_cycles(NdsDisplayControl::Arm9HblankStartCycle);
    if (memory.read(NDS_ARM9, paused_destination, 4u).value !=
            UINT32_C(0xDEADBEEF) ||
        (bus_read_u32(control_reg(2u)) & NdsDmaControl::EnableMask) == 0u) {
        return fail("HBlank DMA incorrectly fired during VBlank");
    }

    /* Clear the paused HBlank channel before re-arming it for display-sync. */
    bus_write_u16(control_reg(2u) + 2u, 0u);

    /* ARM9 display-sync mode transfers once at each visible line start. */
    display.reset_timing();
    constexpr uint32_t display_sync_source = UINT32_C(0x02000E00);
    constexpr uint32_t display_sync_destination = UINT32_C(0x02000F00);
    const uint32_t display_sync_words[] = {
        UINT32_C(0xA1B2C3D4), UINT32_C(0x10293847),
        UINT32_C(0x55667788)
    };
    for (uint32_t index = 0u; index < 3u; ++index) {
        if (memory.write(NDS_ARM9, display_sync_source + index * 4u,
                4u, display_sync_words[index]) != Status::Ok) {
            return fail("could not seed display-sync DMA source");
        }
    }
    bus_write_u32(source_reg(2u), display_sync_source);
    bus_write_u32(destination_reg(2u), display_sync_destination);
    bus_write_u32(control_reg(2u),
        UINT32_C(1) | (UINT32_C(3) << 27u) |
        NdsDmaControl::RepeatMask | NdsDmaControl::WordTransferMask |
        NdsDmaControl::EnableMask);
    display.advance_bus_cycles(NdsDisplayControl::CyclesPerScanline);
    if (memory.read(NDS_ARM9, display_sync_destination, 4u).value !=
            display_sync_words[0] ||
        (bus_read_u32(control_reg(2u)) & NdsDmaControl::EnableMask) == 0u ||
        bus_read_u32(source_reg(2u)) != display_sync_source ||
        bus_read_u32(destination_reg(2u)) != display_sync_destination) {
        return fail("first visible-line display-sync DMA event failed");
    }
    display.advance_bus_cycles(NdsDisplayControl::CyclesPerScanline);
    if (memory.read(NDS_ARM9, display_sync_destination + 4u, 4u).value !=
            display_sync_words[1] ||
        (bus_read_u32(control_reg(2u)) & NdsDmaControl::EnableMask) == 0u) {
        return fail("display-sync DMA did not repeat at the next line start");
    }
    bus_write_u16(control_reg(2u) + 2u, 0u);

    /* A non-repeating display-sync channel disarms after its first line. */
    display.reset_timing();
    bus_write_u32(source_reg(2u), display_sync_source);
    bus_write_u32(destination_reg(2u), display_sync_destination + UINT32_C(0x20));
    bus_write_u32(control_reg(2u),
        UINT32_C(1) | (UINT32_C(3) << 27u) |
        NdsDmaControl::WordTransferMask | NdsDmaControl::EnableMask);
    display.advance_bus_cycles(NdsDisplayControl::CyclesPerScanline);
    if (memory.read(NDS_ARM9,
            display_sync_destination + UINT32_C(0x20), 4u).value !=
            display_sync_words[0] ||
        (bus_read_u32(control_reg(2u)) & NdsDmaControl::EnableMask) != 0u) {
        return fail("one-shot display-sync DMA did not disarm after one line");
    }
    display.advance_bus_cycles(NdsDisplayControl::CyclesPerScanline);
    if (memory.read(NDS_ARM9,
            display_sync_destination + UINT32_C(0x24), 4u).value != 0u) {
        return fail("one-shot display-sync DMA fired more than once");
    }

    /* Display-sync DMA pauses throughout VBlank and stays armed. */
    display.reset_timing();
    display.advance_bus_cycles(
        static_cast<uint64_t>(NdsDisplayControl::VisibleScanlines) *
        NdsDisplayControl::CyclesPerScanline);
    constexpr uint32_t display_sync_vblank_destination =
        UINT32_C(0x02000F80);
    bus_write_u32(source_reg(2u), display_sync_source);
    bus_write_u32(destination_reg(2u), display_sync_vblank_destination);
    bus_write_u32(control_reg(2u),
        UINT32_C(1) | (UINT32_C(3) << 27u) |
        NdsDmaControl::RepeatMask | NdsDmaControl::WordTransferMask |
        NdsDmaControl::EnableMask);
    display.advance_bus_cycles(NdsDisplayControl::CyclesPerScanline);
    if (memory.read(NDS_ARM9, display_sync_vblank_destination, 4u).value != 0u ||
        (bus_read_u32(control_reg(2u)) & NdsDmaControl::EnableMask) == 0u) {
        return fail("display-sync DMA fired during VBlank or disarmed prematurely");
    }
    bus_write_u16(control_reg(2u) + 2u, 0u);

    /* ARM9 main-memory display DMA supplies 4-word FIFO bursts on demand. */
    display.reset_timing();
    constexpr uint32_t mmem_source = UINT32_C(0x02001000);
    constexpr uint32_t mmem_words[] = {
        UINT32_C(0x7FFF0001), UINT32_C(0x7FFE0002),
        UINT32_C(0x7FFD0003), UINT32_C(0x7FFC0004),
        UINT32_C(0x7FFB0005), UINT32_C(0x7FFA0006),
        UINT32_C(0x7FF90007), UINT32_C(0x7FF80008)
    };
    for (uint32_t index = 0u; index < 8u; ++index) {
        if (memory.write(NDS_ARM9, mmem_source + index * 4u,
                4u, mmem_words[index]) != Status::Ok) {
            return fail("could not seed main-memory display DMA source");
        }
    }
    if (display.write(NDS_ARM9, NdsDisplayControl::EngineABase,
            4u, UINT32_C(3) << 16u) != Status::Ok) {
        return fail("could not select Engine A main-memory display mode");
    }
    (void)interrupts.write(
        NDS_ARM9, NdsInterruptControl::IfAddress, 4u, Dma0Irq);
    (void)interrupts.write(
        NDS_ARM9, NdsInterruptControl::IeAddress, 4u, Dma0Irq);
    bus_write_u32(source_reg(0u), mmem_source);
    bus_write_u32(destination_reg(0u),
        NdsDisplayControl::MainMemoryDisplayFifoAddress);
    bus_write_u32(control_reg(0u),
        UINT32_C(4) |
        (UINT32_C(2) << NdsDmaControl::DestinationControlShift) |
        (UINT32_C(4) << 27u) |
        NdsDmaControl::WordTransferMask | NdsDmaControl::IrqEnableMask |
        NdsDmaControl::EnableMask);
    if (display.main_memory_display_fifo_words() != 0u ||
        (bus_read_u32(control_reg(0u)) & NdsDmaControl::EnableMask) == 0u) {
        return fail("main-memory display DMA did not arm without eager transfer");
    }
    constexpr uint16_t expected_mmem_pixels[] = {
        UINT16_C(0x0001), UINT16_C(0x7FFF),
        UINT16_C(0x0002), UINT16_C(0x7FFE),
        UINT16_C(0x0003), UINT16_C(0x7FFD),
        UINT16_C(0x0004), UINT16_C(0x7FFC)
    };
    if (display.main_memory_display_fifo_words() != 0u ||
        display.current_scanline() != 0u) {
        return fail("main-memory display DMA transferred before the first LCD pixel");
    }
    display.advance_bus_cycles(
        8u * NdsDisplayControl::MainMemoryDisplayCyclesPerPixel);
    uint16_t mmd_scanline[NdsDisplayControl::MainMemoryDisplayWidth]{};
    if (display.copy_main_memory_display_scanline(
            0u, mmd_scanline, NdsDisplayControl::MainMemoryDisplayWidth) !=
        Status::Ok) {
        return fail("main-memory display framebuffer scanline was unavailable");
    }
    for (uint32_t pixel = 0u; pixel < 8u; ++pixel) {
        if (mmd_scanline[pixel] != expected_mmem_pixels[pixel]) {
            return fail("clocked main-memory display pixels were out of RGB555 order");
        }
    }
    if (mmd_scanline[8] != UINT16_C(5) ||
        display.main_memory_display_fifo_words() != 4u ||
        bus_read_u32(source_reg(0u)) != mmem_source ||
        bus_read_u32(destination_reg(0u)) !=
            NdsDisplayControl::MainMemoryDisplayFifoAddress) {
        return fail("main-memory display DMA did not refill the next burst on demand");
    }
    display.advance_bus_cycles(
        static_cast<uint64_t>(NdsDisplayControl::VisibleScanlines) *
            NdsDisplayControl::CyclesPerScanline -
        8u * NdsDisplayControl::MainMemoryDisplayCyclesPerPixel);
    if ((bus_read_u32(control_reg(0u)) & NdsDmaControl::EnableMask) != 0u ||
        display.main_memory_display_fifo_words() != 0u ||
        (interrupts.read(NDS_ARM9, NdsInterruptControl::IfAddress, 4u).value &
            Dma0Irq) == 0u) {
        return fail("main-memory display DMA did not stop and IRQ at VBlank");
    }
    (void)display.write(NDS_ARM9, NdsDisplayControl::EngineABase, 4u, 0u);

    /* Invalid source memory is rejected before changing the destination. */
    const uint32_t guard_address = UINT32_C(0x02000A00);
    const uint32_t guard_before = UINT32_C(0xFEEDC0DE);
    if (memory.write(NDS_ARM9, guard_address, 4u, guard_before) != Status::Ok) {
        return fail("could not seed DMA destination guard");
    }
    bus_write_u32(source_reg(0u), UINT32_C(0x04000000));
    bus_write_u32(destination_reg(0u), guard_address);
    const Status rejected = dma.write(
        NDS_ARM9, control_reg(0u), 4u,
        UINT32_C(1) | NdsDmaControl::WordTransferMask |
        NdsDmaControl::EnableMask);
    if (rejected == Status::Ok ||
        memory.read(NDS_ARM9, guard_address, 4u).value != guard_before) {
        return fail("invalid immediate DMA was not rejected transactionally");
    }

    if (!oracis_bus_unbind_dma_control(&dma) ||
        !oracis_bus_unbind_interrupt_control(&interrupts) ||
        !oracis_bus_unbind_external_memory_control(&external_memory) ||
        !oracis_bus_unbind_memory(&memory) ||
        !oracis_arm_runtime_unbind_cpus(&arm9, &arm7)) {
        return fail("DMA bus teardown failed");
    }

    std::puts("NDS DMA memory transfers, display events, main-memory FIFO bursts and IRQs passed");
    return 0;
}
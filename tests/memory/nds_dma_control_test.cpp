#include "oracis/memory/guest_memory_abi.h"
#include "oracis/memory/nds_dma_control.hpp"
#include "oracis/memory/nds_display_control.hpp"
#include "oracis/memory/nds_interrupt_control.hpp"
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
    using oracis::memory::Status;

    GuestMemory memory;
    if (memory.map_ram(
            UINT32_C(0x02000000), UINT32_C(0x00004000),
            oracis::memory::Readable | oracis::memory::Writable,
            "dma-test-ram") != Status::Ok) {
        return fail("could not map DMA test RAM");
    }
    NdsInterruptControl interrupts(memory);
    NdsDmaControl dma(memory, interrupts);
    oracis::memory::NdsDisplayControl display(interrupts, &dma);
    if (display.install() != Status::Ok) {
        return fail("display event source installation failed");
    }

    ArmCpuState arm9{};
    ArmCpuState arm7{};
    arm9.cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm7.cpsr = ORACIS_ARM_MODE_SYSTEM;
    if (!oracis_arm_runtime_bind_cpus(&arm9, &arm7) ||
        !oracis_bus_bind_memory(&memory) ||
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
        !oracis_bus_unbind_memory(&memory) ||
        !oracis_arm_runtime_unbind_cpus(&arm9, &arm7)) {
        return fail("DMA bus teardown failed");
    }

    std::puts("NDS DMA programmed registers, internal pointers, event repeats and IRQ passed");
    return 0;
}
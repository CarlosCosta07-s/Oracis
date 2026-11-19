#include "oracis/memory/guest_memory_abi.h"
#include "oracis/memory/nds_interrupt_control.hpp"
#include "oracis/recomp/runtime_arm.h"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_interrupt_control_test: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    using oracis::memory::GuestMemory;
    using oracis::memory::NdsInterruptControl;
    using oracis::memory::Status;

    GuestMemory memory;
    NdsInterruptControl interrupts(memory);

    constexpr uint32_t timer0 = UINT32_C(1) << 3u;
    constexpr uint32_t timer1 = UINT32_C(1) << 4u;
    constexpr uint32_t dma0 = UINT32_C(1) << 8u;
    constexpr uint32_t dma1 = UINT32_C(1) << 9u;
    constexpr uint32_t keypad = UINT32_C(1) << 12u;

    if (interrupts.read(NDS_ARM9, NdsInterruptControl::ImeAddress, 4u).value != 0u ||
        interrupts.read(NDS_ARM7, NdsInterruptControl::ImeAddress, 4u).value != 0u) {
        return fail("IME did not initialize disabled on both CPUs");
    }

    /* IE enables sources but does not itself raise an interrupt. */
    if (interrupts.write(
            NDS_ARM9, NdsInterruptControl::IeAddress, 4u,
            timer0 | timer1) != Status::Ok ||
        !interrupts.request_irq(NDS_ARM9, timer0 | keypad) ||
        interrupts.irq_asserted(NDS_ARM9) ||
        interrupts.pending_enabled(NDS_ARM9) != timer0) {
        return fail("ARM9 IE/IF gating failed while IME was disabled");
    }

    /* IME implements bit zero only; reserved bits read back as zero. */
    if (interrupts.write(
            NDS_ARM9, NdsInterruptControl::ImeAddress, 4u,
            UINT32_MAX) != Status::Ok ||
        interrupts.read(
            NDS_ARM9, NdsInterruptControl::ImeAddress, 4u).value != 1u ||
        !interrupts.irq_asserted(NDS_ARM9)) {
        return fail("ARM9 IME enable/masking semantics failed");
    }

    /* IF is write-one-to-clear: acknowledge one source, retain the other. */
    if (interrupts.write(
            NDS_ARM9, NdsInterruptControl::IfAddress, 4u, timer0) != Status::Ok ||
        interrupts.read(
            NDS_ARM9, NdsInterruptControl::IfAddress, 4u).value != keypad ||
        interrupts.irq_asserted(NDS_ARM9)) {
        return fail("IF write-one-to-clear semantics failed");
    }
    if (!interrupts.request_irq(NDS_ARM9, timer1) ||
        interrupts.pending_enabled(NDS_ARM9) != timer1 ||
        !interrupts.irq_asserted(NDS_ARM9)) {
        return fail("new interrupt request was not latched");
    }
    if (interrupts.write(
            NDS_ARM9, NdsInterruptControl::IfAddress, 4u, 0u) != Status::Ok ||
        !interrupts.irq_asserted(NDS_ARM9)) {
        return fail("writing zero to IF unexpectedly acknowledged a request");
    }
    if (interrupts.write(
            NDS_ARM9, NdsInterruptControl::IfAddress, 4u, UINT32_MAX) != Status::Ok ||
        interrupts.irq_asserted(NDS_ARM9) ||
        interrupts.read(
            NDS_ARM9, NdsInterruptControl::IfAddress, 4u).value != 0u) {
        return fail("IF full acknowledgement failed");
    }

    /* ARM7 has its own IME/IE/IF bank and does not change ARM9 state. */
    if (interrupts.write(
            NDS_ARM7, NdsInterruptControl::IeAddress, 4u, dma0) != Status::Ok ||
        !interrupts.request_irq(NDS_ARM7, dma0) ||
        interrupts.irq_asserted(NDS_ARM7) ||
        interrupts.pending_enabled(NDS_ARM7) != dma0) {
        return fail("ARM7 interrupt state was not independent");
    }
    if (interrupts.write(
            NDS_ARM7, NdsInterruptControl::ImeAddress, 4u, 1u) != Status::Ok ||
        !interrupts.irq_asserted(NDS_ARM7) ||
        interrupts.irq_asserted(NDS_ARM9)) {
        return fail("ARM7 IME did not gate its local pending request");
    }

    if (interrupts.read(
            NDS_ARM9, NdsInterruptControl::IeAddress, 1u).status !=
            Status::InvalidWidth ||
        interrupts.write(
            NDS_ARM9, NdsInterruptControl::IfAddress, 2u, dma0) !=
            Status::InvalidWidth ||
        interrupts.read(
            NDS_ARM9, NdsInterruptControl::IfAddress + 4u, 4u).status !=
            Status::Unmapped ||
        interrupts.read(
            static_cast<NdsCpu>(77), NdsInterruptControl::IeAddress, 4u).status !=
            Status::InvalidCpu ||
        interrupts.request_irq(static_cast<NdsCpu>(77), dma0)) {
        return fail("invalid interrupt MMIO access or CPU was accepted");
    }

    /* Verify both CPU banks through the generated-code C bus ABI. */
    ArmCpuState arm9{};
    ArmCpuState arm7{};
    arm9.cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm7.cpsr = ORACIS_ARM_MODE_SYSTEM;
    if (!oracis_arm_runtime_bind_cpus(&arm9, &arm7) ||
        !oracis_bus_bind_memory(&memory) ||
        !oracis_bus_bind_interrupt_control(&interrupts)) {
        return fail("interrupt controller bus binding failed");
    }
    if (oracis_bus_bind_interrupt_control(&interrupts)) {
        return fail("duplicate interrupt controller bind was accepted");
    }
    if (oracis_bus_unbind_memory(&memory)) {
        return fail("memory unbind was accepted while interrupt control was bound");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9))
        return fail("ARM9 select failed for bus test");
    bus_write_u32(NdsInterruptControl::ImeAddress, 1u);
    bus_write_u32(NdsInterruptControl::IeAddress, dma1);
    if (!interrupts.request_irq(NDS_ARM9, dma1) ||
        bus_read_u32(NdsInterruptControl::IfAddress) != dma1 ||
        !interrupts.irq_asserted(NDS_ARM9)) {
        return fail("ARM9 interrupt bus read/write routing failed");
    }
    bus_write_u32(NdsInterruptControl::IfAddress, dma1);
    if (interrupts.irq_asserted(NDS_ARM9) ||
        bus_read_u32(NdsInterruptControl::IfAddress) != 0u) {
        return fail("ARM9 IF acknowledgement through bus failed");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM7))
        return fail("ARM7 select failed for bus test");
    bus_write_u32(NdsInterruptControl::ImeAddress, 1u);
    bus_write_u32(NdsInterruptControl::IeAddress, dma0);
    if (!interrupts.request_irq(NDS_ARM7, dma0) ||
        bus_read_u32(NdsInterruptControl::IfAddress) != dma0 ||
        !interrupts.irq_asserted(NDS_ARM7)) {
        return fail("ARM7 interrupt bus state was not independent");
    }
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9) ||
        bus_read_u32(NdsInterruptControl::IfAddress) != 0u) {
        return fail("ARM7 IF leaked into ARM9 bus view");
    }

    if (!oracis_bus_unbind_interrupt_control(&interrupts) ||
        !oracis_bus_unbind_memory(&memory)) {
        return fail("interrupt controller bus teardown failed");
    }

    std::puts("NDS ARM9/ARM7 IME, IE, IF and bus routing tests passed");
    return 0;
}

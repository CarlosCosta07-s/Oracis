#include "oracis/machine/nds_machine.hpp"

#include "oracis/memory/nds_interrupt_control.hpp"
#include "oracis/recomp/arm_cpu_state_ops.h"
#include "oracis/recomp/runtime_arm.h"
#include "oracis/recomp/runtime_scheduler.h"

#include <array>
#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_machine_bios_boot_test: %s\n", message);
    return 1;
}

struct ExecutionFixture {
    uint32_t calls[2]{};
};

bool execute_slice(
    void *opaque, NdsCpu cpu, ArmCpuState *state, uint64_t cycle_budget)
{
    auto *fixture = static_cast<ExecutionFixture *>(opaque);
    if (fixture == nullptr || state == nullptr ||
        (cpu != NDS_ARM9 && cpu != NDS_ARM7) ||
        cycle_budget == 0u || cycle_budget > UINT32_MAX) {
        return false;
    }

    ++fixture->calls[static_cast<unsigned>(cpu)];
    runtime_tick(static_cast<uint32_t>(cycle_budget));
    return runtime_should_yield();
}

} // namespace

int main()
{
    using oracis::machine::NdsMachine;
    using oracis::machine::NdsMachineStatus;
    using oracis::memory::NdsBiosControl;
    using oracis::memory::NdsInterruptControl;
    using oracis::memory::SharedWramMode;

    NdsMachine machine(SharedWramMode::Arm9All32K);
    if (machine.initialize() != NdsMachineStatus::Ok) {
        return fail("machine initialization failed");
    }

    if (machine.start_retail_bios_boot() !=
        NdsMachineStatus::RetailBiosBootUnavailable) {
        return fail("retail BIOS boot started without both user BIOS images");
    }

    std::array<uint8_t, NdsBiosControl::Arm9BiosBytes> arm9_bios{};
    std::array<uint8_t, NdsBiosControl::Arm7BiosBytes> arm7_bios{};
    if (machine.bios_control().load_arm9_bios(
            arm9_bios.data(),
            static_cast<uint32_t>(arm9_bios.size())) !=
            oracis::memory::Status::Ok ||
        machine.bios_control().load_arm7_bios(
            arm7_bios.data(),
            static_cast<uint32_t>(arm7_bios.size())) !=
            oracis::memory::Status::Ok) {
        return fail("synthetic BIOS images could not be loaded");
    }

    if (machine.start_retail_bios_boot() != NdsMachineStatus::Ok ||
        !machine.retail_bios_boot_active() ||
        machine.start_retail_bios_boot() !=
            NdsMachineStatus::RetailBiosBootAlreadyActive) {
        return fail("retail BIOS reset state did not start exactly once");
    }

    constexpr uint32_t ResetCpsr =
        ORACIS_ARM_MODE_SUPERVISOR | CPSR_I_BIT | CPSR_F_BIT;
    const ArmCpuState *arm9 = machine.cpu_state(NDS_ARM9);
    const ArmCpuState *arm7 = machine.cpu_state(NDS_ARM7);
    if (arm9 == nullptr || arm7 == nullptr ||
        arm9->R[15] != NdsBiosControl::Arm9BiosBase ||
        arm7->R[15] != NdsBiosControl::Arm7BiosBase ||
        arm9->cpsr != ResetCpsr || arm7->cpsr != ResetCpsr) {
        return fail("CPU states do not match the modeled BIOS reset vectors");
    }

    if (!machine.cpu_can_execute(NDS_ARM9) ||
        !machine.cpu_can_execute(NDS_ARM7) ||
        machine.cpu_is_held_in_reset(NDS_ARM9) ||
        machine.cpu_is_held_in_reset(NDS_ARM7) ||
        machine.cpu_is_halted(NDS_ARM9) ||
        machine.cpu_is_halted(NDS_ARM7)) {
        return fail("both BIOS CPUs were not runnable from their reset vectors");
    }

    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::IeAddress, 4u, 1u);
    machine.interrupt_control().request_irq(NDS_ARM7, 1u);
    if (!machine.cpu_can_execute(NDS_ARM7) ||
        machine.try_deliver_pending_irq(NDS_ARM7, 0u, 0u) !=
            NdsMachineStatus::IrqNotEligible) {
        return fail("ARM7 reset IRQ mask was conflated with a reset hold");
    }

    /* Direct-entry and pre-decoded IPL paths must not overwrite a BIOS boot. */
    const oracis::machine::NdsProgramSegment empty_segment{};
    if (machine.load_program_segments(empty_segment, empty_segment) !=
            NdsMachineStatus::RetailBiosBootActive ||
        machine.load_firmware_boot_code() !=
            NdsMachineStatus::RetailBiosBootActive) {
        return fail("direct-entry path was allowed to replace BIOS reset state");
    }

    ExecutionFixture fixture{};
    if (machine.run_dual_core_round(8u, &execute_slice, nullptr, &fixture) !=
            NdsMachineStatus::Ok ||
        fixture.calls[static_cast<unsigned>(NDS_ARM9)] != 1u ||
        fixture.calls[static_cast<unsigned>(NDS_ARM7)] != 1u ||
        machine.system_clock().global_bus_cycles() != 8u ||
        machine.cpu_is_held_in_reset(NDS_ARM7)) {
        return fail("both BIOS CPUs did not execute in the first synchronized round");
    }

    if (machine.release_arm7_from_reset() != NdsMachineStatus::CpuNotInReset ||
        machine.cpu_is_held_in_reset(NDS_ARM7) ||
        !machine.cpu_can_execute(NDS_ARM7) ||
        machine.cpu_state(NDS_ARM7)->R[15] != NdsBiosControl::Arm7BiosBase) {
        return fail("legacy ARM7 reset-release hook changed the parallel reset state");
    }

    if (machine.run_dual_core_round(8u, &execute_slice, nullptr, &fixture) !=
            NdsMachineStatus::Ok ||
        fixture.calls[static_cast<unsigned>(NDS_ARM9)] != 2u ||
        fixture.calls[static_cast<unsigned>(NDS_ARM7)] != 2u ||
        machine.system_clock().global_bus_cycles() != 16u) {
        return fail("both BIOS CPUs did not remain on the synchronized dual-core clock");
    }

    if (machine.shutdown() != NdsMachineStatus::Ok) {
        return fail("machine shutdown failed");
    }
    return 0;
}

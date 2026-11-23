#include "oracis/machine/nds_machine.hpp"

#include "oracis/host/detail/sha1.hpp"
#include "oracis/recomp/runtime_arm.h"
#include "oracis/recomp/static_dispatch_abi.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kArm9ProgramAddress = UINT32_C(0x02006000);
constexpr uint32_t kArm7ProgramAddress = UINT32_C(0x02381000);
constexpr uint32_t kGuestStackPointer = UINT32_C(0x02007FF8);
constexpr uint32_t kEffectAddress = UINT32_C(0x02007000);
constexpr uint32_t kSuccessfulOverlay = UINT32_C(70);
constexpr uint32_t kBudgetOverlay = UINT32_C(71);
constexpr uint32_t kMissingOverlay = UINT32_C(72);
constexpr uint32_t kSuccessfulOverlayAddress = UINT32_C(0x02008000);
constexpr uint32_t kBudgetOverlayAddress = UINT32_C(0x02008100);
constexpr uint32_t kMissingOverlayAddress = UINT32_C(0x02008200);

oracis::machine::NdsMachine *g_machine = nullptr;
std::array<uint32_t, 3> g_initializer_order{};
std::size_t g_initializer_order_count = 0u;

void record_initializer(uint32_t value)
{
    if (g_initializer_order_count < g_initializer_order.size()) {
        g_initializer_order[g_initializer_order_count++] = value;
    }
}

int fail(const char *message)
{
    std::fprintf(stderr, "nds_overlay_initializer_test: %s\n", message);
    return 1;
}

void test_bx_lr_return()
{
    const uint32_t target = g_cpu.R[14];
    g_cpu.R[15] = target & ((target & 1u) != 0u ? ~UINT32_C(1) : ~UINT32_C(3));
    if ((target & 1u) != 0u) {
        g_cpu.cpsr |= CPSR_T_BIT;
    } else {
        g_cpu.cpsr &= ~CPSR_T_BIT;
    }
    runtime_tick(1u);
    if (runtime_unwinding()) return;
    if (runtime_call_should_return(g_cpu.R[15])) return;
    runtime_dispatch_with_exchange(target);
}

void initializer_one()
{
    record_initializer(1u);
    if (g_machine != nullptr) {
        (void)g_machine->guest_memory().write(
            NDS_ARM9, kEffectAddress, 4u, UINT32_C(0xA1B2C3D4));
    }
    test_bx_lr_return();
}

void initializer_two()
{
    record_initializer(2u);
    if (g_machine != nullptr) {
        (void)g_machine->guest_memory().write(
            NDS_ARM9, kEffectAddress + 4u, 4u, UINT32_C(0x10203040));
    }
    test_bx_lr_return();
}

void initializer_three_thumb()
{
    record_initializer(3u);
    if (g_machine != nullptr) {
        (void)g_machine->guest_memory().write(
            NDS_ARM9, kEffectAddress + 8u, 4u, UINT32_C(0x55667788));
    }
    test_bx_lr_return();
}

void initializer_over_budget()
{
    runtime_tick(50u);
    if (runtime_unwinding()) return;
    test_bx_lr_return();
}

void initializer_with_missing_nested_target()
{
    constexpr uint32_t missing_target = UINT32_C(0x0200B000);
    g_cpu.R[15] = missing_target;
    runtime_dispatch(missing_target);
    if (runtime_unwinding()) return;
    test_bx_lr_return();
}

void put_u32_le(std::vector<uint8_t> &bytes, std::size_t offset, uint32_t value)
{
    if (offset + 4u > bytes.size()) return;
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1u] = static_cast<uint8_t>(value >> 8u);
    bytes[offset + 2u] = static_cast<uint8_t>(value >> 16u);
    bytes[offset + 3u] = static_cast<uint8_t>(value >> 24u);
}

oracis::machine::NdsOverlayImage make_overlay(
    uint32_t overlay_id,
    uint32_t address,
    std::vector<uint8_t> &contents,
    uint32_t initializer_count,
    uint32_t static_table_offset = 8u)
{
    const uint32_t static_begin = address + static_table_offset;
    const uint32_t static_end = static_begin + initializer_count * 4u;
    const std::string digest = oracis::host::detail::sha1_hex(contents);
    /* Keep the image hash alive for the duration of load_overlay(). */
    static std::array<std::string, 3> hashes;
    const std::size_t slot =
        overlay_id == kSuccessfulOverlay ? 0u :
        overlay_id == kBudgetOverlay ? 1u : 2u;
    hashes[slot] = digest;
    return {
        NDS_ARM9,
        overlay_id,
        address,
        static_cast<uint32_t>(contents.size()),
        16u,
        static_begin,
        static_end,
        contents.data(),
        hashes[slot].c_str()
    };
}

int prepare_machine(oracis::machine::NdsMachine &machine)
{
    using oracis::machine::NdsMachineStatus;
    const uint8_t arm9_program[] = {0x00u, 0x00u, 0xA0u, 0xE1u};
    const uint8_t arm7_program[] = {0x70u, 0x47u};
    const oracis::machine::NdsProgramSegment arm9{
        NDS_ARM9, kArm9ProgramAddress, sizeof(arm9_program),
        kArm9ProgramAddress, false, arm9_program
    };
    const oracis::machine::NdsProgramSegment arm7{
        NDS_ARM7, kArm7ProgramAddress, sizeof(arm7_program),
        kArm7ProgramAddress, true, arm7_program
    };
    if (machine.initialize() != NdsMachineStatus::Ok ||
        machine.load_program_segments(arm9, arm7) != NdsMachineStatus::Ok) {
        return fail("machine initialization or synthetic program load failed");
    }
    machine.cpu_state(NDS_ARM9)->R[13] = kGuestStackPointer;
    machine.cpu_state(NDS_ARM9)->R[0] = UINT32_C(0x11112222);
    machine.cpu_state(NDS_ARM9)->R[1] = UINT32_C(0x33334444);
    machine.cpu_state(NDS_ARM9)->R[2] = UINT32_C(0x55556666);
    machine.cpu_state(NDS_ARM9)->R[3] = UINT32_C(0x77778888);
    machine.cpu_state(NDS_ARM9)->R[14] = UINT32_C(0x02001234);
    machine.cpu_state(NDS_ARM9)->cpsr = ORACIS_ARM_MODE_SYSTEM;
    return 0;
}

} // namespace

int main()
{
    using oracis::machine::NdsMachine;
    using oracis::machine::NdsOverlayImage;
    using oracis::machine::NdsOverlayStatus;
    using oracis::machine::SharedWramMode;

    NdsMachine machine(SharedWramMode::Arm9All32K);
    if (prepare_machine(machine) != 0) return 1;
    g_machine = &machine;

    const NdsDispatchEntry entries[] = {
        {kSuccessfulOverlayAddress, 0u, &initializer_one, nullptr},
        {kSuccessfulOverlayAddress + 4u, 0u, &initializer_two, nullptr},
        {kSuccessfulOverlayAddress + 8u, 1u, &initializer_three_thumb, nullptr},
        {kBudgetOverlayAddress, 0u, &initializer_over_budget, nullptr},
        {kMissingOverlayAddress, 0u, &initializer_with_missing_nested_target, nullptr}
    };
    if (oracis_static_dispatch_register_bank(
            NDS_ARM9, entries, 4u, "overlay-initializer-test") !=
        ORACIS_DISPATCH_REGISTER_OK) {
        return fail("synthetic initializer dispatch bank registration failed");
    }

    std::vector<uint8_t> success_image(24u, 0u);
    put_u32_le(success_image, 12u, kSuccessfulOverlayAddress);
    put_u32_le(success_image, 16u, kSuccessfulOverlayAddress + 4u);
    put_u32_le(success_image, 20u, (kSuccessfulOverlayAddress + 8u) | 1u);
    NdsOverlayImage success_overlay = make_overlay(
        kSuccessfulOverlay, kSuccessfulOverlayAddress, success_image, 3u, 12u);
    if (machine.load_overlay(success_overlay) != NdsOverlayStatus::Ok) {
        return fail("successful initializer overlay failed to load");
    }

    g_initializer_order.fill(0u);
    g_initializer_order_count = 0u;
    ArmCpuState *arm9 = machine.cpu_state(NDS_ARM9);
    const ArmCpuState caller_state = *arm9;
    const NdsOverlayStatus success = machine.run_overlay_static_initializers(
        NDS_ARM9, kSuccessfulOverlay, 100u);
    if (success != NdsOverlayStatus::Ok ||
        g_initializer_order_count != 3u ||
        g_initializer_order != std::array<uint32_t, 3>{1u, 2u, 3u} ||
        machine.overlay_loader().static_initializer_progress(
            NDS_ARM9, kSuccessfulOverlay) != 3u ||
        runtime_call_stack_depth() != 0u ||
        arm9->R[0] != caller_state.R[0] ||
        arm9->R[1] != caller_state.R[1] ||
        arm9->R[2] != caller_state.R[2] ||
        arm9->R[3] != caller_state.R[3] ||
        arm9->R[13] != caller_state.R[13] ||
        arm9->R[14] != caller_state.R[14] ||
        arm9->R[15] != caller_state.R[15] ||
        arm9->cpsr != caller_state.cpsr) {
        return fail("constructors did not execute in order or preserve CPU context");
    }
    if (machine.guest_memory().read(
            NDS_ARM9, kEffectAddress, 4u).value != UINT32_C(0xA1B2C3D4) ||
        machine.guest_memory().read(
            NDS_ARM9, kEffectAddress + 4u, 4u).value != UINT32_C(0x10203040) ||
        machine.guest_memory().read(
            NDS_ARM9, kEffectAddress + 8u, 4u).value != UINT32_C(0x55667788)) {
        return fail("constructor guest-memory side effects were not retained");
    }
    if (machine.run_overlay_static_initializers(
            NDS_ARM9, kSuccessfulOverlay, 100u) !=
        NdsOverlayStatus::InitializersAlreadyRun) {
        return fail("completed constructor sequence was allowed to run twice");
    }

    std::vector<uint8_t> budget_image(12u, 0u);
    put_u32_le(budget_image, 8u, kBudgetOverlayAddress);
    NdsOverlayImage budget_overlay = make_overlay(
        kBudgetOverlay, kBudgetOverlayAddress, budget_image, 1u);
    if (machine.load_overlay(budget_overlay) != NdsOverlayStatus::Ok) {
        return fail("budget-test overlay failed to load");
    }
    if (machine.run_overlay_static_initializers(
            NDS_ARM9, kBudgetOverlay, 10u) !=
        NdsOverlayStatus::InitializerBudgetExceeded ||
        machine.overlay_loader().static_initializer_progress(
            NDS_ARM9, kBudgetOverlay) != 0u ||
        machine.run_overlay_static_initializers(
            NDS_ARM9, kBudgetOverlay, 100u) !=
        NdsOverlayStatus::InitializerExecutionFailed) {
        return fail("initializer cycle budget or no-retry failure state was not enforced");
    }

    std::vector<uint8_t> missing_image(12u, 0u);
    put_u32_le(missing_image, 8u, kMissingOverlayAddress);
    NdsOverlayImage missing_overlay = make_overlay(
        kMissingOverlay, kMissingOverlayAddress, missing_image, 1u);
    if (machine.load_overlay(missing_overlay) != NdsOverlayStatus::Ok) {
        return fail("dispatch-miss overlay failed to load");
    }
    if (machine.run_overlay_static_initializers(
            NDS_ARM9, kMissingOverlay, 100u) !=
        NdsOverlayStatus::InitializerDispatchMissing) {
        return fail("nested static-dispatch miss was not captured by the initializer boundary");
    }

    if (oracis_static_dispatch_unregister_bank(
            NDS_ARM9, entries, 4u) == 0) {
        return fail("synthetic initializer dispatch bank could not be unregistered");
    }
    g_machine = nullptr;
    if (machine.shutdown() != oracis::machine::NdsMachineStatus::Ok) {
        return fail("initializer test machine failed to shut down cleanly");
    }

    std::puts("bounded overlay static initializer execution tests passed");
    return 0;
}

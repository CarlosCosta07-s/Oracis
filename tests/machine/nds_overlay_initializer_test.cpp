#include "oracis/machine/nds_machine.hpp"

#include "oracis/host/detail/sha1.hpp"
#include "oracis/recomp/runtime_arm.h"
#include "oracis/recomp/runtime_scheduler.h"
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
constexpr uint32_t kDeferredOverlay = UINT32_C(73);
constexpr uint32_t kDeferredOverlayAddress = UINT32_C(0x02008300);
constexpr uint32_t kDeferredLoadOverlay = UINT32_C(74);
constexpr uint32_t kDeferredLoadOverlayAddress = UINT32_C(0x02008400);
constexpr uint32_t kDestructorChainHeadAddress = UINT32_C(0x02008A00);
constexpr uint32_t kDestructorChainNodeAddress = UINT32_C(0x02008A20);
constexpr uint32_t kDestructorChainObjectAddress = UINT32_C(0x0200C080);
constexpr uint32_t kDestructorChainFunctionPointer = UINT32_C(0x0200C101);

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

void initializer_deferred()
{
    record_initializer(4u);
    test_bx_lr_return();
}

struct DeferredRequestContext {
    oracis::machine::NdsMachine *machine = nullptr;
    uint32_t overlay_id = 0u;
    oracis::machine::NdsOverlayStatus request_status =
        oracis::machine::NdsOverlayStatus::Busy;
};

bool request_deferred_initializer(
    void *opaque, NdsCpu cpu, ArmCpuState *state, uint64_t cycle_budget)
{
    (void)state;
    (void)cycle_budget;
    auto *context = static_cast<DeferredRequestContext *>(opaque);
    if (context == nullptr || context->machine == nullptr) return false;
    if (cpu != NDS_ARM9) return true;

    context->request_status = context->machine->request_overlay_static_initializers(
        cpu, context->overlay_id, 100u);
    return context->request_status ==
            oracis::machine::NdsOverlayStatus::InitializerPending &&
        runtime_should_yield();
}

struct DeferredLoadRequestContext {
    oracis::machine::NdsMachine *machine = nullptr;
    oracis::machine::NdsOverlayImage image{};
    oracis::machine::NdsOverlayStatus request_status =
        oracis::machine::NdsOverlayStatus::Busy;
};

bool request_deferred_overlay_load(
    void *opaque, NdsCpu cpu, ArmCpuState *state, uint64_t cycle_budget)
{
    (void)state;
    (void)cycle_budget;
    auto *context = static_cast<DeferredLoadRequestContext *>(opaque);
    if (context == nullptr || context->machine == nullptr) return false;
    if (cpu != NDS_ARM9) return true;

    context->request_status =
        context->machine->request_overlay_load(context->image);
    return context->request_status ==
            oracis::machine::NdsOverlayStatus::OverlayLoadPending &&
        runtime_should_yield();
}

struct DestructorRegistrationContext {
    oracis::machine::NdsMachine *machine = nullptr;
    uint32_t head_pointer_address = 0u;
    uint32_t object_address = 0u;
    uint32_t destructor_pointer = 0u;
    uint32_t chain_node_address = 0u;
    oracis::machine::NdsOverlayDestructorChainStatus status =
        oracis::machine::NdsOverlayDestructorChainStatus::Busy;
};

bool register_guest_global_destructor(
    void *opaque, NdsCpu cpu, ArmCpuState *state, uint64_t cycle_budget)
{
    (void)state;
    (void)cycle_budget;
    auto *context = static_cast<DestructorRegistrationContext *>(opaque);
    if (context == nullptr || context->machine == nullptr) return false;
    if (cpu != NDS_ARM9) return true;

    context->status = context->machine->register_overlay_global_object(
        cpu,
        context->head_pointer_address,
        context->object_address,
        context->destructor_pointer,
        context->chain_node_address);
    runtime_tick(1u);
    return context->status ==
        oracis::machine::NdsOverlayDestructorChainStatus::Ok;
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
    using oracis::memory::SharedWramMode;

    NdsMachine machine(SharedWramMode::Arm9All32K);
    if (prepare_machine(machine) != 0) return 1;
    g_machine = &machine;

    /* Match the retail NitroSDK overlay address regions and CP15 windows. */
    constexpr uint32_t ItcmRegionControl = UINT32_C(16) << 1u;
    constexpr uint32_t DtcmRegionControl =
        UINT32_C(0x027C0000) | (UINT32_C(5) << 1u);
    const uint32_t TcmControl =
        oracis::memory::NdsArm9Tcm::ResetControl |
        oracis::memory::NdsArm9Tcm::ItcmEnableMask |
        oracis::memory::NdsArm9Tcm::DtcmEnableMask;
    if (!machine.arm9_tcm().write_region(true, ItcmRegionControl) ||
        !machine.arm9_tcm().write_region(false, DtcmRegionControl) ||
        !machine.arm9_tcm().set_control(TcmControl)) {
        return fail("retail ITCM/DTCM windows could not be configured");
    }

    const std::array<uint8_t, 0x20> itcm_image{};
    const std::vector<uint8_t> itcm_bytes(
        itcm_image.begin(), itcm_image.end());
    const std::string itcm_digest =
        oracis::host::detail::sha1_hex(itcm_bytes);
    constexpr uint32_t RetailItcmOverlayAddress = UINT32_C(0x01FF8660);
    const NdsOverlayImage retail_itcm_overlay{
        NDS_ARM9, 300u, RetailItcmOverlayAddress,
        static_cast<uint32_t>(itcm_image.size()), 0u,
        0u, 0u, itcm_image.data(), itcm_digest.c_str()
    };
    if (machine.load_overlay(retail_itcm_overlay) != NdsOverlayStatus::Ok ||
        machine.guest_memory().validate_execute(
            NDS_ARM9, RetailItcmOverlayAddress,
            static_cast<uint32_t>(itcm_image.size())) !=
            oracis::memory::Status::Ok) {
        return fail("NitroSDK ITCM overlay aperture was not classified and loaded");
    }

    constexpr uint32_t RetailDtcmOverlayAddress = UINT32_C(0x027C0000);
    const std::array<uint8_t, 8> dtcm_image{};
    const std::vector<uint8_t> dtcm_bytes(dtcm_image.begin(), dtcm_image.end());
    const std::string dtcm_digest =
        oracis::host::detail::sha1_hex(dtcm_bytes);
    const NdsOverlayImage retail_dtcm_overlay{
        NDS_ARM9, 301u, RetailDtcmOverlayAddress,
        static_cast<uint32_t>(dtcm_image.size()), 8u, 0u, 0u,
        dtcm_image.data(), dtcm_digest.c_str()
    };
    const NdsOverlayStatus dtcm_load_status =
        machine.load_overlay(retail_dtcm_overlay);
    const oracis::memory::Status dtcm_write_status =
        machine.guest_memory().validate_write(
            NDS_ARM9, RetailDtcmOverlayAddress, 16u);
    const oracis::memory::Status dtcm_read_status =
        machine.guest_memory().validate_read(
            NDS_ARM9, RetailDtcmOverlayAddress,
            static_cast<uint32_t>(dtcm_image.size()));
    if (dtcm_load_status != NdsOverlayStatus::Ok ||
        dtcm_write_status != oracis::memory::Status::Ok ||
        dtcm_read_status != oracis::memory::Status::Ok) {
        std::fprintf(stderr,
            "DTCM diagnostics: load=%u write=%u read=%u\\n",
            static_cast<unsigned>(dtcm_load_status),
            static_cast<unsigned>(dtcm_write_status),
            static_cast<unsigned>(dtcm_read_status));
        return fail("DTCM data overlay was rejected or data access failed");
    }

    const NdsDispatchEntry entries[] = {
        {kSuccessfulOverlayAddress, 0u, &initializer_one, nullptr},
        {kSuccessfulOverlayAddress + 4u, 0u, &initializer_two, nullptr},
        {kSuccessfulOverlayAddress + 8u, 1u, &initializer_three_thumb, nullptr},
        {kBudgetOverlayAddress, 0u, &initializer_over_budget, nullptr},
        {kMissingOverlayAddress, 0u, &initializer_with_missing_nested_target, nullptr},
        {kDeferredOverlayAddress, 0u, &initializer_deferred, nullptr}
    };
    if (oracis_static_dispatch_register_bank(
            NDS_ARM9, entries, 6u, "overlay-initializer-test") !=
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
    if (machine.unload_overlay(
            NDS_ARM9, kSuccessfulOverlay, true) !=
        NdsOverlayStatus::FinalizersUnavailable) {
        return fail("started overlay was unloaded without static finalization");
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

    std::vector<uint8_t> deferred_image(12u, 0u);
    put_u32_le(deferred_image, 8u, kDeferredOverlayAddress);
    NdsOverlayImage deferred_overlay = make_overlay(
        kDeferredOverlay, kDeferredOverlayAddress, deferred_image, 1u);
    if (machine.load_overlay(deferred_overlay) != NdsOverlayStatus::Ok) {
        return fail("deferred-initializer overlay failed to load");
    }

    g_initializer_order.fill(0u);
    g_initializer_order_count = 0u;
    DeferredRequestContext deferred_context{&machine, kDeferredOverlay};
    const ArmCpuState pre_round_state = *arm9;
    const auto round_status = machine.run_dual_core_round(
        100u, &request_deferred_initializer, nullptr, &deferred_context);
    const uint32_t deferred_progress =
        machine.overlay_loader().static_initializer_progress(
            NDS_ARM9, kDeferredOverlay);
    const bool deferred_call_stack_balanced = runtime_call_stack_depth() == 0u;
    const bool deferred_cpu_restored =
        arm9->R[13] == pre_round_state.R[13] &&
        arm9->R[14] == pre_round_state.R[14] &&
        arm9->R[15] == pre_round_state.R[15] &&
        arm9->cpsr == pre_round_state.cpsr;
    if (round_status != oracis::machine::NdsMachineStatus::Ok ||
        deferred_context.request_status != NdsOverlayStatus::InitializerPending ||
        g_initializer_order_count != 1u ||
        g_initializer_order[0] != 4u ||
        deferred_progress != 1u ||
        !deferred_call_stack_balanced ||
        !deferred_cpu_restored) {
        std::fprintf(stderr,
            "deferred diagnostics: round=%u request=%u order_count=%zu first=%u "
            "progress=%u call_depth=%u cpu_pc=0x%08X expected_pc=0x%08X "
            "arm9_cycles=%llu\\n",
            static_cast<unsigned>(round_status),
            static_cast<unsigned>(deferred_context.request_status),
            g_initializer_order_count, g_initializer_order[0],
            deferred_progress, runtime_call_stack_depth(),
            arm9->R[15], pre_round_state.R[15],
            static_cast<unsigned long long>(
                oracis_scheduler_cycle_count(NDS_ARM9)));
        return fail("deferred constructors did not run at the guest slice boundary");
    }

    std::vector<uint8_t> deferred_load_bytes{
        0x91u, 0x82u, 0x73u, 0x64u,
        0x55u, 0x46u, 0x37u, 0x28u
    };
    const std::string deferred_load_sha1 =
        oracis::host::detail::sha1_hex(deferred_load_bytes);
    const NdsOverlayImage deferred_load_image{
        NDS_ARM9,
        kDeferredLoadOverlay,
        kDeferredLoadOverlayAddress,
        static_cast<uint32_t>(deferred_load_bytes.size()),
        8u,
        0u,
        0u,
        deferred_load_bytes.data(),
        deferred_load_sha1.c_str()
    };
    DeferredLoadRequestContext deferred_load_context{
        &machine, deferred_load_image
    };
    const auto deferred_load_round = machine.run_dual_core_round(
        100u, &request_deferred_overlay_load, nullptr,
        &deferred_load_context);
    if (deferred_load_round != oracis::machine::NdsMachineStatus::Ok ||
        deferred_load_context.request_status !=
            NdsOverlayStatus::OverlayLoadPending ||
        !machine.overlay_loader().is_loaded(
            NDS_ARM9, kDeferredLoadOverlay)) {
        return fail("guest-requested overlay load was not published at the slice boundary");
    }
    for (std::size_t index = 0u;
         index < deferred_load_bytes.size(); ++index) {
        const auto byte = machine.guest_memory().read(
            NDS_ARM9,
            kDeferredLoadOverlayAddress + static_cast<uint32_t>(index),
            1u);
        if (!byte || byte.value != deferred_load_bytes[index]) {
            return fail("deferred overlay load did not publish the complete image");
        }
    }
    const auto bss_word = machine.guest_memory().read(
        NDS_ARM9,
        kDeferredLoadOverlayAddress +
            static_cast<uint32_t>(deferred_load_bytes.size()),
        4u);
    if (!bss_word || bss_word.value != 0u ||
        machine.request_overlay_load(deferred_load_image) !=
            NdsOverlayStatus::AlreadyLoaded) {
        return fail("deferred overlay BSS or duplicate-load semantics were incorrect");
    }

    if (machine.guest_memory().write(
            NDS_ARM9, kDestructorChainHeadAddress, 4u, 0u) !=
        oracis::memory::Status::Ok) {
        return fail("could not initialize the synthetic destructor chain head");
    }
    DestructorRegistrationContext registration_context{
        &machine,
        kDestructorChainHeadAddress,
        kDestructorChainObjectAddress,
        kDestructorChainFunctionPointer,
        kDestructorChainNodeAddress
    };
    const auto registration_round = machine.run_dual_core_round(
        100u, &register_guest_global_destructor, nullptr,
        &registration_context);
    if (registration_round != oracis::machine::NdsMachineStatus::Ok ||
        registration_context.status !=
            oracis::machine::NdsOverlayDestructorChainStatus::Ok) {
        return fail("guest-slice Metrowerks destructor registration failed");
    }

    const auto registered_head = machine.guest_memory().read(
        NDS_ARM9, kDestructorChainHeadAddress, 4u);
    const auto registered_next = machine.guest_memory().read(
        NDS_ARM9, kDestructorChainNodeAddress, 4u);
    const auto registered_dtor = machine.guest_memory().read(
        NDS_ARM9, kDestructorChainNodeAddress + 4u, 4u);
    const auto registered_obj = machine.guest_memory().read(
        NDS_ARM9, kDestructorChainNodeAddress + 8u, 4u);
    if (!registered_head || registered_head.value !=
            kDestructorChainNodeAddress ||
        !registered_next || registered_next.value != 0u ||
        !registered_dtor || registered_dtor.value !=
            kDestructorChainFunctionPointer ||
        !registered_obj || registered_obj.value !=
            kDestructorChainObjectAddress) {
        return fail("Metrowerks destructor registration wrote the wrong guest node fields");
    }
    if (machine.register_overlay_global_object(
            NDS_ARM9, kDestructorChainHeadAddress,
            kDestructorChainObjectAddress, kDestructorChainFunctionPointer,
            kDestructorChainNodeAddress) !=
        oracis::machine::NdsOverlayDestructorChainStatus::Busy) {
        return fail("destructor registration was accepted outside the owning guest slice");
    }

    if (oracis_static_dispatch_unregister_bank(
            NDS_ARM9, entries, 6u) == 0) {
        return fail("synthetic initializer dispatch bank could not be unregistered");
    }
    g_machine = nullptr;
    if (machine.shutdown() != oracis::machine::NdsMachineStatus::Ok) {
        return fail("initializer test machine failed to shut down cleanly");
    }

    std::puts("bounded overlay static initializer execution tests passed");
    return 0;
}

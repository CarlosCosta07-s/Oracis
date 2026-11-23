#include "oracis/recomp/static_dispatch_abi.h"
#include "oracis/recomp/static_dispatch.hpp"

#include <cstdint>
#include <cstdio>

namespace {

unsigned arm9_calls = 0u;
unsigned arm7_calls = 0u;
unsigned thumb_calls = 0u;
unsigned arm7_thumb_calls = 0u;
unsigned validated_calls = 0u;

void arm9_body(void) { ++arm9_calls; }
void arm7_body(void) { ++arm7_calls; }
void thumb_body(void) { ++thumb_calls; }
void arm7_thumb_body(void) { ++arm7_thumb_calls; }
void validated_body(void) { ++validated_calls; }

const uint8_t expected_bytes[] = {0x12u, 0x34u, 0x56u, 0x78u};
const NdsStaticValidation validation = {
    0x02001000u, sizeof(expected_bytes), expected_bytes, nullptr, 0u
};

int accept_expected_validation(
    NdsCpu cpu,
    const NdsStaticValidation *candidate,
    uint32_t pc,
    uint32_t thumb,
    void *user)
{
    return cpu == NDS_ARM9 &&
           candidate == user &&
           pc == 0x02001000u &&
           thumb == 0u;
}

int fail(const char *message)
{
    std::fprintf(stderr, "static_dispatch_runtime_test: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    ArmCpuState arm9{};
    ArmCpuState arm7{};
    arm9.cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm7.cpsr = ORACIS_ARM_MODE_SYSTEM;

    if (!oracis_arm_runtime_bind_cpus(&arm9, &arm7))
        return fail("CPU-state binding failed");

    oracis_static_dispatch_clear();
    oracis_static_dispatch_set_validation_callback(nullptr, nullptr);

    const NdsDispatchEntry arm9_table[] = {
        {0x02000000u, 0u, arm9_body, nullptr},
        {0x02000020u, 1u, thumb_body, nullptr},
    };
    const NdsDispatchEntry arm7_table[] = {
        {0x02380000u, 0u, arm7_body, nullptr},
        {0x02380020u, 1u, arm7_thumb_body, nullptr},
    };
    const NdsDispatchEntry validated_table[] = {
        {0x02001000u, 0u, validated_body, &validation},
    };

    if (oracis_static_dispatch_register_bank(
            NDS_ARM9, arm9_table, 2u, "runtime-arm9") !=
        static_cast<int>(oracis::recomp::RegisterBankStatus::Ok)) {
        return fail("ARM9 bank registration failed");
    }
    if (oracis_static_dispatch_register_bank(
            NDS_ARM7, arm7_table, 2u, "runtime-arm7") !=
        static_cast<int>(oracis::recomp::RegisterBankStatus::Ok)) {
        return fail("ARM7 bank registration failed");
    }
    if (oracis_static_dispatch_register_bank(
            NDS_ARM9, validated_table, 1u, "validated-arm9") !=
        static_cast<int>(oracis::recomp::RegisterBankStatus::Ok)) {
        return fail("validated bank registration failed");
    }

    /* Same-state dispatch resolves the currently selected CPU. */
    runtime_dispatch(0x02000000u);
    if (arm9_calls != 1u || arm7_calls != 0u)
        return fail("ARM9 runtime dispatch invoked the wrong bank");

    if (!oracis_arm_runtime_select_cpu(NDS_ARM7))
        return fail("ARM7 selection failed");
    runtime_dispatch(0x02380000u);
    if (arm7_calls != 1u || arm9_calls != 1u)
        return fail("ARM7 runtime dispatch invoked the wrong bank");

    /* ARM7 must preserve its own CPSR.T across the same exchange fallback. */
    runtime_dispatch_with_exchange(0x02380021u);
    if (arm7_thumb_calls != 1u || (arm7.cpsr & CPSR_T_BIT) == 0u)
        return fail("ARM7 Thumb exchange did not select its Thumb row");
    NdsLinkSlot arm7_call_slot{};
    arm7_call_slot.target_pc = 0x02380000u;
    runtime_link_call(&arm7_call_slot);
    if (arm7_calls != 2u || (arm7.cpsr & CPSR_T_BIT) != 0u)
        return fail("ARM7 Thumb-to-ARM link-call did not exchange state");
    NdsLinkSlot arm7_branch_slot{};
    arm7_branch_slot.target_pc = 0x02380021u;
    runtime_link_branch(&arm7_branch_slot);
    if (arm7_thumb_calls != 2u || (arm7.cpsr & CPSR_T_BIT) == 0u)
        return fail("ARM7 ARM-to-Thumb link-branch did not exchange state");
    NdsLinkSlot arm7_fallthrough_slot{};
    arm7_fallthrough_slot.target_pc = 0x02380000u;
    runtime_link_fallthrough(&arm7_fallthrough_slot);
    if (arm7_calls != 3u || (arm7.cpsr & CPSR_T_BIT) != 0u)
        return fail("ARM7 Thumb-to-ARM link-fallthrough did not exchange state");

    /* Exchange takes the state from target bit zero before lookup. */
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9))
        return fail("ARM9 reselection failed");
    runtime_dispatch_with_exchange(0x02000021u);
    if (thumb_calls != 1u || (arm9.cpsr & CPSR_T_BIT) == 0u)
        return fail("Thumb exchange did not set CPSR.T and invoke Thumb row");

    runtime_dispatch_with_exchange(0x02000000u);
    if (arm9_calls != 2u || (arm9.cpsr & CPSR_T_BIT) != 0u)
        return fail("ARM exchange did not clear CPSR.T and invoke ARM row");

    /*
     * Link slots encode the target ISA in target_pc bit zero. The unresolved
     * fallback must preserve that state while using ordinary table lookup.
     */
    NdsLinkSlot call_slot{};
    call_slot.target_pc = 0x02000021u;
    NdsLinkSlot branch_slot{};
    branch_slot.target_pc = 0x02000000u;
    NdsLinkSlot fallthrough_slot{};
    fallthrough_slot.target_pc = 0x02000021u;

    runtime_link_call(&call_slot);
    if (thumb_calls != 2u || (arm9.cpsr & CPSR_T_BIT) == 0u) {
        return fail("Thumb link-call did not select Thumb state and row");
    }
    runtime_link_branch(&branch_slot);
    if (arm9_calls != 3u || (arm9.cpsr & CPSR_T_BIT) != 0u) {
        return fail("ARM link-branch did not restore ARM state and row");
    }
    runtime_link_fallthrough(&fallthrough_slot);
    if (thumb_calls != 3u || (arm9.cpsr & CPSR_T_BIT) == 0u) {
        return fail("Thumb link-fallthrough did not select Thumb state and row");
    }
    if (runtime_inline_leaf_admit(&call_slot, thumb_body) != 0) {
        return fail("inline leaf was admitted without a resolved link cache");
    }

    /* Restore the ARM caller state before checking the ARM validation row. */
    runtime_dispatch_with_exchange(0x02000000u);
    if ((arm9.cpsr & CPSR_T_BIT) != 0u) {
        return fail("ARM state was not restored after link-slot mode tests");
    }

    /* A validated/mutable row stays inactive until identity is proven. */
    uint32_t lookup_status = UINT32_MAX;
    uint32_t candidate_count = 0u;
    const NdsDispatchEntry *inactive = nullptr;
    const NdsDispatchEntry *selected = oracis_static_dispatch_lookup(
        NDS_ARM9, 0x02001000u, arm9.cpsr,
        &lookup_status, &candidate_count, &inactive);
    if (selected != nullptr ||
        lookup_status != static_cast<uint32_t>(
            oracis::recomp::DispatchLookupStatus::Miss) ||
        candidate_count != 1u || inactive != &validated_table[0]) {
        return fail("validated row ran without an identity callback");
    }

    const uint32_t before_callback_key =
        oracis::recomp::global_static_dispatch_registry().link_epoch_key(NDS_ARM9);
    oracis_static_dispatch_set_validation_callback(
        accept_expected_validation,
        const_cast<NdsStaticValidation *>(&validation));
    const uint32_t after_callback_key =
        oracis::recomp::global_static_dispatch_registry().link_epoch_key(NDS_ARM9);
    if (after_callback_key == before_callback_key) {
        return fail("validation callback replacement did not invalidate link keys");
    }
    runtime_dispatch(0x02001000u);
    if (validated_calls != 1u)
        return fail("accepted validated row was not invoked");

    if (!oracis_static_dispatch_unregister_bank(
            NDS_ARM9, validated_table, 1u)) {
        return fail("validated bank unregister failed");
    }
    selected = oracis_static_dispatch_lookup(
        NDS_ARM9, 0x02001000u, arm9.cpsr,
        &lookup_status, &candidate_count, &inactive);
    if (selected != nullptr ||
        lookup_status != static_cast<uint32_t>(
            oracis::recomp::DispatchLookupStatus::Miss)) {
        return fail("unregistered validated row remained searchable");
    }

    /* Clearing bank rows must preserve the still-bound memory validator. */
    oracis_static_dispatch_clear();
    if (oracis_static_dispatch_register_bank(
            NDS_ARM9, validated_table, 1u, "validated-after-clear") !=
        static_cast<int>(oracis::recomp::RegisterBankStatus::Ok)) {
        return fail("bank registration after registry clear failed");
    }
    runtime_dispatch(0x02001000u);
    if (validated_calls != 2u) {
        return fail("registry clear unexpectedly removed the memory validator");
    }

    const uint32_t before_clear_key =
        oracis::recomp::global_static_dispatch_registry().link_epoch_key(NDS_ARM9);
    oracis_static_dispatch_clear();
    const uint32_t after_clear_key =
        oracis::recomp::global_static_dispatch_registry().link_epoch_key(NDS_ARM9);
    if (after_clear_key == before_clear_key) {
        return fail("C ABI registry clear did not invalidate link keys");
    }
    const uint32_t before_reset_callback_key = after_clear_key;
    oracis_static_dispatch_set_validation_callback(nullptr, nullptr);
    if (oracis::recomp::global_static_dispatch_registry().link_epoch_key(NDS_ARM9) ==
        before_reset_callback_key) {
        return fail("validation callback reset did not invalidate link keys");
    }

    std::puts("C ABI static dispatch registration and runtime transfer passed");
    return 0;
}

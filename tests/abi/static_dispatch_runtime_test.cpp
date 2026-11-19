#include "oracis/recomp/static_dispatch_abi.h"
#include "oracis/recomp/static_dispatch.hpp"

#include <cstdint>
#include <cstdio>

namespace {

unsigned arm9_calls = 0u;
unsigned arm7_calls = 0u;
unsigned thumb_calls = 0u;
unsigned validated_calls = 0u;

void arm9_body(void) { ++arm9_calls; }
void arm7_body(void) { ++arm7_calls; }
void thumb_body(void) { ++thumb_calls; }
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
            NDS_ARM7, arm7_table, 1u, "runtime-arm7") !=
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

    /* Exchange takes the state from target bit zero before lookup. */
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9))
        return fail("ARM9 reselection failed");
    runtime_dispatch_with_exchange(0x02000021u);
    if (thumb_calls != 1u || (arm9.cpsr & CPSR_T_BIT) == 0u)
        return fail("Thumb exchange did not set CPSR.T and invoke Thumb row");

    runtime_dispatch_with_exchange(0x02000000u);
    if (arm9_calls != 2u || (arm9.cpsr & CPSR_T_BIT) != 0u)
        return fail("ARM exchange did not clear CPSR.T and invoke ARM row");

    /* Link slots currently remain unresolved and route through dispatch. */
    NdsLinkSlot call_slot{};
    call_slot.target_pc = 0x02000000u;
    NdsLinkSlot branch_slot{};
    branch_slot.target_pc = 0x02000000u;
    NdsLinkSlot fallthrough_slot{};
    fallthrough_slot.target_pc = 0x02000000u;

    runtime_link_call(&call_slot);
    runtime_link_branch(&branch_slot);
    runtime_link_fallthrough(&fallthrough_slot);
    if (arm9_calls != 5u) {
        return fail("unresolved link slots did not use the normal dispatcher");
    }
    if (runtime_inline_leaf_admit(&call_slot, arm9_body) != 0) {
        return fail("inline leaf was admitted without a resolved link cache");
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

    oracis_static_dispatch_set_validation_callback(
        accept_expected_validation,
        const_cast<NdsStaticValidation *>(&validation));
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

    oracis_static_dispatch_clear();
    oracis_static_dispatch_set_validation_callback(nullptr, nullptr);

    std::puts("C ABI static dispatch registration and runtime transfer passed");
    return 0;
}

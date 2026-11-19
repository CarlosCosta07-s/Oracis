#include "oracis/recomp/static_dispatch.hpp"

#include <cstdint>
#include <cstdio>

using oracis::recomp::DispatchLookupStatus;
using oracis::recomp::RegisterBankStatus;
using oracis::recomp::StaticDispatchRegistry;

namespace {

int arm9_entry_a_calls = 0;
int arm9_entry_b_calls = 0;
int arm7_entry_calls = 0;
int thumb_entry_calls = 0;

void arm9_entry_a(void) { ++arm9_entry_a_calls; }
void arm9_entry_b(void) { ++arm9_entry_b_calls; }
void arm7_entry(void) { ++arm7_entry_calls; }
void thumb_entry(void) { ++thumb_entry_calls; }

int fail(const char *message)
{
    std::fprintf(stderr, "static_dispatch_test: %s\n", message);
    return 1;
}

struct ValidationMemory {
    uint32_t base;
    const uint8_t *bytes;
    uint32_t size;
};

bool validate_bytes(const NdsStaticValidation *validation,
                    uint32_t pc,
                    bool thumb,
                    void *user)
{
    if (validation == nullptr || user == nullptr) return false;
    const auto *memory = static_cast<const ValidationMemory *>(user);
    const uint64_t validation_end =
        static_cast<uint64_t>(validation->addr) + validation->size;
    const uint64_t memory_end =
        static_cast<uint64_t>(memory->base) + memory->size;
    const uint32_t instruction_size = thumb ? 2u : 4u;

    if (validation->addr < memory->base ||
        validation_end > memory_end ||
        pc < validation->addr ||
        static_cast<uint64_t>(pc) + instruction_size > validation_end) {
        return false;
    }

    const std::size_t offset = validation->addr - memory->base;
    for (uint32_t index = 0u; index < validation->size; ++index) {
        if (validation->expected[index] != memory->bytes[offset + index])
            return false;
    }

    for (uint32_t dep_index = 0u;
         dep_index < validation->dependency_count;
         ++dep_index) {
        const NdsStaticValidationRange &dependency =
            validation->dependencies[dep_index];
        const uint64_t dep_end =
            static_cast<uint64_t>(dependency.addr) + dependency.size;
        if (dependency.addr < memory->base || dep_end > memory_end)
            return false;
        const std::size_t dep_offset = dependency.addr - memory->base;
        for (uint32_t index = 0u; index < dependency.size; ++index) {
            if (dependency.expected[index] !=
                memory->bytes[dep_offset + index]) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

int main()
{
    StaticDispatchRegistry registry;

    const NdsDispatchEntry arm9_table[] = {
        {0x02000000u, 0u, arm9_entry_a, nullptr},
        {0x02000100u, 0u, arm9_entry_a, nullptr},
        {0x02000200u, 1u, thumb_entry, nullptr},
    };
    const NdsDispatchEntry arm7_table[] = {
        {0x02380000u, 0u, arm7_entry, nullptr},
    };

    if (registry.register_bank(NDS_ARM9, arm9_table, 3u, "arm9-main") !=
        RegisterBankStatus::Ok) {
        return fail("ARM9 bank registration failed");
    }
    if (registry.register_bank(NDS_ARM7, arm7_table, 1u, "arm7-main") !=
        RegisterBankStatus::Ok) {
        return fail("ARM7 bank registration failed");
    }
    if (registry.bank_count(NDS_ARM9) != 1u ||
        registry.bank_count(NDS_ARM7) != 1u) {
        return fail("bank counts are incorrect");
    }

    auto result = registry.lookup(NDS_ARM9, 0x02000001u, 0u);
    if (!result || result.selected->fn != arm9_entry_a ||
        result.selected->addr != 0x02000000u) {
        return fail("ARM address lookup did not strip the target mode bit");
    }

    result = registry.lookup(NDS_ARM9, 0x02000201u, CPSR_T_BIT);
    if (!result || result.selected->fn != thumb_entry) {
        return fail("Thumb dispatch lookup failed");
    }

    result = registry.lookup(NDS_ARM9, 0x02000200u, 0u);
    if (result.status != DispatchLookupStatus::Miss) {
        return fail("ARM lookup incorrectly selected a Thumb-only row");
    }

    result = registry.lookup(NDS_ARM7, 0x02380000u, 0u);
    if (!result || result.selected->fn != arm7_entry) {
        return fail("ARM7 dispatch selected wrong CPU table");
    }

    result = registry.lookup(static_cast<NdsCpu>(77), 0x02000000u, 0u);
    if (result.status != DispatchLookupStatus::InvalidCpu) {
        return fail("invalid CPU lookup was not rejected");
    }

    result = registry.lookup(NDS_ARM7, 0x02000000u, 0u);
    if (result.status != DispatchLookupStatus::Miss) {
        return fail("missing address should return a static dispatch miss");
    }

    result = registry.lookup(NDS_ARM7, 0x02000002u, 0u);
    if (result.status != DispatchLookupStatus::MisalignedTarget) {
        return fail("misaligned ARM target was not rejected");
    }

    const uint8_t expected_small[] = {
        0x10u, 0x20u, 0x30u, 0x40u,
    };
    const uint8_t expected_large[] = {
        0x10u, 0x20u, 0x30u, 0x40u,
        0x50u, 0x60u, 0x70u, 0x80u,
    };
    const NdsStaticValidation small_validation = {
        0x02001000u, 4u, expected_small, nullptr, 0u
    };
    const NdsStaticValidation large_validation = {
        0x02001000u, 8u, expected_large, nullptr, 0u
    };
    const NdsDispatchEntry small_candidate[] = {
        {0x02001000u, 0u, arm9_entry_a, &small_validation},
    };
    const NdsDispatchEntry large_candidate[] = {
        {0x02001000u, 0u, arm9_entry_b, &large_validation},
    };

    if (registry.register_bank(NDS_ARM9, small_candidate, 1u, "arm9-small") !=
            RegisterBankStatus::Ok ||
        registry.register_bank(NDS_ARM9, large_candidate, 1u, "arm9-large") !=
            RegisterBankStatus::Ok) {
        return fail("validated candidate bank registration failed");
    }

    const uint8_t live_bytes[] = {
        0x10u, 0x20u, 0x30u, 0x40u,
        0x50u, 0x60u, 0x70u, 0x80u,
    };
    ValidationMemory validation_memory{
        0x02001000u, live_bytes, sizeof(live_bytes)
    };

    result = registry.lookup(
        NDS_ARM9, 0x02001000u, 0u, validate_bytes, &validation_memory);
    if (!result || result.selected->fn != arm9_entry_b ||
        result.candidate_count != 2u) {
        return fail("larger live validation span did not win");
    }

    const uint8_t changed_bytes[] = {
        0x10u, 0x20u, 0x30u, 0x40u,
        0x99u, 0x60u, 0x70u, 0x80u,
    };
    validation_memory.bytes = changed_bytes;
    result = registry.lookup(
        NDS_ARM9, 0x02001000u, 0u, validate_bytes, &validation_memory);
    if (!result || result.selected->fn != arm9_entry_a ||
        result.inactive == nullptr ||
        result.candidate_count != 2u) {
        return fail("stale larger candidate was not ignored");
    }

    /* Without a validation callback, mutable/validated candidates stay
       inactive; the immutable lower-ranked row remains eligible. */
    result = registry.lookup(NDS_ARM9, 0x02001000u, 0u);
    if (!result || result.selected->fn != arm9_entry_a) {
        return fail("validated candidate ran without a validation callback");
    }

    const NdsDispatchEntry unsorted[] = {
        {0x02002000u, 0u, arm9_entry_a, nullptr},
        {0x02001000u, 0u, arm9_entry_a, nullptr},
    };
    if (registry.register_bank(NDS_ARM9, unsorted, 2u, "unsorted") !=
        RegisterBankStatus::UnsortedTable) {
        return fail("unsorted dispatch table was accepted");
    }

    /* Equal addresses are sorted by the mode byte (ARM=0, Thumb=1). */
    const NdsDispatchEntry same_address_modes[] = {
        {0x02005000u, 0u, arm9_entry_a, nullptr},
        {0x02005000u, 1u, thumb_entry, nullptr},
    };
    if (registry.register_bank(
            NDS_ARM9, same_address_modes, 2u, "same-address-modes") !=
        RegisterBankStatus::Ok) {
        return fail("same-address ARM/Thumb rows should be accepted in key order");
    }
    result = registry.lookup(NDS_ARM9, 0x02005000u, 0u);
    if (!result || result.selected->fn != arm9_entry_a) {
        return fail("ARM row for a shared address was not selected");
    }
    result = registry.lookup(NDS_ARM9, 0x02005001u, CPSR_T_BIT);
    if (!result || result.selected->fn != thumb_entry) {
        return fail("Thumb row for a shared address was not selected");
    }

    const NdsDispatchEntry unsorted_same_address[] = {
        {0x02006000u, 1u, thumb_entry, nullptr},
        {0x02006000u, 0u, arm9_entry_a, nullptr},
    };
    if (registry.register_bank(
            NDS_ARM9, unsorted_same_address, 2u, "unsorted-mode-key") !=
        RegisterBankStatus::UnsortedTable) {
        return fail("same-address rows with reversed mode key were accepted");
    }

    const NdsDispatchEntry duplicate_key[] = {
        {0x02007000u, 0u, arm9_entry_a, nullptr},
        {0x02007000u, 0u, arm9_entry_b, nullptr},
    };
    if (registry.register_bank(
            NDS_ARM9, duplicate_key, 2u, "duplicate-key") !=
        RegisterBankStatus::DuplicateEntryKey) {
        return fail("duplicate address/mode key within a bank was accepted");
    }

    const NdsDispatchEntry bad_fn[] = {
        {0x02003000u, 0u, nullptr, nullptr},
    };
    if (registry.register_bank(NDS_ARM9, bad_fn, 1u, "bad-fn") !=
        RegisterBankStatus::InvalidEntry) {
        return fail("null dispatch function was accepted");
    }

    const NdsDispatchEntry bad_thumb[] = {
        {0x02004000u, 2u, arm9_entry_a, nullptr},
    };
    if (registry.register_bank(NDS_ARM9, bad_thumb, 1u, "bad-thumb") !=
        RegisterBankStatus::InvalidEntry) {
        return fail("invalid Thumb marker was accepted");
    }

    const NdsDispatchEntry bad_align[] = {
        {0x02004002u, 0u, arm9_entry_a, nullptr},
    };
    if (registry.register_bank(NDS_ARM9, bad_align, 1u, "bad-align") !=
        RegisterBankStatus::InvalidEntry) {
        return fail("misaligned ARM entry was accepted");
    }

    if (registry.register_bank(NDS_ARM9, arm9_table, 3u, "arm9-main") !=
        RegisterBankStatus::DuplicateBankName) {
        return fail("duplicate bank name was accepted");
    }

    if (!registry.unregister_bank(NDS_ARM9, same_address_modes, 2u) ||
        registry.unregister_bank(NDS_ARM9, same_address_modes, 2u) ||
        registry.unregister_bank(static_cast<NdsCpu>(77), same_address_modes, 2u)) {
        return fail("dispatch bank unregister accepted invalid state");
    }
    if (registry.bank_count(NDS_ARM9) != 3u) {
        return fail("dispatch bank count did not decrease after unregister");
    }
    result = registry.lookup(NDS_ARM9, 0x02005000u, 0u);
    if (result.status != DispatchLookupStatus::Miss) {
        return fail("unregistered bank remained visible to lookup");
    }
    if (registry.register_bank(
            NDS_ARM9, same_address_modes, 2u, "same-address-modes") !=
        RegisterBankStatus::Ok) {
        return fail("bank name was not reusable after unregister");
    }

    registry.clear();
    if (registry.bank_count(NDS_ARM9) != 0u ||
        registry.bank_count(NDS_ARM7) != 0u ||
        registry.lookup(NDS_ARM9, 0x02000000u, 0u).status !=
            DispatchLookupStatus::CpuNotRegistered) {
        return fail("registry clear did not remove all banks");
    }

    std::puts("static ARM9/ARM7 dispatch registry tests passed");
    return 0;
}

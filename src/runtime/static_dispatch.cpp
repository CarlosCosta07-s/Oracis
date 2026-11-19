#include "oracis/recomp/static_dispatch.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace oracis::recomp {
namespace {

constexpr uint64_t kGuestAddressEnd = UINT64_C(0x1'0000'0000);

bool valid_range(uint32_t address, uint32_t size) noexcept
{
    if (size == 0u) return false;
    return static_cast<uint64_t>(address) + size <= kGuestAddressEnd;
}

bool validation_owns_entry(
    const NdsStaticValidation *validation,
    uint32_t pc,
    bool thumb) noexcept
{
    if (validation == nullptr || validation->expected == nullptr ||
        validation->size == 0u) {
        return false;
    }

    const uint32_t instruction_size = thumb ? 2u : 4u;
    const uint64_t begin = validation->addr;
    const uint64_t end = begin + validation->size;
    const uint64_t instruction_end =
        static_cast<uint64_t>(pc) + instruction_size;

    return end <= kGuestAddressEnd &&
           pc >= begin &&
           instruction_end <= end;
}

} // namespace

bool StaticDispatchRegistry::valid_cpu(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 || cpu == NDS_ARM7;
}

bool StaticDispatchRegistry::valid_validation(
    const NdsStaticValidation *validation,
    uint32_t entry_address,
    bool thumb) noexcept
{
    if (validation == nullptr) return true;

    if (validation->expected == nullptr ||
        !valid_range(validation->addr, validation->size) ||
        !validation_owns_entry(validation, entry_address, thumb)) {
        return false;
    }

    if (validation->dependency_count != 0u &&
        validation->dependencies == nullptr) {
        return false;
    }

    for (uint32_t index = 0u;
         index < validation->dependency_count;
         ++index) {
        const NdsStaticValidationRange &dependency =
            validation->dependencies[index];
        if (dependency.expected == nullptr ||
            !valid_range(dependency.addr, dependency.size)) {
            return false;
        }
    }

    return true;
}

uint32_t StaticDispatchRegistry::candidate_rank(
    const NdsDispatchEntry &entry) noexcept
{
    return entry.validation != nullptr ? entry.validation->size : 0u;
}

RegisterBankStatus StaticDispatchRegistry::register_bank(
    NdsCpu cpu,
    const NdsDispatchEntry *table,
    std::size_t length,
    std::string bank_name)
{
    if (!valid_cpu(cpu)) return RegisterBankStatus::InvalidCpu;
    if (table == nullptr || length == 0u || bank_name.empty() ||
        length > std::numeric_limits<uint32_t>::max()) {
        return RegisterBankStatus::InvalidTable;
    }

    std::vector<Bank> &banks =
        cpu == NDS_ARM9 ? arm9_banks_ : arm7_banks_;

    for (const Bank &bank : banks) {
        if (bank.name == bank_name) {
            return RegisterBankStatus::DuplicateBankName;
        }
    }

    uint32_t previous_address = 0u;
    uint8_t previous_thumb = 0u;
    bool have_previous = false;

    for (std::size_t index = 0u; index < length; ++index) {
        const NdsDispatchEntry &entry = table[index];
        if (entry.fn == nullptr || entry.thumb > 1u) {
            return RegisterBankStatus::InvalidEntry;
        }

        const bool thumb = entry.thumb != 0u;
        const uint32_t alignment = thumb ? 2u : 4u;
        if ((entry.addr % alignment) != 0u) {
            return RegisterBankStatus::InvalidEntry;
        }

        if (have_previous) {
            if (entry.addr < previous_address ||
                (entry.addr == previous_address &&
                 entry.thumb < previous_thumb)) {
                return RegisterBankStatus::UnsortedTable;
            }
            if (entry.addr == previous_address &&
                entry.thumb == previous_thumb) {
                return RegisterBankStatus::DuplicateEntryKey;
            }
        }
        previous_address = entry.addr;
        previous_thumb = entry.thumb;
        have_previous = true;

        if (!valid_validation(entry.validation, entry.addr, thumb)) {
            return RegisterBankStatus::InvalidValidation;
        }
    }

    try {
        banks.push_back(Bank{std::move(bank_name), table, length});
    } catch (...) {
        return RegisterBankStatus::InvalidTable;
    }
    return RegisterBankStatus::Ok;
}

DispatchLookupResult StaticDispatchRegistry::lookup(
    NdsCpu cpu,
    uint32_t target_pc,
    uint32_t cpsr,
    DispatchValidationFn validation_live,
    void *user) const noexcept
{
    DispatchLookupResult result{};
    if (!valid_cpu(cpu)) {
        result.status = DispatchLookupStatus::InvalidCpu;
        return result;
    }

    const std::vector<Bank> &banks =
        cpu == NDS_ARM9 ? arm9_banks_ : arm7_banks_;
    if (banks.empty()) {
        result.status = DispatchLookupStatus::CpuNotRegistered;
        return result;
    }

    const bool thumb = (cpsr & CPSR_T_BIT) != 0u;
    const uint32_t address = target_pc & ~UINT32_C(1);
    const uint32_t alignment = thumb ? 2u : 4u;
    if ((address % alignment) != 0u) {
        result.status = DispatchLookupStatus::MisalignedTarget;
        return result;
    }

    uint32_t best_rank = 0u;
    for (const Bank &bank : banks) {
        const NdsDispatchEntry *begin = bank.table;
        const NdsDispatchEntry *end = bank.table + bank.length;
        const NdsDispatchEntry *first = std::lower_bound(
            begin, end, address,
            [](const NdsDispatchEntry &entry, uint32_t wanted) {
                return entry.addr < wanted;
            });

        for (const NdsDispatchEntry *entry = first;
             entry != end && entry->addr == address;
             ++entry) {
            if ((entry->thumb != 0u) != thumb) continue;

            ++result.candidate_count;
            if (entry->validation != nullptr &&
                (validation_live == nullptr ||
                 !validation_live(entry->validation, address, thumb, user))) {
                result.inactive = entry;
                continue;
            }

            const uint32_t rank = candidate_rank(*entry);
            if (result.selected == nullptr || rank > best_rank) {
                result.selected = entry;
                best_rank = rank;
            }
            /* Equal rank deliberately keeps the first-registered row. */
        }
    }

    result.status = result.selected != nullptr
        ? DispatchLookupStatus::Found
        : DispatchLookupStatus::Miss;
    return result;
}

std::size_t StaticDispatchRegistry::bank_count(NdsCpu cpu) const noexcept
{
    if (!valid_cpu(cpu)) return 0u;
    return cpu == NDS_ARM9 ? arm9_banks_.size() : arm7_banks_.size();
}

const char *StaticDispatchRegistry::register_status_name(
    RegisterBankStatus status) noexcept
{
    switch (status) {
    case RegisterBankStatus::Ok: return "ok";
    case RegisterBankStatus::InvalidCpu: return "invalid CPU";
    case RegisterBankStatus::InvalidTable: return "invalid table";
    case RegisterBankStatus::InvalidEntry: return "invalid dispatch entry";
    case RegisterBankStatus::InvalidValidation: return "invalid validation metadata";
    case RegisterBankStatus::UnsortedTable: return "dispatch table is not sorted by address/mode key";
    case RegisterBankStatus::DuplicateEntryKey: return "duplicate dispatch address/mode key";
    case RegisterBankStatus::DuplicateBankName: return "duplicate bank name";
    }
    return "unknown registration status";
}

const char *StaticDispatchRegistry::lookup_status_name(
    DispatchLookupStatus status) noexcept
{
    switch (status) {
    case DispatchLookupStatus::Found: return "found";
    case DispatchLookupStatus::InvalidCpu: return "invalid CPU";
    case DispatchLookupStatus::CpuNotRegistered: return "CPU has no registered bank";
    case DispatchLookupStatus::Miss: return "static dispatch miss";
    case DispatchLookupStatus::MisalignedTarget: return "misaligned dispatch target";
    }
    return "unknown lookup status";
}

} // namespace oracis::recomp

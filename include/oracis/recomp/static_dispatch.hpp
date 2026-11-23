/*
 * Oracis static ARM/Thumb dispatch registry.
 *
 * This is an independent implementation of the generated-bank table
 * contract. It does not copy or call the NDSRecomp runner/runtime.
 *
 * The registry stores non-owning pointers to immutable generated tables.
 * The caller must keep every table and its validation metadata alive for the
 * lifetime of the registry. Banks are registered before execution starts.
 */
#ifndef ORACIS_RECOMP_STATIC_DISPATCH_HPP
#define ORACIS_RECOMP_STATIC_DISPATCH_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "oracis/recomp/runtime_arm.h"

namespace oracis::recomp {

enum class RegisterBankStatus : uint8_t {
    Ok = 0,
    InvalidCpu,
    InvalidTable,
    InvalidEntry,
    InvalidValidation,
    UnsortedTable,
    DuplicateEntryKey,
    DuplicateBankName,
    DuplicateBankTable
};

enum class DispatchLookupStatus : uint8_t {
    Found = 0,
    InvalidCpu,
    CpuNotRegistered,
    Miss,
    MisalignedTarget
};

using DispatchValidationFn = bool (*)(
    NdsCpu cpu,
    const NdsStaticValidation *validation,
    uint32_t pc,
    bool thumb,
    void *user);

struct DispatchLookupResult {
    DispatchLookupStatus status = DispatchLookupStatus::Miss;
    const NdsDispatchEntry *selected = nullptr;
    const NdsDispatchEntry *inactive = nullptr;
    uint32_t candidate_count = 0u;

    explicit operator bool() const noexcept {
        return status == DispatchLookupStatus::Found && selected != nullptr;
    }
};

class StaticDispatchRegistry final {
public:
    StaticDispatchRegistry() = default;

    StaticDispatchRegistry(const StaticDispatchRegistry &) = delete;
    StaticDispatchRegistry &operator=(const StaticDispatchRegistry &) = delete;
    StaticDispatchRegistry(StaticDispatchRegistry &&) noexcept = default;
    StaticDispatchRegistry &operator=(StaticDispatchRegistry &&) noexcept = default;

    /*
     * Register a sorted generated table for one CPU. Multiple banks may
     * contain the same (address, mode) key; lookup ranks live candidates by
     * validation span, with first registration winning ties.
     *
     * Table and validation pointers are non-owning and must remain immutable
     * and alive until the registry is destroyed.
     */
    RegisterBankStatus register_bank(
        NdsCpu cpu,
        const NdsDispatchEntry *table,
        std::size_t length,
        std::string bank_name);

    /*
     * Remove exactly the registered table identified by its original pointer
     * and length. The caller must ensure no generated function from that bank
     * is currently executing before unregistering it.
     */
    bool unregister_bank(
        NdsCpu cpu,
        const NdsDispatchEntry *table,
        std::size_t length) noexcept;

    void clear() noexcept;

    /* NdsLinkSlot.key format: (dispatch_epoch << 1) | cpu. Zero disables
       cached links and is reserved for unresolved slots. */
    uint32_t link_epoch_key(NdsCpu cpu) const noexcept;
    void invalidate_link_epoch() noexcept;

    /*
     * Look up the target address with ARM/Thumb state taken from CPSR.T.
     * The low bit of target_pc is stripped like the generated-bank contract;
     * CPSR.T is authoritative for selecting the table row.
     *
     * A row with validation metadata is inactive unless validation_live is
     * supplied and accepts it. An immutable row with null validation is
     * eligible without a callback and has rank zero.
     */
    DispatchLookupResult lookup(
        NdsCpu cpu,
        uint32_t target_pc,
        uint32_t cpsr,
        DispatchValidationFn validation_live = nullptr,
        void *user = nullptr) const noexcept;

    std::size_t bank_count(NdsCpu cpu) const noexcept;
    static const char *register_status_name(RegisterBankStatus status) noexcept;
    static const char *lookup_status_name(DispatchLookupStatus status) noexcept;

private:
    struct Bank {
        std::string name;
        const NdsDispatchEntry *table = nullptr;
        std::size_t length = 0u;
    };

    static bool valid_cpu(NdsCpu cpu) noexcept;
    static bool valid_validation(
        const NdsStaticValidation *validation,
        uint32_t entry_address,
        bool thumb) noexcept;
    static uint32_t candidate_rank(const NdsDispatchEntry &entry) noexcept;
    void advance_link_epoch() noexcept;

    uint32_t link_epoch_ = 1u;
    std::vector<Bank> arm9_banks_;
    std::vector<Bank> arm7_banks_;
};


/* Shared registry used by the machine-facing C ABI. */
StaticDispatchRegistry &global_static_dispatch_registry() noexcept;

} // namespace oracis::recomp

#endif /* ORACIS_RECOMP_STATIC_DISPATCH_HPP */

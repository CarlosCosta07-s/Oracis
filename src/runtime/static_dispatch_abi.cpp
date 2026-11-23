#include "oracis/recomp/static_dispatch_abi.h"
#include "oracis/recomp/static_dispatch.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

oracis::recomp::StaticDispatchRegistry &registry() noexcept
{
    return oracis::recomp::global_static_dispatch_registry();
}

OracisDispatchValidationFn g_validation_callback = nullptr;
void *g_validation_user = nullptr;

struct ValidationContext {
    OracisDispatchValidationFn callback;
    void *user;
};

bool validate_adapter(NdsCpu cpu,
                      const NdsStaticValidation *validation,
                      uint32_t pc,
                      bool thumb,
                      void *opaque)
{
    const auto *context = static_cast<const ValidationContext *>(opaque);
    if (context == nullptr || context->callback == nullptr) return false;
    return context->callback(
        cpu, validation, pc, thumb ? 1u : 0u, context->user) != 0;
}

[[noreturn]] void dispatch_failure(
    const char *kind,
    NdsCpu cpu,
    uint32_t target_pc,
    uint32_t cpsr,
    uint32_t status,
    uint32_t candidate_count,
    const NdsDispatchEntry *inactive)
{
    std::fprintf(stderr,
        "[Oracis dispatch] %s: CPU=%u target=0x%08X PC=0x%08X "
        "CPSR=0x%08X status=%u candidates=%u inactive=%s\n",
        kind,
        static_cast<unsigned>(cpu),
        target_pc,
        oracis_active_cpu_state ? oracis_active_cpu_state->R[15] : 0u,
        cpsr,
        status,
        candidate_count,
        inactive ? "yes" : "no");
    std::abort();
}

} // namespace

extern "C" int oracis_static_dispatch_register_bank(
    NdsCpu cpu,
    const NdsDispatchEntry *table,
    uint32_t length,
    const char *bank_name)
{
    if (bank_name == nullptr) {
        return static_cast<int>(
            oracis::recomp::RegisterBankStatus::InvalidTable);
    }
    try {
        const auto result = registry().register_bank(
            cpu, table, length, std::string(bank_name));
        return static_cast<int>(result);
    } catch (...) {
        return static_cast<int>(
            oracis::recomp::RegisterBankStatus::InvalidTable);
    }
}

extern "C" int oracis_static_dispatch_unregister_bank(
    NdsCpu cpu,
    const NdsDispatchEntry *table,
    uint32_t length)
{
    return registry().unregister_bank(cpu, table, length) ? 1 : 0;
}

extern "C" void oracis_static_dispatch_clear(void)
{
    /* Clearing bank rows does not change the lifetime of the memory validator.
       The owner removes that callback explicitly when it unbinds GuestMemory. */
    registry().clear();
}

extern "C" void oracis_static_dispatch_set_validation_callback(
    OracisDispatchValidationFn callback,
    void *user)
{
    if (g_validation_callback != callback ||
        g_validation_user != user) {
        g_validation_callback = callback;
        g_validation_user = user;
        registry().invalidate_link_epoch();
    }
}

extern "C" const NdsDispatchEntry *oracis_static_dispatch_lookup(
    NdsCpu cpu,
    uint32_t target_pc,
    uint32_t cpsr,
    uint32_t *status,
    uint32_t *candidate_count,
    const NdsDispatchEntry **inactive_candidate)
{
    ValidationContext context{g_validation_callback, g_validation_user};
    const auto result = registry().lookup(
        cpu,
        target_pc,
        cpsr,
        context.callback != nullptr ? validate_adapter : nullptr,
        &context);

    if (status != nullptr) {
        *status = static_cast<uint32_t>(result.status);
    }
    if (candidate_count != nullptr) {
        *candidate_count = result.candidate_count;
    }
    if (inactive_candidate != nullptr) {
        *inactive_candidate = result.inactive;
    }
    return result.selected;
}

extern "C" void runtime_dispatch(uint32_t target_pc)
{
    if (oracis_active_cpu_state == nullptr) {
        dispatch_failure("no active CPU state", g_nds_active, target_pc,
                         0u, 0u, 0u, nullptr);
    }

    uint32_t status = 0u;
    uint32_t candidate_count = 0u;
    const NdsDispatchEntry *inactive = nullptr;
    const NdsDispatchEntry *entry = oracis_static_dispatch_lookup(
        g_nds_active,
        target_pc,
        oracis_active_cpu_state->cpsr,
        &status,
        &candidate_count,
        &inactive);

    if (entry == nullptr) {
        dispatch_failure(
            "static dispatch miss",
            g_nds_active,
            target_pc,
            oracis_active_cpu_state->cpsr,
            status,
            candidate_count,
            inactive);
    }

    entry->fn();
}

extern "C" void runtime_dispatch_literal_branch(uint32_t target_pc)
{
    runtime_dispatch(target_pc);
}

extern "C" void runtime_dispatch_literal_call(uint32_t target_pc)
{
    runtime_dispatch(target_pc);
}

extern "C" void runtime_dispatch_literal_fallthrough(uint32_t target_pc)
{
    runtime_dispatch(target_pc);
}

extern "C" void runtime_dispatch_with_exchange(uint32_t target_pc)
{
    if (oracis_active_cpu_state == nullptr) {
        dispatch_failure("exchange without active CPU state", g_nds_active,
                         target_pc, 0u, 0u, 0u, nullptr);
    }

    if ((target_pc & 1u) != 0u) {
        oracis_active_cpu_state->cpsr |= CPSR_T_BIT;
    } else {
        oracis_active_cpu_state->cpsr &= ~CPSR_T_BIT;
    }
    runtime_dispatch(target_pc);
}

/*
 * Direct-link slots are an ABI optimization. Until Oracis adds a resolved
 * slot cache with generation/content guards, these entry points deliberately
 * remain unresolved and route through the ordinary static dispatcher.
 *
 * The ABI stores the target execution state in target_pc bit zero. Preserve
 * that bit through the normal exchange path; stripping it before dispatch
 * would incorrectly reuse the caller's CPSR.T state for ARM/Thumb transfers.
 */
extern "C" void runtime_link_call(NdsLinkSlot *slot)
{
    if (slot == nullptr) {
        dispatch_failure("null call link slot", g_nds_active, 0u,
                         oracis_active_cpu_state
                             ? oracis_active_cpu_state->cpsr : 0u,
                         0u, 0u, nullptr);
    }
    runtime_dispatch_with_exchange(slot->target_pc);
}

extern "C" void runtime_link_branch(NdsLinkSlot *slot)
{
    if (slot == nullptr) {
        dispatch_failure("null branch link slot", g_nds_active, 0u,
                         oracis_active_cpu_state
                             ? oracis_active_cpu_state->cpsr : 0u,
                         0u, 0u, nullptr);
    }
    runtime_dispatch_with_exchange(slot->target_pc);
}

extern "C" void runtime_link_fallthrough(NdsLinkSlot *slot)
{
    if (slot == nullptr) {
        dispatch_failure("null fallthrough link slot", g_nds_active, 0u,
                         oracis_active_cpu_state
                             ? oracis_active_cpu_state->cpsr : 0u,
                         0u, 0u, nullptr);
    }
    runtime_dispatch_with_exchange(slot->target_pc);
}

extern "C" int runtime_inline_leaf_admit(
    const NdsLinkSlot *slot,
    void (*expected)(void))
{
    (void)slot;
    (void)expected;
    /* No direct-link cache exists yet, so inline copies are never admitted. */
    return 0;
}

extern "C" void runtime_dispatch_miss(uint32_t target_pc)
{
    const uint32_t cpsr = oracis_active_cpu_state
        ? oracis_active_cpu_state->cpsr : 0u;
    dispatch_failure("explicit static dispatch miss", g_nds_active,
                     target_pc, cpsr,
                     static_cast<uint32_t>(
                         oracis::recomp::DispatchLookupStatus::Miss),
                     0u, nullptr);
}

extern "C" void runtime_dispatch_bad_entry(uint32_t target_pc)
{
    const uint32_t cpsr = oracis_active_cpu_state
        ? oracis_active_cpu_state->cpsr : 0u;
    dispatch_failure("invalid generated dispatch entry", g_nds_active,
                     target_pc, cpsr,
                     static_cast<uint32_t>(
                         oracis::recomp::DispatchLookupStatus::Miss),
                     0u, nullptr);
}

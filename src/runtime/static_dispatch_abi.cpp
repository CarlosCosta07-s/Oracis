#include "oracis/recomp/static_dispatch_abi.h"
#include "oracis/recomp/static_dispatch.hpp"
#include "oracis/recomp/runtime_scheduler.h"

#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace {

oracis::recomp::StaticDispatchRegistry &registry() noexcept
{
    return oracis::recomp::global_static_dispatch_registry();
}

OracisDispatchValidationFn g_validation_callback = nullptr;
void *g_validation_user = nullptr;
OracisDispatchMissHandler g_dispatch_miss_handler = nullptr;
void *g_dispatch_miss_user = nullptr;

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

bool recover_dispatch_miss(
    NdsCpu cpu,
    uint32_t target_pc,
    uint32_t cpsr,
    uint32_t status,
    uint32_t candidate_count,
    const NdsDispatchEntry *inactive) noexcept
{
    if (g_dispatch_miss_handler == nullptr) return false;
    int handled = 0;
    try {
        handled = g_dispatch_miss_handler(
            g_dispatch_miss_user,
            cpu,
            target_pc,
            cpsr,
            status,
            candidate_count,
            inactive != nullptr ? 1u : 0u);
    } catch (...) {
        handled = 0;
    }
    if (handled == 0) return false;
    if (!oracis_scheduler_set_unwinding(cpu, 1)) {
        dispatch_failure(
            "dispatch miss handler could not request guest unwind",
            cpu,
            target_pc,
            cpsr,
            status,
            candidate_count,
            inactive);
    }
    return true;
}

bool deliver_prefetch_abort(uint32_t target_pc)
{
    const int result = oracis_arm_runtime_try_prefetch_abort(target_pc);
    if (result < 0 || result > 1) {
        dispatch_failure(
            "Prefetch Abort callback integration failure",
            g_nds_active,
            target_pc,
            oracis_active_cpu_state
                ? oracis_active_cpu_state->cpsr : 0u,
            static_cast<uint32_t>(result),
            0u,
            nullptr);
    }
    if (result == 0) return false;

    if (!oracis_scheduler_set_unwinding(g_nds_active, 1)) {
        dispatch_failure(
            "Prefetch Abort could not request generated-code unwind",
            g_nds_active,
            target_pc,
            oracis_active_cpu_state
                ? oracis_active_cpu_state->cpsr : 0u,
            0u,
            0u,
            nullptr);
    }
    return true;
}

/*
 * The guard is runner-owned and intentionally opaque to generated code.
 * It snapshots the exact validation row/function under one global link key.
 * GuestMemory's global mutation epoch makes the snapshot stale after any
 * write or map/permission change, without dereferencing a foreign guard.
 */
struct OracisLinkGuard {
    uint32_t key = 0u;
    uint32_t target_pc = 0u;
    NdsCpu cpu = NDS_ARM9;
    const NdsStaticValidation *validation = nullptr;
    void (*fn)(void) = nullptr;
};

struct OracisLinkSlotCache {
    uint32_t key = 0u;
    uint32_t target_pc = 0u;
    NdsCpu cpu = NDS_ARM9;
    const NdsStaticValidation *validation = nullptr;
    void (*fn)(void) = nullptr;
    OracisLinkGuard guard{};
    bool resolved = false;
};

/*
 * Generated slots retain guard values across epoch changes. Use one
 * process-lifetime marker for mutable rows instead of exposing the address of
 * a cache-owned object that may be destroyed when records are reclaimed.
 * The actual per-callsite guard snapshot stays private in the map entry.
 */
const unsigned char g_mutable_link_guard_token = 0u;

/*
 * Slot addresses are opaque callsite keys; the cache does not own slot storage.
 * unordered_map preserves references/pointers to elements across rehash, while
 * the externally visible guard marker has process lifetime and never dangles.
 * Registry/bank and epoch lifecycle hooks reclaim private records; a marker in
 * a stale slot is accepted only when a matching, current map entry exists.
 */
std::unordered_map<const NdsLinkSlot *, OracisLinkSlotCache> g_link_slot_cache;
uint32_t g_link_slot_cache_epoch = 0u;

const void *mutable_link_guard_token() noexcept
{
    return static_cast<const void *>(&g_mutable_link_guard_token);
}

/*
 * Cache records own guard tokens referenced by generated NdsLinkSlots. Drop
 * every record when the registry topology or memory epoch changes, and when
 * banks are explicitly removed, so repeated machine sessions cannot retain
 * orphaned callsite records indefinitely.
 */
void clear_link_slot_cache() noexcept
{
    g_link_slot_cache.clear();
    g_link_slot_cache_epoch = 0u;
}

void synchronize_link_slot_cache_epoch(uint32_t key) noexcept
{
    const uint32_t epoch = key >> 1u;
    if (key == 0u || epoch == 0u || epoch != g_link_slot_cache_epoch) {
        g_link_slot_cache.clear();
        g_link_slot_cache_epoch = key != 0u ? epoch : 0u;
    }
}

bool link_slot_cache_hit(
    const NdsLinkSlot *slot,
    NdsCpu cpu,
    uint32_t key,
    const OracisLinkSlotCache **cache_out) noexcept
{
    if (slot == nullptr || key == 0u || slot->key != key ||
        slot->fn == nullptr) {
        return false;
    }

    const auto found = g_link_slot_cache.find(slot);
    if (found == g_link_slot_cache.end()) return false;

    const OracisLinkSlotCache &cache = found->second;
    if (!cache.resolved || cache.key != key || cache.cpu != cpu ||
        cache.target_pc != slot->target_pc || cache.fn != slot->fn ||
        cache.fn == nullptr) {
        return false;
    }

    if (cache.validation != nullptr) {
        if (slot->guard != mutable_link_guard_token() ||
            cache.guard.key != key ||
            cache.guard.target_pc != cache.target_pc ||
            cache.guard.cpu != cpu ||
            cache.guard.validation != cache.validation ||
            cache.guard.fn != cache.fn) {
            return false;
        }
    } else if (slot->guard != nullptr) {
        return false;
    }

    if (cache_out != nullptr) *cache_out = &cache;
    return true;
}

void select_link_target_state(uint32_t target_pc) noexcept
{
    if ((target_pc & 1u) != 0u) {
        oracis_active_cpu_state->cpsr |= CPSR_T_BIT;
    } else {
        oracis_active_cpu_state->cpsr &= ~CPSR_T_BIT;
    }
}

void dispatch_link_slot(NdsLinkSlot *slot, const char *kind)
{
    if (slot == nullptr) {
        dispatch_failure(kind, g_nds_active, 0u,
                         oracis_active_cpu_state
                             ? oracis_active_cpu_state->cpsr : 0u,
                         0u, 0u, nullptr);
    }
    if (oracis_active_cpu_state == nullptr) {
        dispatch_failure("link without active CPU state", g_nds_active,
                         slot->target_pc, 0u, 0u, 0u, nullptr);
    }

    const NdsCpu cpu = g_nds_active;
    const uint32_t target_pc = slot->target_pc;
    const uint32_t key = registry().link_epoch_key(cpu);
    synchronize_link_slot_cache_epoch(key);
    const OracisLinkSlotCache *cached = nullptr;
    if (link_slot_cache_hit(slot, cpu, key, &cached)) {
        select_link_target_state(target_pc);
        void (*const body)(void) = cached->fn;
        body();
        return;
    }

    /* Never expose an old function/guard while resolving a new epoch. */
    slot->fn = nullptr;
    slot->key = 0u;
    slot->guard = nullptr;

    select_link_target_state(target_pc);
    uint32_t status = 0u;
    uint32_t candidate_count = 0u;
    const NdsDispatchEntry *inactive = nullptr;
    const NdsDispatchEntry *entry = oracis_static_dispatch_lookup(
        cpu, target_pc, oracis_active_cpu_state->cpsr,
        &status, &candidate_count, &inactive);
    if (entry == nullptr) {
        const uint32_t cpsr = oracis_active_cpu_state->cpsr;
        if (deliver_prefetch_abort(target_pc)) return;
        if (recover_dispatch_miss(
                cpu, target_pc, cpsr, status, candidate_count, inactive)) {
            return;
        }
        dispatch_failure("static link dispatch miss", cpu, target_pc,
                         cpsr, status, candidate_count, inactive);
    }

    if (key != 0u) {
        try {
            auto inserted = g_link_slot_cache.try_emplace(slot);
            OracisLinkSlotCache &cache = inserted.first->second;
            cache.key = key;
            cache.target_pc = target_pc;
            cache.cpu = cpu;
            cache.validation = entry->validation;
            cache.fn = entry->fn;
            cache.resolved = true;
            cache.guard.key = key;
            cache.guard.target_pc = target_pc;
            cache.guard.cpu = cpu;
            cache.guard.validation = entry->validation;
            cache.guard.fn = entry->fn;

            slot->fn = entry->fn;
            slot->key = key;
            slot->guard = entry->validation != nullptr
                ? mutable_link_guard_token()
                : nullptr;
        } catch (...) {
            /* Allocation failure is a safe cache miss, never a dispatch miss. */
            slot->fn = nullptr;
            slot->key = 0u;
            slot->guard = nullptr;
        }
    }

    entry->fn();
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
        if (result == oracis::recomp::RegisterBankStatus::Ok) {
            clear_link_slot_cache();
        }
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
    const bool removed = registry().unregister_bank(cpu, table, length);
    if (removed) clear_link_slot_cache();
    return removed ? 1 : 0;
}

extern "C" void oracis_static_dispatch_clear(void)
{
    /* Clearing bank rows does not change the lifetime of the memory validator.
       The owner removes that callback explicitly when it unbinds GuestMemory. */
    registry().clear();
    clear_link_slot_cache();
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
        clear_link_slot_cache();
    }
}

extern "C" void oracis_static_dispatch_set_memory_generation_provider(
    OracisDispatchMemoryGenerationFn provider,
    void *user)
{
    registry().set_memory_generation_provider(provider, user);
    clear_link_slot_cache();
}

extern "C" void oracis_static_dispatch_set_miss_handler(
    OracisDispatchMissHandler handler,
    void *user)
{
    g_dispatch_miss_handler = handler;
    g_dispatch_miss_user = handler != nullptr ? user : nullptr;
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
        const uint32_t cpsr = oracis_active_cpu_state->cpsr;
        if (deliver_prefetch_abort(target_pc)) return;
        if (recover_dispatch_miss(
                g_nds_active, target_pc, cpsr,
                status, candidate_count, inactive)) {
            return;
        }
        dispatch_failure(
            "static dispatch miss",
            g_nds_active,
            target_pc,
            cpsr,
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
 * Link slots are a per-callsite cache. A nonzero key is accepted only when
 * the live registry/memory epoch, CPU, target PC/ISA, function pointer and
 * (for mutable rows) Oracis-owned content guard all match the snapshot.
 * Failure always falls back to the ordinary validated table lookup.
 */
extern "C" void runtime_link_call(NdsLinkSlot *slot)
{
    dispatch_link_slot(slot, "null call link slot");
}

extern "C" void runtime_link_branch(NdsLinkSlot *slot)
{
    dispatch_link_slot(slot, "null branch link slot");
}

extern "C" void runtime_link_fallthrough(NdsLinkSlot *slot)
{
    dispatch_link_slot(slot, "null fallthrough link slot");
}

extern "C" int runtime_inline_leaf_admit(
    const NdsLinkSlot *slot,
    void (*expected)(void))
{
    if (slot == nullptr || expected == nullptr ||
        oracis_active_cpu_state == nullptr) {
        return 0;
    }

    const NdsCpu cpu = g_nds_active;
    const uint32_t key = registry().link_epoch_key(cpu);
    synchronize_link_slot_cache_epoch(key);
    const OracisLinkSlotCache *cached = nullptr;
    if (!link_slot_cache_hit(slot, cpu, key, &cached)) return 0;

    /* Inline code is compiled for one ISA state; never admit across BX/BLX. */
    const bool slot_thumb = (slot->target_pc & 1u) != 0u;
    const bool active_thumb =
        (oracis_active_cpu_state->cpsr & CPSR_T_BIT) != 0u;
    if (slot_thumb != active_thumb) return 0;

    return cached->fn == expected ? 1 : 0;
}

extern "C" void runtime_dispatch_miss(uint32_t target_pc)
{
    const uint32_t cpsr = oracis_active_cpu_state
        ? oracis_active_cpu_state->cpsr : 0u;
    if (deliver_prefetch_abort(target_pc)) return;
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

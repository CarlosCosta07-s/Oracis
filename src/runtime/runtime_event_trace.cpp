#include "oracis/recomp/runtime_arm.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {

struct CpuEventRing {
    std::array<OracisRuntimeEvent, ORACIS_RUNTIME_EVENT_CAPACITY> events{};
    uint64_t oldest_sequence = 1u;
    uint64_t next_sequence = 1u;
    uint64_t dropped = 0u;
};

std::array<CpuEventRing, 2> g_event_rings{};

bool valid_cpu(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 || cpu == NDS_ARM7;
}

CpuEventRing *ring_for(NdsCpu cpu) noexcept
{
    if (!valid_cpu(cpu)) return nullptr;
    return &g_event_rings[static_cast<std::size_t>(cpu)];
}

[[noreturn]] void trace_failure(const char *operation)
{
    std::fprintf(stderr,
        "[Oracis runtime trace] %s failed (CPU=%u, PC=0x%08X)\n",
        operation,
        static_cast<unsigned>(g_nds_active),
        oracis_active_cpu_state ? oracis_active_cpu_state->R[15] : 0u);
    std::abort();
}

void append_event(
    NdsCpu cpu,
    uint32_t kind,
    uint32_t pc,
    uint32_t addr,
    uint32_t value,
    uint32_t aux)
{
    CpuEventRing *ring = ring_for(cpu);
    if (ring == nullptr) trace_failure("append event with invalid CPU");
    if (ring->next_sequence == std::numeric_limits<uint64_t>::max()) {
        trace_failure("event sequence overflow");
    }

    const uint64_t sequence = ring->next_sequence++;
    if (sequence - ring->oldest_sequence >=
        static_cast<uint64_t>(ORACIS_RUNTIME_EVENT_CAPACITY)) {
        ++ring->oldest_sequence;
        ++ring->dropped;
    }

    OracisRuntimeEvent event{};
    event.sequence = sequence;
    event.kind = kind;
    event.pc = pc;
    event.addr = addr;
    event.value = value;
    event.aux = aux;
    ring->events[static_cast<std::size_t>(
        (sequence - 1u) % ORACIS_RUNTIME_EVENT_CAPACITY)] = event;
}

} // namespace

extern "C" {

uint32_t oracis_runtime_event_available(NdsCpu cpu)
{
    const CpuEventRing *ring = ring_for(cpu);
    if (ring == nullptr) return 0u;
    const uint64_t count = ring->next_sequence - ring->oldest_sequence;
    return count > ORACIS_RUNTIME_EVENT_CAPACITY
        ? ORACIS_RUNTIME_EVENT_CAPACITY
        : static_cast<uint32_t>(count);
}

uint64_t oracis_runtime_event_dropped(NdsCpu cpu)
{
    const CpuEventRing *ring = ring_for(cpu);
    return ring != nullptr ? ring->dropped : 0u;
}

uint32_t oracis_runtime_event_read(
    NdsCpu cpu,
    uint64_t *next_sequence,
    OracisRuntimeEvent *events,
    uint32_t capacity)
{
    CpuEventRing *ring = ring_for(cpu);
    if (ring == nullptr || next_sequence == nullptr ||
        (capacity != 0u && events == nullptr)) {
        return 0u;
    }

    uint64_t cursor = *next_sequence;
    if (cursor == 0u || cursor < ring->oldest_sequence) {
        cursor = ring->oldest_sequence;
    }
    if (cursor > ring->next_sequence) cursor = ring->next_sequence;

    uint32_t copied = 0u;
    while (copied < capacity && cursor < ring->next_sequence) {
        const OracisRuntimeEvent &event = ring->events[
            static_cast<std::size_t>(
                (cursor - 1u) % ORACIS_RUNTIME_EVENT_CAPACITY)];
        if (event.sequence != cursor) {
            /* Defensive recovery if a host reads while a producer advances. */
            cursor = ring->oldest_sequence;
            continue;
        }
        events[copied++] = event;
        ++cursor;
    }

    *next_sequence = cursor;
    return copied;
}

int oracis_runtime_event_clear(NdsCpu cpu)
{
    CpuEventRing *ring = ring_for(cpu);
    if (ring == nullptr) return 0;
    *ring = CpuEventRing{};
    return 1;
}

void runtime_trace_event(
    uint32_t kind,
    uint32_t pc,
    uint32_t addr,
    uint32_t value,
    uint32_t aux)
{
    if (oracis_active_cpu_state == nullptr ||
        !valid_cpu(g_nds_active)) {
        trace_failure("runtime_trace_event without an active CPU");
    }
    if (kind < RUNTIME_TRACE_DISPATCH ||
        kind > RUNTIME_TRACE_MEM_READ) {
        trace_failure("runtime_trace_event with an unknown event kind");
    }

    append_event(g_nds_active, kind, pc, addr, value, aux);
}

void runtime_live_transfer(
    uint32_t source_pc,
    uint32_t target_pc,
    uint32_t transfer_type)
{
    if (oracis_active_cpu_state == nullptr ||
        !valid_cpu(g_nds_active)) {
        trace_failure("runtime_live_transfer without an active CPU");
    }
    if (transfer_type < NDS_LIVE_TRANSFER_B ||
        transfer_type > NDS_LIVE_TRANSFER_LDM_PC) {
        trace_failure("runtime_live_transfer with an unknown transfer type");
    }

    const uint32_t target_thumb =
        (g_cpu.cpsr & CPSR_T_BIT) != 0u ? 1u : 0u;
    append_event(
        g_nds_active,
        RUNTIME_TRACE_LIVE_TRANSFER,
        source_pc,
        target_pc,
        transfer_type,
        target_thumb);
}

} // extern "C"

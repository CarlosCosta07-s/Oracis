#include "oracis/memory/nds_external_memory_control.hpp"
namespace oracis::memory {
bool NdsExternalMemoryControl::valid_cpu(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 || cpu == NDS_ARM7;
}
unsigned NdsExternalMemoryControl::cpu_index(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 ? 0u : 1u;
}
ReadResult NdsExternalMemoryControl::read(
    NdsCpu cpu, uint32_t address, uint32_t width) const noexcept
{
    if (address != Address) return {Status::Unmapped, 0u};
    if (!valid_cpu(cpu)) return {Status::InvalidCpu, 0u};
    if (width != 2u) return {Status::InvalidWidth, 0u};
    const unsigned index = cpu_index(cpu);
    return {Status::Ok, static_cast<uint32_t>(
        (local_timing_[index] & GbaTimingMask) | shared_control_)};
}
Status NdsExternalMemoryControl::write(
    NdsCpu cpu, uint32_t address, uint32_t width, uint32_t value) noexcept
{
    if (address != Address) return Status::Unmapped;
    if (!valid_cpu(cpu)) return Status::InvalidCpu;
    if (width != 2u) return Status::InvalidWidth;
    local_timing_[cpu_index(cpu)] =
        static_cast<uint16_t>(value) & GbaTimingMask;
    /* Shared access-right and bus-mode fields are ARM9-owned. */
    if (cpu == NDS_ARM9) {
        shared_control_ = static_cast<uint16_t>(value) & SharedControlMask;
    }
    return Status::Ok;
}
} // namespace oracis::memory

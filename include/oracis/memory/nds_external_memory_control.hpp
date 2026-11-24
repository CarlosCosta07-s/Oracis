/*
 * Nintendo DS external-memory control/status register.
 * ARM9 owns the shared access-right and main-memory fields; each CPU retains
 * its own GBA-slot timing fields.
 */
#ifndef ORACIS_MEMORY_NDS_EXTERNAL_MEMORY_CONTROL_HPP
#define ORACIS_MEMORY_NDS_EXTERNAL_MEMORY_CONTROL_HPP
#include <array>
#include <cstdint>
#include "oracis/memory/guest_memory.hpp"
namespace oracis::memory {
class NdsExternalMemoryControl final {
public:
    static constexpr uint32_t Address = UINT32_C(0x04000204);
    static constexpr uint16_t GbaTimingMask = UINT16_C(0x007F);
    static constexpr uint16_t GbaAccessArm7Mask = UINT16_C(1) << 7u;
    static constexpr uint16_t GameCardAccessArm7Mask = UINT16_C(1) << 11u;
    static constexpr uint16_t MainMemorySynchronousMask = UINT16_C(1) << 14u;
    static constexpr uint16_t Arm7MainMemoryPriorityMask = UINT16_C(1) << 15u;
    static constexpr uint16_t SharedControlMask =
        GbaAccessArm7Mask | GameCardAccessArm7Mask |
        MainMemorySynchronousMask | Arm7MainMemoryPriorityMask;
    NdsExternalMemoryControl() noexcept = default;
    NdsExternalMemoryControl(const NdsExternalMemoryControl &) = delete;
    NdsExternalMemoryControl &operator=(const NdsExternalMemoryControl &) = delete;
    ReadResult read(NdsCpu cpu, uint32_t address, uint32_t width) const noexcept;
    Status write(NdsCpu cpu, uint32_t address, uint32_t width, uint32_t value) noexcept;
private:
    static bool valid_cpu(NdsCpu cpu) noexcept;
    static unsigned cpu_index(NdsCpu cpu) noexcept;
    std::array<uint16_t, 2> local_timing_{};
    uint16_t shared_control_ = 0u;
};
} // namespace oracis::memory
#endif /* ORACIS_MEMORY_NDS_EXTERNAL_MEMORY_CONTROL_HPP */

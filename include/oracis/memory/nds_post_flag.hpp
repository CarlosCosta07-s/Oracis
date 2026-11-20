/*
 * Nintendo DS POSTFLG register and BIOS CustomPost service.
 *
 * POSTFLG is visible to both CPUs for reads. Only ARM9 may write it directly;
 * bit 0 is a sticky post-boot flag, bit 1 is ARM9 read/write, and upper bits
 * read as zero. BIOS CustomPost supplies a 32-bit argument in R0.
 */
#ifndef ORACIS_MEMORY_NDS_POST_FLAG_HPP
#define ORACIS_MEMORY_NDS_POST_FLAG_HPP

#include <cstdint>

#include "oracis/memory/guest_memory.hpp"

namespace oracis::memory {

class NdsPostFlag final {
public:
    static constexpr uint32_t Address = UINT32_C(0x04000300);
    static constexpr uint8_t PostBootMask = UINT8_C(1u << 0u);
    static constexpr uint8_t Arm9ControlMask = UINT8_C(1u << 1u);
    static constexpr uint8_t ImplementedMask =
        static_cast<uint8_t>(PostBootMask | Arm9ControlMask);

    Status install() noexcept;
    ReadResult read(NdsCpu cpu, uint32_t address, uint32_t width) const noexcept;
    Status write(NdsCpu cpu, uint32_t address, uint32_t width,
                 uint32_t value) noexcept;
    Status write_from_bios(NdsCpu cpu, uint32_t value) noexcept;

    bool installed() const noexcept { return installed_; }
    uint8_t value() const noexcept { return value_; }

private:
    static bool valid_cpu(NdsCpu cpu) noexcept;
    Status apply_arm9_value(uint32_t value) noexcept;

    uint8_t value_ = 0u;
    bool installed_ = false;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_POST_FLAG_HPP */

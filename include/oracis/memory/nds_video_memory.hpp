/*
 * Nintendo DS fixed palette and OAM memory.
 *
 * Maps the standard palette RAM and object-attribute memory used by the
 * two 2D display engines. The actual 2D renderer and scanline arbitration
 * are separate devices and are not modeled here.
 */
#ifndef ORACIS_MEMORY_NDS_VIDEO_MEMORY_HPP
#define ORACIS_MEMORY_NDS_VIDEO_MEMORY_HPP

#include <cstdint>

#include "oracis/memory/guest_memory.hpp"

namespace oracis::memory {

class NdsVideoMemory final {
public:
    static constexpr uint32_t PaletteBase = UINT32_C(0x05000000);
    static constexpr uint32_t PaletteSize = UINT32_C(0x00000800);
    static constexpr uint32_t OamBase = UINT32_C(0x07000000);
    static constexpr uint32_t OamSize = UINT32_C(0x00000800);

    explicit NdsVideoMemory(GuestMemory &memory) noexcept
        : memory_(memory) {}

    NdsVideoMemory(const NdsVideoMemory &) = delete;
    NdsVideoMemory &operator=(const NdsVideoMemory &) = delete;

    Status install();

    /*
     * Intercepts CPU stores to fixed palette/OAM memory. Byte stores are
     * ignored by the NDS video-memory bus; halfword and word stores reach
     * GuestMemory. Reads use the installed ARM9-only GuestMemory regions.
     */
    Status write(NdsCpu cpu,
                 uint32_t address,
                 uint32_t width,
                 uint32_t value) noexcept;

    GuestMemory &backing_memory() const noexcept { return memory_; }
    bool installed() const noexcept { return installed_; }

private:
    static bool contains_address(uint32_t address) noexcept;

    GuestMemory &memory_;
    bool installed_ = false;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_VIDEO_MEMORY_HPP */

/*
 * Nintendo DS WRAMCNT control device.
 *
 * The device translates ARM9 WRAMCNT writes into CPU-visible Shared WRAM
 * views. It deliberately does not emulate the undocumented WRAMSTAT value.
 */
#ifndef ORACIS_MEMORY_NDS_WRAM_CONTROL_HPP
#define ORACIS_MEMORY_NDS_WRAM_CONTROL_HPP

#include <cstdint>

#include "oracis/memory/nds_retail_map.hpp"

namespace oracis::memory {

class NdsWramControl final {
public:
    NdsWramControl(GuestMemory &memory, SharedWramMode initial_mode) noexcept
        : memory_(memory), mode_(initial_mode) {}

    NdsWramControl(const NdsWramControl &) = delete;
    NdsWramControl &operator=(const NdsWramControl &) = delete;

    Status install();

    ReadResult read(NdsCpu cpu,
                    uint32_t address,
                    uint32_t width) const noexcept;

    Status write(NdsCpu cpu,
                 uint32_t address,
                 uint32_t width,
                 uint32_t value) noexcept;

    SharedWramMode mode() const noexcept { return mode_; }
    GuestMemory &backing_memory() const noexcept { return memory_; }
    bool installed() const noexcept { return installed_; }

    static constexpr uint32_t WramStatAddress = UINT32_C(0x04000241);
    static constexpr uint32_t WramCntAddress = UINT32_C(0x04000247);

private:
    GuestMemory &memory_;
    SharedWramMode mode_;
    bool installed_ = false;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_WRAM_CONTROL_HPP */

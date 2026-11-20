/*
 * Nintendo DS 2D display-engine configuration registers.
 *
 * This device stores the ARM9-visible DISPCNT and BG0CNT-BG3CNT registers
 * for engines A and B. Scanline status, rendering and PPU memory fetches
 * are deliberately owned by later display/timing components.
 */
#ifndef ORACIS_MEMORY_NDS_DISPLAY_CONTROL_HPP
#define ORACIS_MEMORY_NDS_DISPLAY_CONTROL_HPP

#include <array>
#include <cstdint>

#include "oracis/memory/guest_memory.hpp"

namespace oracis::memory {

class NdsDisplayControl final {
public:
    static constexpr uint32_t EngineABase = UINT32_C(0x04000000);
    static constexpr uint32_t EngineBBase = UINT32_C(0x04001000);
    static constexpr uint32_t DispcntOffset = UINT32_C(0x0000);
    static constexpr uint32_t BgControlOffset = UINT32_C(0x0008);
    static constexpr uint32_t BgCount = 4u;

    static constexpr uint32_t EngineBDispcntSupportedMask =
        UINT32_C(0xC0B3FFF7);

    NdsDisplayControl() noexcept = default;
    NdsDisplayControl(const NdsDisplayControl &) = delete;
    NdsDisplayControl &operator=(const NdsDisplayControl &) = delete;

    Status install() noexcept;

    ReadResult read(NdsCpu cpu,
                    uint32_t address,
                    uint32_t width) const noexcept;

    Status write(NdsCpu cpu,
                 uint32_t address,
                 uint32_t width,
                 uint32_t value) noexcept;

    bool installed() const noexcept { return installed_; }

private:
    struct EngineState {
        uint32_t dispcnt = 0u;
        std::array<uint16_t, BgCount> bgcnt{};
    };

    static bool valid_cpu(NdsCpu cpu) noexcept;
    static bool locate_register(
        uint32_t address, unsigned &engine_index,
        uint32_t &register_offset) noexcept;

    std::array<EngineState, 2> engines_{};
    bool installed_ = false;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_DISPLAY_CONTROL_HPP */

/*
 * Nintendo DS ARM9 tightly-coupled memory.
 *
 * Physical ITCM/DTCM stores are private to ARM9. CP15 region registers expose
 * periodic windows, and active TCM windows shadow lower-priority RAM mappings.
 */
#ifndef ORACIS_MEMORY_NDS_ARM9_TCM_HPP
#define ORACIS_MEMORY_NDS_ARM9_TCM_HPP

#include <cstdint>

#include "oracis/memory/guest_memory.hpp"

namespace oracis::memory {

class NdsArm9Tcm final {
public:
    struct TimingState {
        uint32_t itcm_window_size = 0u;
        uint32_t dtcm_base = 0u;
        uint32_t dtcm_window_size = 0u;
        bool itcm_load_mode = false;
        bool dtcm_load_mode = false;
    };
    static constexpr uint32_t ItcmPhysicalSize = UINT32_C(0x00008000);
    static constexpr uint32_t DtcmPhysicalSize = UINT32_C(0x00004000);
    static constexpr uint32_t ItcmBackingBase = UINT32_C(0xF1000000);
    static constexpr uint32_t DtcmBackingBase =
        ItcmBackingBase + ItcmPhysicalSize;

    static constexpr uint32_t ItcmLoadModeMask = UINT32_C(1) << 19u;
    static constexpr uint32_t ItcmEnableMask = UINT32_C(1) << 18u;
    static constexpr uint32_t DtcmLoadModeMask = UINT32_C(1) << 17u;
    static constexpr uint32_t DtcmEnableMask = UINT32_C(1) << 16u;
    static constexpr uint32_t AlternateVectorSelectMask = UINT32_C(1) << 13u;
    static constexpr uint32_t RequiredControlOnesMask = UINT32_C(0x78);
    static constexpr uint32_t ResetControl =
        RequiredControlOnesMask | AlternateVectorSelectMask;
    static constexpr uint32_t ItcmResetRegionControl = UINT32_C(6) << 1u;
    static constexpr uint32_t DtcmResetRegionControl = UINT32_C(5) << 1u;

    explicit NdsArm9Tcm(GuestMemory &memory) noexcept : memory_(memory) {}

    Status install() noexcept;
    uint32_t control() const noexcept { return control_; }
    uint32_t size_register() const noexcept;
    uint32_t itcm_code_window_size() const noexcept;
    TimingState timing_state() const noexcept;

    bool set_control(uint32_t value) noexcept;
    bool read_region(bool instruction, uint32_t &value) const noexcept;
    bool write_region(bool instruction, uint32_t value) noexcept;

private:
    bool map_region(bool instruction, uint32_t value) noexcept;
    static bool decode_region(
        bool instruction, uint32_t value,
        uint32_t &base, uint32_t &window_size) noexcept;

    GuestMemory &memory_;
    uint32_t control_ = ResetControl;
    uint32_t itcm_region_control_ = ItcmResetRegionControl;
    uint32_t dtcm_region_control_ = DtcmResetRegionControl;
    bool installed_ = false;
    bool itcm_mapped_ = false;
    bool dtcm_mapped_ = false;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_ARM9_TCM_HPP */

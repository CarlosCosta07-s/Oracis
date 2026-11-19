#include "oracis/memory/nds_arm9_tcm.hpp"

#include <cstdint>

namespace oracis::memory {
namespace {

constexpr uint8_t kItcmPermissions = static_cast<uint8_t>(
    Readable | Writable | Executable);
constexpr uint8_t kDtcmPermissions = static_cast<uint8_t>(
    Readable | Writable);
constexpr uint8_t kOverlayPriority = 1u;
constexpr const char *kItcmRegionName = "NDS ARM9 ITCM window";
constexpr const char *kDtcmRegionName = "NDS ARM9 DTCM window";
constexpr uint32_t kReservedRegionBits = UINT32_C(0x00000FC1);
constexpr uint32_t kMaximumNdsTcmWindow = UINT32_C(0x02000000);

} // namespace

Status NdsArm9Tcm::install() noexcept
{
    if (installed_) return Status::InvalidRegion;

    Status status = memory_.map_ram(
        ItcmBackingBase, ItcmPhysicalSize, kItcmPermissions,
        "NDS ARM9 ITCM physical store", CpuAccessNone);
    if (status != Status::Ok) return status;

    status = memory_.map_ram(
        DtcmBackingBase, DtcmPhysicalSize, kDtcmPermissions,
        "NDS ARM9 DTCM physical store", CpuAccessNone);
    if (status != Status::Ok) {
        memory_.unmap_region("NDS ARM9 ITCM physical store");
        return status;
    }

    /*
     * ARM946E-S resets both region bases to zero and their visible sizes to
     * the physical TCM sizes. Install the default ITCM window but keep it
     * inaccessible until CP15 Control.ITCM is enabled. DTCM base zero is
     * deliberately not mapped: software must program a nonzero base first.
     */
    status = memory_.map_mirror_overlay(
        0u, ItcmBackingBase, ItcmPhysicalSize, ItcmPhysicalSize,
        kItcmRegionName, CpuAccessNone, kOverlayPriority);
    if (status != Status::Ok) {
        memory_.unmap_region("NDS ARM9 DTCM physical store");
        memory_.unmap_region("NDS ARM9 ITCM physical store");
        return status;
    }

    installed_ = true;
    itcm_mapped_ = true;
    return Status::Ok;
}

uint32_t NdsArm9Tcm::size_register() const noexcept
{
    /* ARM946E-S: ITCM 32KB (encoding 6), DTCM 16KB (encoding 5). */
    return (UINT32_C(5) << 18u) | (UINT32_C(6) << 6u);
}

NdsArm9Tcm::TimingState NdsArm9Tcm::timing_state() const noexcept
{
    TimingState state{};
    if (!installed_) return state;

    if (itcm_mapped_ && (control_ & ItcmEnableMask) != 0u) {
        uint32_t base = 0u;
        uint32_t window_size = 0u;
        if (decode_region(true, itcm_region_control_, base, window_size) &&
            base == 0u) {
            state.itcm_window_size = window_size;
            state.itcm_load_mode =
                (control_ & ItcmLoadModeMask) != 0u;
        }
    }

    if (dtcm_mapped_ && (control_ & DtcmEnableMask) != 0u) {
        uint32_t base = 0u;
        uint32_t window_size = 0u;
        if (decode_region(false, dtcm_region_control_, base, window_size)) {
            state.dtcm_base = base;
            state.dtcm_window_size = window_size;
            state.dtcm_load_mode =
                (control_ & DtcmLoadModeMask) != 0u;
        }
    }
    return state;
}

uint32_t NdsArm9Tcm::itcm_code_window_size() const noexcept
{
    return timing_state().itcm_window_size;
}

bool NdsArm9Tcm::decode_region(
    bool instruction, uint32_t value,
    uint32_t &base, uint32_t &window_size) noexcept
{
    if ((value & kReservedRegionBits) != 0u) return false;

    const uint32_t size_encoding = (value >> 1u) & UINT32_C(0x1F);
    if (size_encoding < 3u || size_encoding > 16u) return false;

    const uint64_t decoded_size = UINT64_C(1) << (size_encoding + 9u);
    /*
     * ARM946E-S permits visible TCM areas smaller than physical storage.
     * Oracis currently models the NDS' normal low-address/32MB aperture;
     * larger encodings are refused rather than truncated in GuestMemory.
     */
    if (decoded_size > kMaximumNdsTcmWindow) return false;

    base = value & UINT32_C(0xFFFFF000);
    if ((instruction && base != 0u) ||
        (!instruction && base == 0u)) return false;
    if ((static_cast<uint64_t>(base) & (decoded_size - 1u)) != 0u ||
        static_cast<uint64_t>(base) + decoded_size >
            UINT64_C(0x1'0000'0000)) {
        return false;
    }

    window_size = static_cast<uint32_t>(decoded_size);
    return true;
}

bool NdsArm9Tcm::map_region(bool instruction, uint32_t value) noexcept
{
    if (!installed_) return false;

    uint32_t base = 0u;
    uint32_t window_size = 0u;
    if (!decode_region(instruction, value, base, window_size)) return false;

    const bool enabled = (control_ &
        (instruction ? ItcmEnableMask : DtcmEnableMask)) != 0u;
    const bool load_mode = (control_ &
        (instruction ? ItcmLoadModeMask : DtcmLoadModeMask)) != 0u;
    const uint8_t read_access =
        enabled && !load_mode ? CpuAccessArm9 : CpuAccessNone;
    const uint8_t write_access =
        enabled ? CpuAccessArm9 : CpuAccessNone;
    /* DTCM belongs to the data interface; instruction fetches bypass it. */
    const uint8_t execute_access =
        enabled && instruction ? CpuAccessArm9 : CpuAccessNone;
    const uint8_t cpu_access = static_cast<uint8_t>(
        read_access | write_access | execute_access);
    const uint32_t canonical_base = instruction
        ? ItcmBackingBase : DtcmBackingBase;
    const uint32_t period = instruction
        ? ItcmPhysicalSize : DtcmPhysicalSize;
    const char *region_name = instruction
        ? kItcmRegionName : kDtcmRegionName;
    const bool mapped = instruction ? itcm_mapped_ : dtcm_mapped_;

    const Status status = mapped
        ? memory_.replace_mirror_overlay(
            region_name, base, canonical_base, period,
            window_size, cpu_access, kOverlayPriority,
            read_access, write_access, execute_access)
        : memory_.map_mirror_overlay(
            base, canonical_base, period, window_size,
            region_name, cpu_access, kOverlayPriority,
            read_access, write_access, execute_access);
    if (status != Status::Ok) return false;

    if (instruction) {
        itcm_region_control_ = value;
        itcm_mapped_ = true;
    } else {
        dtcm_region_control_ = value;
        dtcm_mapped_ = true;
    }
    return true;
}

bool NdsArm9Tcm::set_control(uint32_t value) noexcept
{
    constexpr uint32_t kSupportedControlMask =
        RequiredControlOnesMask |
        AlternateVectorSelectMask |
        ItcmLoadModeMask |
        ItcmEnableMask |
        DtcmLoadModeMask |
        DtcmEnableMask;
    if ((value & ~kSupportedControlMask) != 0u ||
        (value & RequiredControlOnesMask) != RequiredControlOnesMask ||
        ((value & ItcmEnableMask) != 0u && !itcm_mapped_) ||
        ((value & DtcmEnableMask) != 0u && !dtcm_mapped_)) {
        return false;
    }

    const bool itcm_enabled = (value & ItcmEnableMask) != 0u;
    const bool dtcm_enabled = (value & DtcmEnableMask) != 0u;
    const bool itcm_load_mode = (value & ItcmLoadModeMask) != 0u;
    const bool dtcm_load_mode = (value & DtcmLoadModeMask) != 0u;
    const uint8_t itcm_read = itcm_enabled && !itcm_load_mode
        ? CpuAccessArm9 : CpuAccessNone;
    const uint8_t itcm_write = itcm_enabled
        ? CpuAccessArm9 : CpuAccessNone;
    const uint8_t dtcm_read = dtcm_enabled && !dtcm_load_mode
        ? CpuAccessArm9 : CpuAccessNone;
    const uint8_t dtcm_write = dtcm_enabled
        ? CpuAccessArm9 : CpuAccessNone;
    const GuestMemory::CpuAccessUpdate updates[] = {
        {kItcmRegionName,
         itcm_enabled ? CpuAccessArm9 : CpuAccessNone,
         itcm_read, itcm_write,
         itcm_enabled ? CpuAccessArm9 : CpuAccessNone},
        {kDtcmRegionName,
         dtcm_enabled ? CpuAccessArm9 : CpuAccessNone,
         dtcm_read, dtcm_write, CpuAccessNone}
    };
    GuestMemory::CpuAccessUpdate active_updates[2]{};
    std::size_t update_count = 0u;
    if (itcm_mapped_) active_updates[update_count++] = updates[0];
    if (dtcm_mapped_) active_updates[update_count++] = updates[1];
    if (update_count != 0u &&
        memory_.update_cpu_access(active_updates, update_count) != Status::Ok) {
        return false;
    }

    control_ = value;
    return true;
}

bool NdsArm9Tcm::read_region(
    bool instruction, uint32_t &value) const noexcept
{
    if (!installed_) return false;
    value = instruction ? itcm_region_control_ : dtcm_region_control_;
    return true;
}

bool NdsArm9Tcm::write_region(
    bool instruction, uint32_t value) noexcept
{
    return map_region(instruction, value);
}

} // namespace oracis::memory

#include "oracis/memory/nds_wram_control.hpp"

namespace oracis::memory {

Status NdsWramControl::install()
{
    if (installed_) return Status::InvalidRegion;
    try {
        const Status status = NdsRetailRamMap::install(memory_, mode_);
        if (status == Status::Ok) installed_ = true;
        return status;
    } catch (...) {
        return Status::InvalidRegion;
    }
}

ReadResult NdsWramControl::read(
    NdsCpu cpu,
    uint32_t address,
    uint32_t width) const noexcept
{
    if (address == WramStatAddress) {
        /*
         * 0x04000241 is WRAMSTAT only on ARM7. On ARM9 the same address
         * belongs to VRAMCNT_B, so this WRAM device must leave it untouched.
         */
        if (cpu == NDS_ARM9) return {Status::Unmapped, 0u};
        if (cpu != NDS_ARM7) return {Status::InvalidCpu, 0u};
        if (width != 1u) return {Status::InvalidWidth, 0u};
        if (!installed_) return {Status::Unmapped, 0u};

        /* GBATEK: bits 0-1 expose the current WRAMCNT assignment; 2-7=0. */
        return {
            Status::Ok,
            static_cast<uint32_t>(static_cast<uint8_t>(mode_) & 0x03u)
        };
    }

    if (address != WramCntAddress) return {Status::Unmapped, 0u};
    if (cpu != NDS_ARM9) return {Status::CpuAccessDenied, 0u};
    if (width != 1u) return {Status::InvalidWidth, 0u};
    if (!installed_) return {Status::Unmapped, 0u};

    return {
        Status::Ok,
        static_cast<uint32_t>(static_cast<uint8_t>(mode_))
    };
}

Status NdsWramControl::write(
    NdsCpu cpu,
    uint32_t address,
    uint32_t width,
    uint32_t value) noexcept
{
    if (address == WramStatAddress) {
        /* ARM7 WRAMSTAT is read-only; ARM9 owns VRAMCNT_B at this address. */
        if (cpu == NDS_ARM9) return Status::Unmapped;
        if (cpu != NDS_ARM7) return Status::InvalidCpu;
        if (width != 1u) return Status::InvalidWidth;
        if (!installed_) return Status::Unmapped;
        return Status::PermissionDenied;
    }

    if (address != WramCntAddress) return Status::Unmapped;
    if (cpu != NDS_ARM9) return Status::CpuAccessDenied;
    if (width != 1u) return Status::InvalidWidth;
    if (!installed_) return Status::Unmapped;

    /*
     * WRAMCNT bits 0-1 select one of four modes; bits 2-7 are reserved.
     * Rebuilding the views preserves the two 16KB physical backing banks.
     */
    const SharedWramMode requested =
        static_cast<SharedWramMode>(value & UINT32_C(0x03));
    try {
        const Status status =
            NdsRetailRamMap::set_shared_wram_mode(memory_, requested);
        if (status == Status::Ok) mode_ = requested;
        return status;
    } catch (...) {
        return Status::InvalidRegion;
    }
}

} // namespace oracis::memory

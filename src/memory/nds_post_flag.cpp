#include "oracis/memory/nds_post_flag.hpp"

namespace oracis::memory {

bool NdsPostFlag::valid_cpu(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 || cpu == NDS_ARM7;
}

Status NdsPostFlag::install() noexcept
{
    if (installed_) return Status::InvalidRegion;
    value_ = 0u;
    installed_ = true;
    return Status::Ok;
}

ReadResult NdsPostFlag::read(
    NdsCpu cpu, uint32_t address, uint32_t width) const noexcept
{
    if (address != Address) return {Status::Unmapped, 0u};
    if (!installed_) return {Status::Unmapped, 0u};
    if (!valid_cpu(cpu)) return {Status::InvalidCpu, 0u};
    if (width != 1u) return {Status::InvalidWidth, 0u};
    return {Status::Ok, value_};
}

Status NdsPostFlag::apply_arm9_value(uint32_t value) noexcept
{
    /* POSTFLG bit 0 may be set, but never cleared during a running session. */
    const uint8_t sticky_post_boot = static_cast<uint8_t>(
        value_ & PostBootMask);
    value_ = static_cast<uint8_t>(
        sticky_post_boot | (value & Arm9ControlMask));
    if ((value & PostBootMask) != 0u) {
        value_ = static_cast<uint8_t>(value_ | PostBootMask);
    }
    value_ = static_cast<uint8_t>(value_ & ImplementedMask);
    return Status::Ok;
}

Status NdsPostFlag::write(
    NdsCpu cpu,
    uint32_t address,
    uint32_t width,
    uint32_t value) noexcept
{
    if (address != Address) return Status::Unmapped;
    if (!installed_) return Status::Unmapped;
    if (!valid_cpu(cpu)) return Status::InvalidCpu;
    if (width != 1u) return Status::InvalidWidth;
    if (cpu != NDS_ARM9) return Status::PermissionDenied;
    return apply_arm9_value(value);
}

Status NdsPostFlag::write_from_bios(NdsCpu cpu, uint32_t value) noexcept
{
    if (!installed_) return Status::Unmapped;
    if (!valid_cpu(cpu)) return Status::InvalidCpu;
    if (cpu != NDS_ARM9) return Status::PermissionDenied;
    return apply_arm9_value(value);
}

} // namespace oracis::memory

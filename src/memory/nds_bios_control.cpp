#include "oracis/memory/nds_bios_control.hpp"

namespace oracis::memory {

namespace {

constexpr uint8_t kBiosPermissions =
    static_cast<uint8_t>(Readable | Executable);
constexpr const char *kArm7BiosRegionName = "NDS ARM7 BIOS ROM";
constexpr const char *kArm9BiosRegionName = "NDS ARM9 BIOS ROM";

} // namespace

bool NdsBiosControl::range_contains(
    uint32_t base, uint32_t size,
    uint32_t address, uint32_t width) noexcept
{
    if (width != 1u && width != 2u && width != 4u) return false;
    const uint64_t end = static_cast<uint64_t>(address) + width;
    const uint64_t region_end = static_cast<uint64_t>(base) + size;
    return address >= base && end <= region_end;
}

Status NdsBiosControl::load_arm7_bios(
    const uint8_t *bytes, uint32_t size) noexcept
{
    if (bytes == nullptr || size != Arm7BiosBytes) return Status::InvalidRegion;
    if (arm7_bios_loaded_) return Status::Overlap;
    const Status status = memory_.map_image(
        Arm7BiosBase, bytes, Arm7BiosBytes, kBiosPermissions,
        kArm7BiosRegionName, CpuAccessArm7);
    if (status != Status::Ok) return status;
    arm7_bios_loaded_ = true;
    protection_boundary_ = 0u;
    protection_locked_ = false;
    return Status::Ok;
}

Status NdsBiosControl::load_arm9_bios(
    const uint8_t *bytes, uint32_t size) noexcept
{
    if (bytes == nullptr || size != Arm9BiosBytes) return Status::InvalidRegion;
    if (arm9_bios_loaded_) return Status::Overlap;
    const Status status = memory_.map_image(
        Arm9BiosBase, bytes, Arm9BiosBytes, kBiosPermissions,
        kArm9BiosRegionName, CpuAccessArm9);
    if (status != Status::Ok) return status;
    arm9_bios_loaded_ = true;
    return Status::Ok;
}

Status NdsBiosControl::unload_arm7_bios() noexcept
{
    if (!arm7_bios_loaded_) return Status::Unmapped;
    if (!memory_.unmap_region(kArm7BiosRegionName)) return Status::InvalidRegion;
    arm7_bios_loaded_ = false;
    protection_boundary_ = 0u;
    protection_locked_ = false;
    return Status::Ok;
}

Status NdsBiosControl::unload_arm9_bios() noexcept
{
    if (!arm9_bios_loaded_) return Status::Unmapped;
    if (!memory_.unmap_region(kArm9BiosRegionName)) return Status::InvalidRegion;
    arm9_bios_loaded_ = false;
    return Status::Ok;
}

Status NdsBiosControl::uninstall() noexcept
{
    if (arm9_bios_loaded_ && unload_arm9_bios() != Status::Ok) {
        return Status::InvalidRegion;
    }
    if (arm7_bios_loaded_ && unload_arm7_bios() != Status::Ok) {
        return Status::InvalidRegion;
    }
    protection_boundary_ = 0u;
    protection_locked_ = false;
    return Status::Ok;
}

bool NdsBiosControl::arm7_byte_read_allowed(
    uint32_t offset, uint32_t current_pc) const noexcept
{
    if (current_pc < protection_boundary_) return true;
    return offset >= protection_boundary_ && current_pc < Arm7BiosBytes;
}

ReadResult NdsBiosControl::read(
    NdsCpu cpu, uint32_t address, uint32_t width,
    uint32_t current_pc) const noexcept
{
    if (cpu != NDS_ARM7 && cpu != NDS_ARM9) {
        return {Status::InvalidCpu, 0u};
    }
    if (width != 1u && width != 2u && width != 4u) {
        if ((cpu == NDS_ARM7 && address == BiosProtectionAddress) ||
            (cpu == NDS_ARM7 && address < Arm7BiosBytes) ||
            (cpu == NDS_ARM9 && address >= Arm9BiosBase &&
             address < Arm9BiosBase + Arm9BiosBytes)) {
            return {Status::InvalidWidth, 0u};
        }
        return {Status::Unmapped, 0u};
    }

    if (cpu == NDS_ARM7 && address == BiosProtectionAddress) {
        if (width != 4u) return {Status::InvalidWidth, 0u};
        return {Status::Ok, protection_boundary_};
    }

    if (cpu == NDS_ARM7 && address < Arm7BiosBytes) {
        if (!arm7_bios_loaded_) return {Status::Unmapped, 0u};
        if (!range_contains(Arm7BiosBase, Arm7BiosBytes, address, width)) {
            return {Status::CrossesRegion, 0u};
        }
        uint32_t value = 0u;
        for (uint32_t index = 0u; index < width; ++index) {
            const uint32_t byte_address = address + index;
            const uint32_t offset = byte_address - Arm7BiosBase;
            uint8_t byte = UINT8_C(0xFF);
            if (arm7_byte_read_allowed(offset, current_pc)) {
                const ReadResult mapped = memory_.read(
                    NDS_ARM7, byte_address, 1u);
                if (!mapped) return mapped;
                byte = static_cast<uint8_t>(mapped.value);
            }
            value |= static_cast<uint32_t>(byte) << (index * 8u);
        }
        return {Status::Ok, value};
    }

    if (cpu == NDS_ARM9 && address >= Arm9BiosBase &&
        address < Arm9BiosBase + Arm9BiosBytes) {
        if (!arm9_bios_loaded_) return {Status::Unmapped, 0u};
        if (!range_contains(Arm9BiosBase, Arm9BiosBytes, address, width)) {
            return {Status::CrossesRegion, 0u};
        }
        return memory_.read(NDS_ARM9, address, width);
    }

    return {Status::Unmapped, 0u};
}

Status NdsBiosControl::write(
    NdsCpu cpu, uint32_t address, uint32_t width,
    uint32_t value) noexcept
{
    if (cpu != NDS_ARM7 && cpu != NDS_ARM9) return Status::InvalidCpu;
    if (cpu != NDS_ARM7 || address != BiosProtectionAddress) {
        return Status::Unmapped;
    }
    if (width != 4u) return Status::InvalidWidth;
    if (!protection_locked_) {
        /* Hardware ignores bit 0 and the unused upper register bits. */
        protection_boundary_ = value & UINT32_C(0x00003FFE);
        protection_locked_ = true;
    }
    return Status::Ok;
}

} // namespace oracis::memory

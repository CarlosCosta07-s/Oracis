#include "oracis/memory/nds_arm9_protection_unit.hpp"

#include <limits>

namespace oracis::memory {
namespace {

constexpr uint32_t kRegisterByteMask = UINT32_C(0x000000FF);
constexpr uint32_t kRegionReservedMask = UINT32_C(0x00000FC0);
constexpr uint32_t kRegionSizeMask = UINT32_C(0x0000003E);
constexpr uint32_t kRegionEnableMask = UINT32_C(0x00000001);
constexpr uint64_t kAddressSpaceSize = UINT64_C(0x1'0000'0000);

} // namespace

bool NdsArm9ProtectionUnit::valid_access_permission(uint8_t value) noexcept
{
    return value <= 3u || value == 5u || value == 6u;
}

bool NdsArm9ProtectionUnit::write_cacheable(
    bool instruction, uint32_t value) noexcept
{
    if ((value & ~kRegisterByteMask) != 0u) return false;
    if (instruction) instruction_cacheable_ = value;
    else data_cacheable_ = value;
    return true;
}

uint32_t NdsArm9ProtectionUnit::read_cacheable(bool instruction) const noexcept
{
    return instruction ? instruction_cacheable_ : data_cacheable_;
}

bool NdsArm9ProtectionUnit::write_bufferable(uint32_t value) noexcept
{
    if ((value & ~kRegisterByteMask) != 0u) return false;
    data_bufferable_ = value;
    return true;
}

uint32_t NdsArm9ProtectionUnit::read_bufferable() const noexcept
{
    return data_bufferable_;
}

bool NdsArm9ProtectionUnit::write_access_permissions(
    bool instruction, bool extended, uint32_t value) noexcept
{
    auto &permissions = instruction
        ? instruction_permissions_ : data_permissions_;

    if (extended) {
        for (uint32_t region = 0u; region < RegionCount; ++region) {
            const uint8_t permission = static_cast<uint8_t>(
                (value >> (region * 4u)) & UINT32_C(0x0F));
            if (!valid_access_permission(permission)) return false;
        }
        for (uint32_t region = 0u; region < RegionCount; ++region) {
            permissions[region] = static_cast<uint8_t>(
                (value >> (region * 4u)) & UINT32_C(0x0F));
        }
    } else {
        for (uint32_t region = 0u; region < RegionCount; ++region) {
            permissions[region] = static_cast<uint8_t>(
                (value >> (region * 2u)) & UINT32_C(0x03));
        }
    }

    if (instruction) instruction_permissions_programmed_ = true;
    else data_permissions_programmed_ = true;
    return true;
}

uint32_t NdsArm9ProtectionUnit::read_access_permissions(
    bool instruction, bool extended) const noexcept
{
    const auto &permissions = instruction
        ? instruction_permissions_ : data_permissions_;
    uint32_t value = 0u;
    const uint32_t width = extended ? 4u : 2u;
    const uint32_t mask = extended ? UINT32_C(0x0F) : UINT32_C(0x03);
    for (uint32_t region = 0u; region < RegionCount; ++region) {
        value |= (static_cast<uint32_t>(permissions[region]) & mask)
            << (region * width);
    }
    return value;
}

bool NdsArm9ProtectionUnit::decode_region(
    uint32_t value, uint64_t &base, uint64_t &size) noexcept
{
    if ((value & kRegionReservedMask) != 0u) return false;
    const uint32_t encoding = (value & kRegionSizeMask) >> 1u;
    if (encoding < 11u) return false;

    size = UINT64_C(1) << (encoding + 1u);
    base = static_cast<uint64_t>(value & UINT32_C(0xFFFFF000));
    if ((base & (size - 1u)) != 0u || base + size > kAddressSpaceSize) {
        return false;
    }
    return true;
}

bool NdsArm9ProtectionUnit::write_region(
    uint32_t index, uint32_t value) noexcept
{
    if (index >= RegionCount) return false;
    uint64_t base = 0u;
    uint64_t size = 0u;
    if (!decode_region(value, base, size)) return false;
    regions_[index] = value;
    return true;
}

bool NdsArm9ProtectionUnit::read_region(
    uint32_t index, uint32_t &value) const noexcept
{
    if (index >= RegionCount) return false;
    value = regions_[index];
    return true;
}

bool NdsArm9ProtectionUnit::can_enable() const noexcept
{
    if (!data_permissions_programmed_ ||
        !instruction_permissions_programmed_) {
        return false;
    }

    for (uint32_t index = 0u; index < RegionCount; ++index) {
        if ((regions_[index] & kRegionEnableMask) != 0u) return true;
    }
    return false;
}

int NdsArm9ProtectionUnit::matching_region(uint32_t address) const noexcept
{
    /* ARM946E-S gives the highest-numbered overlapping region priority. */
    for (int index = static_cast<int>(RegionCount) - 1; index >= 0; --index) {
        const uint32_t value = regions_[static_cast<std::size_t>(index)];
        if ((value & kRegionEnableMask) == 0u) continue;
        uint64_t base = 0u;
        uint64_t size = 0u;
        if (!decode_region(value, base, size)) continue;
        const uint64_t candidate = address;
        if (candidate >= base && candidate < base + size) return index;
    }
    return -1;
}

bool NdsArm9ProtectionUnit::permission_allows(
    uint8_t permission, bool write, bool privileged) noexcept
{
    switch (permission) {
    case 0u: /* No access. */
        return false;
    case 1u: /* Privileged read/write, User no access. */
        return privileged;
    case 2u: /* Privileged read/write, User read-only. */
        return privileged || !write;
    case 3u: /* Read/write for both privilege levels. */
        return true;
    case 5u: /* Privileged read-only, User no access. */
        return privileged && !write;
    case 6u: /* Read-only for both privilege levels. */
        return !write;
    default:
        return false;
    }
}

bool NdsArm9ProtectionUnit::check_access(
    uint32_t address, uint32_t size, AccessKind kind,
    bool privileged) const noexcept
{
    if (size == 0u ||
        static_cast<uint64_t>(address) + size > kAddressSpaceSize) {
        return false;
    }
    if (!enabled_) return true;

    const bool instruction = kind == AccessKind::InstructionFetch;
    const bool write = kind == AccessKind::DataWrite;
    const auto &permissions = instruction
        ? instruction_permissions_ : data_permissions_;

    /*
     * Check every addressed byte so a transfer crossing a protection-region
     * boundary cannot inherit the permission of only its first byte.
     */
    for (uint32_t offset = 0u; offset < size; ++offset) {
        const uint64_t current = static_cast<uint64_t>(address) + offset;
        const int region = matching_region(static_cast<uint32_t>(current));
        if (region < 0) return false;
        if (!permission_allows(
                permissions[static_cast<std::size_t>(region)],
                write, privileged)) {
            return false;
        }
    }
    return true;
}

bool NdsArm9ProtectionUnit::attributes(
    uint32_t address, bool instruction, Attributes &result) const noexcept
{
    if (!enabled_) return false;
    const int region = matching_region(address);
    if (region < 0) return false;
    const uint32_t index = static_cast<uint32_t>(region);
    const uint32_t cache_bits = instruction
        ? instruction_cacheable_ : data_cacheable_;
    result.region = static_cast<uint8_t>(index);
    result.cacheable = ((cache_bits >> index) & 1u) != 0u;
    result.bufferable = !instruction &&
        ((data_bufferable_ >> index) & 1u) != 0u;
    return true;
}

} // namespace oracis::memory

#include "oracis/memory/guest_memory.hpp"

#include <limits>
#include <utility>

namespace oracis::memory {
namespace {

constexpr uint64_t kGuestAddressSpaceSize = UINT64_C(0x1'0000'0000);
constexpr uint8_t kKnownPermissions =
    static_cast<uint8_t>(Readable | Writable | Executable);

bool has_permission(uint8_t permissions, Permission permission) noexcept
{
    return (permissions & static_cast<uint8_t>(permission)) != 0u;
}

} // namespace

bool GuestMemory::valid_cpu(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 || cpu == NDS_ARM7;
}

bool GuestMemory::valid_range(uint32_t base, uint32_t size) noexcept
{
    if (size == 0u) return false;
    const uint64_t end = static_cast<uint64_t>(base) + size;
    return end <= kGuestAddressSpaceSize;
}

bool GuestMemory::overlaps(uint32_t base, uint32_t size) const noexcept
{
    const uint64_t begin = base;
    const uint64_t end = begin + size;

    for (const Region &region : regions_) {
        const uint64_t other_begin = region.base;
        const uint64_t other_end = other_begin + region.size;
        if (begin < other_end && other_begin < end) return true;
    }
    return false;
}

Status GuestMemory::map_ram(uint32_t base,
                            uint32_t size,
                            uint8_t permissions,
                            std::string name)
{
    if (!valid_range(base, size) || name.empty() ||
        (permissions & ~kKnownPermissions) != 0u ||
        (permissions & (Readable | Writable | Executable)) == 0u) {
        return Status::InvalidRegion;
    }
    if (overlaps(base, size)) return Status::Overlap;

    try {
        Region region;
        region.base = base;
        region.size = size;
        region.permissions = permissions;
        region.name = std::move(name);
        region.backing = std::make_shared<Backing>(size);
        regions_.push_back(std::move(region));
    } catch (...) {
        return Status::InvalidRegion;
    }
    return Status::Ok;
}

Status GuestMemory::map_alias(uint32_t alias_base,
                              uint32_t canonical_base,
                              uint32_t size,
                              std::string name)
{
    if (!valid_range(alias_base, size) || !valid_range(canonical_base, size) ||
        name.empty()) {
        return Status::InvalidRegion;
    }
    if (overlaps(alias_base, size)) return Status::Overlap;

    const Resolution target = resolve(canonical_base, size);
    if (target.status != Status::Ok || target.region == nullptr) {
        return target.status;
    }

    try {
        Region alias;
        alias.base = alias_base;
        alias.size = size;
        alias.backing_offset = target.backing_offset;
        alias.permissions = target.region->permissions;
        alias.name = std::move(name);
        alias.backing = target.region->backing;
        regions_.push_back(std::move(alias));
    } catch (...) {
        return Status::InvalidRegion;
    }
    return Status::Ok;
}

GuestMemory::Resolution GuestMemory::resolve(uint32_t address,
                                             uint32_t size) const noexcept
{
    if (size == 0u) return {Status::InvalidWidth, nullptr, 0u};
    const uint64_t access_end = static_cast<uint64_t>(address) + size;
    if (access_end > kGuestAddressSpaceSize) {
        return {Status::AddressOverflow, nullptr, 0u};
    }

    for (const Region &region : regions_) {
        const uint64_t region_begin = region.base;
        const uint64_t region_end = region_begin + region.size;
        if (address >= region_begin && address < region_end) {
            if (access_end > region_end) {
                return {Status::CrossesRegion, nullptr, 0u};
            }
            const uint32_t offset = address - region.base;
            return {
                Status::Ok,
                &region,
                region.backing_offset + offset
            };
        }
    }
    return {Status::Unmapped, nullptr, 0u};
}

GuestMemory::MutableResolution GuestMemory::resolve_mutable(
    uint32_t address, uint32_t size) noexcept
{
    const Resolution result = resolve(address, size);
    if (result.status != Status::Ok || result.region == nullptr) {
        return {result.status, nullptr, 0u};
    }

    for (Region &region : regions_) {
        if (&region == result.region) {
            return {Status::Ok, &region, result.backing_offset};
        }
    }
    return {Status::Unmapped, nullptr, 0u};
}

ReadResult GuestMemory::read(NdsCpu cpu,
                             uint32_t address,
                             uint32_t width) const noexcept
{
    if (!valid_cpu(cpu)) return {Status::InvalidCpu, 0u};
    if (width != 1u && width != 2u && width != 4u) {
        return {Status::InvalidWidth, 0u};
    }

    const Resolution result = resolve(address, width);
    if (result.status != Status::Ok || result.region == nullptr) {
        return {result.status, 0u};
    }
    if (!has_permission(result.region->permissions, Readable)) {
        return {Status::PermissionDenied, 0u};
    }

    const auto &bytes = result.region->backing->bytes;
    const uint32_t offset = result.backing_offset;
    uint32_t value = 0u;
    for (uint32_t index = 0u; index < width; ++index) {
        value |= static_cast<uint32_t>(bytes[offset + index]) << (index * 8u);
    }
    return {Status::Ok, value};
}

Status GuestMemory::write(NdsCpu cpu,
                          uint32_t address,
                          uint32_t width,
                          uint32_t value) noexcept
{
    if (!valid_cpu(cpu)) return Status::InvalidCpu;
    if (width != 1u && width != 2u && width != 4u) {
        return Status::InvalidWidth;
    }

    const MutableResolution result = resolve_mutable(address, width);
    if (result.status != Status::Ok || result.region == nullptr) {
        return result.status;
    }
    if (!has_permission(result.region->permissions, Writable)) {
        return Status::PermissionDenied;
    }

    auto &bytes = result.region->backing->bytes;
    auto &provenance = result.region->backing->write_provenance;
    const uint32_t offset = result.backing_offset;
    for (uint32_t index = 0u; index < width; ++index) {
        bytes[offset + index] =
            static_cast<uint8_t>((value >> (index * 8u)) & 0xFFu);
        provenance[offset + index] = 1u;
    }
    return Status::Ok;
}

Status GuestMemory::validate_execute(NdsCpu cpu,
                                     uint32_t address,
                                     uint32_t size) const noexcept
{
    if (!valid_cpu(cpu)) return Status::InvalidCpu;
    const Resolution result = resolve(address, size);
    if (result.status != Status::Ok || result.region == nullptr) {
        return result.status;
    }
    if (!has_permission(result.region->permissions, Executable)) {
        return Status::PermissionDenied;
    }
    return Status::Ok;
}

Status GuestMemory::validate_identity(
    NdsCpu cpu,
    uint32_t address,
    uint32_t size,
    const uint8_t *expected) const noexcept
{
    if (!valid_cpu(cpu)) return Status::InvalidCpu;
    if (expected == nullptr || size == 0u) return Status::InvalidRegion;

    const Resolution result = resolve(address, size);
    if (result.status != Status::Ok || result.region == nullptr) {
        return result.status;
    }
    if (!has_permission(result.region->permissions, Readable)) {
        return Status::PermissionDenied;
    }

    const auto &bytes = result.region->backing->bytes;
    const auto &provenance = result.region->backing->write_provenance;
    const uint32_t offset = result.backing_offset;

    /*
     * Writable RAM is not trusted merely because its zero-filled initial
     * backing happens to match an all-zero validation record. Every byte in
     * the validated span must have been materialized by a guest/host bus write
     * before it can identify an executable generation.
     */
    if (has_permission(result.region->permissions, Writable)) {
        for (uint32_t index = 0u; index < size; ++index) {
            if (provenance[offset + index] == 0u) {
                return Status::MissingWriteProvenance;
            }
        }
    }

    for (uint32_t index = 0u; index < size; ++index) {
        if (bytes[offset + index] != expected[index]) {
            return Status::IdentityMismatch;
        }
    }
    return Status::Ok;
}

const char *GuestMemory::status_name(Status status) noexcept
{
    switch (status) {
    case Status::Ok: return "ok";
    case Status::InvalidCpu: return "invalid CPU";
    case Status::InvalidWidth: return "invalid access width";
    case Status::InvalidRegion: return "invalid memory region";
    case Status::AddressOverflow: return "guest address overflow";
    case Status::Overlap: return "overlapping memory region";
    case Status::Unmapped: return "unmapped address";
    case Status::CrossesRegion: return "access crosses memory region";
    case Status::PermissionDenied: return "memory permission denied";
    case Status::IdentityMismatch: return "static code identity mismatch";
    case Status::MissingWriteProvenance: return "RAM bytes have no write provenance";
    }
    return "unknown status";
}

} // namespace oracis::memory

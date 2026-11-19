#include "oracis/memory/guest_memory.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace oracis::memory {
namespace {

constexpr uint64_t kGuestAddressSpaceSize = UINT64_C(0x1'0000'0000);
constexpr uint8_t kKnownPermissions =
    static_cast<uint8_t>(Readable | Writable | Executable);
constexpr uint8_t kKnownCpuAccess =
    static_cast<uint8_t>(CpuAccessArm9 | CpuAccessArm7);

bool has_permission(uint8_t permissions, Permission permission) noexcept
{
    return (permissions & static_cast<uint8_t>(permission)) != 0u;
}

} // namespace

bool GuestMemory::valid_cpu(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 || cpu == NDS_ARM7;
}

uint8_t GuestMemory::cpu_access_bit(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 ? CpuAccessArm9 : CpuAccessArm7;
}

bool GuestMemory::valid_cpu_access(uint8_t cpu_access) noexcept
{
    return (cpu_access & ~kKnownCpuAccess) == 0u;
}

bool GuestMemory::valid_range(uint32_t base, uint32_t size) noexcept
{
    if (size == 0u) return false;
    const uint64_t end = static_cast<uint64_t>(base) + size;
    return end <= kGuestAddressSpaceSize;
}

bool GuestMemory::overlaps(uint32_t base,
                           uint32_t size,
                           uint8_t cpu_access) const noexcept
{
    if (cpu_access == CpuAccessNone) return false;

    const uint64_t begin = base;
    const uint64_t end = begin + size;

    for (const Region &region : regions_) {
        if ((region.cpu_access & cpu_access) == 0u) continue;

        const uint64_t other_begin = region.base;
        const uint64_t other_end = other_begin + region.size;
        if (begin < other_end && other_begin < end) return true;
    }
    return false;
}

GuestMemory::Resolution GuestMemory::resolve_any(
    uint32_t address, uint32_t size) const noexcept
{
    if (size == 0u) return {Status::InvalidWidth, nullptr, 0u};
    const uint64_t access_end = static_cast<uint64_t>(address) + size;
    if (access_end > kGuestAddressSpaceSize) {
        return {Status::AddressOverflow, nullptr, 0u};
    }

    const Region *selected = nullptr;
    for (const Region &region : regions_) {
        const uint64_t begin = region.base;
        const uint64_t end = begin + region.size;
        if (address < begin || address >= end) continue;
        if (access_end > end) return {Status::CrossesRegion, nullptr, 0u};
        if (selected != nullptr) return {Status::Overlap, nullptr, 0u};
        selected = &region;
    }
    if (selected == nullptr) return {Status::Unmapped, nullptr, 0u};

    const uint32_t offset = address - selected->base;
    const uint32_t backing_offset = backing_index(*selected, offset);
    return {Status::Ok, selected, backing_offset};
}

GuestMemory::Resolution GuestMemory::resolve(
    NdsCpu cpu, uint32_t address, uint32_t size) const noexcept
{
    if (!valid_cpu(cpu)) return {Status::InvalidCpu, nullptr, 0u};
    if (size == 0u) return {Status::InvalidWidth, nullptr, 0u};

    const uint64_t access_end = static_cast<uint64_t>(address) + size;
    if (access_end > kGuestAddressSpaceSize) {
        return {Status::AddressOverflow, nullptr, 0u};
    }

    const uint8_t cpu_bit = cpu_access_bit(cpu);
    const Region *selected = nullptr;
    bool cpu_denied = false;

    for (const Region &region : regions_) {
        const uint64_t begin = region.base;
        const uint64_t end = begin + region.size;
        if (address < begin || address >= end) continue;

        if ((region.cpu_access & cpu_bit) == 0u) {
            cpu_denied = true;
            continue;
        }
        if (access_end > end) {
            return {Status::CrossesRegion, nullptr, 0u};
        }
        if (selected != nullptr) return {Status::Overlap, nullptr, 0u};
        selected = &region;
    }

    if (selected == nullptr) {
        return {
            cpu_denied ? Status::CpuAccessDenied : Status::Unmapped,
            nullptr,
            0u
        };
    }

    const uint32_t offset = address - selected->base;
    const uint32_t offset_in_backing = backing_index(*selected, offset);
    return {Status::Ok, selected, offset_in_backing};
}

uint32_t GuestMemory::backing_index(
    const Region &region, uint32_t address_offset) noexcept
{
    if (region.backing_period != 0u) {
        address_offset %= region.backing_period;
    }
    return region.backing_offset + address_offset;
}

Status GuestMemory::map_ram(uint32_t base,
                            uint32_t size,
                            uint8_t permissions,
                            std::string name,
                            uint8_t cpu_access)
{
    if (!valid_range(base, size) || name.empty() ||
        (permissions & ~kKnownPermissions) != 0u ||
        (permissions & (Readable | Writable | Executable)) == 0u ||
        !valid_cpu_access(cpu_access)) {
        return Status::InvalidRegion;
    }
    if (overlaps(base, size, cpu_access)) return Status::Overlap;

    try {
        Region region;
        region.base = base;
        region.size = size;
        region.permissions = permissions;
        region.cpu_access = cpu_access;
        region.name = std::move(name);
        region.backing = std::make_shared<Backing>(size);
        regions_.push_back(std::move(region));
    } catch (...) {
        return Status::InvalidRegion;
    }
    return Status::Ok;
}

Status GuestMemory::map_image(uint32_t base,
                              const uint8_t *contents,
                              uint32_t size,
                              uint8_t permissions,
                              std::string name,
                              uint8_t cpu_access)
{
    if (!valid_range(base, size) || contents == nullptr || name.empty() ||
        (permissions & ~kKnownPermissions) != 0u ||
        !has_permission(permissions, Readable) ||
        !valid_cpu_access(cpu_access)) {
        return Status::InvalidRegion;
    }
    if (overlaps(base, size, cpu_access)) return Status::Overlap;

    try {
        Region region;
        region.base = base;
        region.size = size;
        region.permissions = permissions;
        region.cpu_access = cpu_access;
        region.name = std::move(name);
        region.backing = std::make_shared<Backing>(size);
        std::memcpy(region.backing->bytes.data(), contents, size);
        std::fill(region.backing->write_provenance.begin(),
                  region.backing->write_provenance.end(), 1u);
        regions_.push_back(std::move(region));
    } catch (...) {
        return Status::InvalidRegion;
    }
    return Status::Ok;
}

Status GuestMemory::map_alias(uint32_t alias_base,
                              uint32_t canonical_base,
                              uint32_t size,
                              std::string name,
                              uint8_t cpu_access)
{
    if (!valid_range(alias_base, size) ||
        !valid_range(canonical_base, size) ||
        name.empty() ||
        (cpu_access != CpuAccessInherit && !valid_cpu_access(cpu_access))) {
        return Status::InvalidRegion;
    }

    const Resolution target = resolve_any(canonical_base, size);
    if (target.status != Status::Ok || target.region == nullptr) {
        return target.status;
    }

    const uint8_t effective_access = cpu_access == CpuAccessInherit
        ? target.region->cpu_access : cpu_access;
    if (overlaps(alias_base, size, effective_access)) return Status::Overlap;

    try {
        Region alias;
        alias.base = alias_base;
        alias.size = size;
        alias.backing_offset = target.backing_offset;
        alias.permissions = target.region->permissions;
        alias.cpu_access = effective_access;
        alias.name = std::move(name);
        alias.backing = target.region->backing;
        regions_.push_back(std::move(alias));
    } catch (...) {
        return Status::InvalidRegion;
    }
    return Status::Ok;
}

Status GuestMemory::map_mirror(uint32_t alias_base,
                               uint32_t canonical_base,
                               uint32_t period,
                               uint32_t window_size,
                               std::string name,
                               uint8_t cpu_access)
{
    if (!valid_range(alias_base, window_size) ||
        !valid_range(canonical_base, period) ||
        period == 0u ||
        name.empty() ||
        !valid_cpu_access(cpu_access)) {
        return Status::InvalidRegion;
    }

    const Resolution target = resolve_any(canonical_base, period);
    if (target.status != Status::Ok || target.region == nullptr) {
        return target.status;
    }

    const uint8_t effective_access = cpu_access == CpuAccessInherit
        ? target.region->cpu_access : cpu_access;
    if (overlaps(alias_base, window_size, effective_access)) {
        return Status::Overlap;
    }

    try {
        Region mirror;
        mirror.base = alias_base;
        mirror.size = window_size;
        mirror.backing_offset = target.backing_offset;
        mirror.permissions = target.region->permissions;
        mirror.cpu_access = effective_access;
        mirror.backing_period = period;
        mirror.name = std::move(name);
        mirror.backing = target.region->backing;
        regions_.push_back(std::move(mirror));
    } catch (...) {
        return Status::InvalidRegion;
    }
    return Status::Ok;
}

bool GuestMemory::unmap_region(const std::string &name) noexcept
{
    if (name.empty()) return false;
    const auto found = std::find_if(
        regions_.begin(), regions_.end(),
        [&name](const Region &region) { return region.name == name; });
    if (found == regions_.end()) return false;
    regions_.erase(found);
    return true;
}

GuestMemory::MutableResolution GuestMemory::resolve_mutable(
    NdsCpu cpu, uint32_t address, uint32_t size) noexcept
{
    const Resolution result = resolve(cpu, address, size);
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

    const Resolution result = resolve(cpu, address, width);
    if (result.status != Status::Ok || result.region == nullptr) {
        return {result.status, 0u};
    }
    if (!has_permission(result.region->permissions, Readable)) {
        return {Status::PermissionDenied, 0u};
    }

    const auto &bytes = result.region->backing->bytes;
    const uint32_t address_offset = address - result.region->base;
    uint32_t value = 0u;
    for (uint32_t index = 0u; index < width; ++index) {
        const uint32_t byte_index =
            backing_index(*result.region, address_offset + index);
        value |= static_cast<uint32_t>(bytes[byte_index]) << (index * 8u);
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

    const MutableResolution result = resolve_mutable(cpu, address, width);
    if (result.status != Status::Ok || result.region == nullptr) {
        return result.status;
    }
    if (!has_permission(result.region->permissions, Writable)) {
        return Status::PermissionDenied;
    }

    auto &bytes = result.region->backing->bytes;
    auto &provenance = result.region->backing->write_provenance;
    const uint32_t address_offset = address - result.region->base;
    for (uint32_t index = 0u; index < width; ++index) {
        const uint32_t byte_index =
            backing_index(*result.region, address_offset + index);
        bytes[byte_index] =
            static_cast<uint8_t>((value >> (index * 8u)) & 0xFFu);
        provenance[byte_index] = 1u;
    }
    return Status::Ok;
}

Status GuestMemory::validate_execute(NdsCpu cpu,
                                     uint32_t address,
                                     uint32_t size) const noexcept
{
    if (!valid_cpu(cpu)) return Status::InvalidCpu;
    const Resolution result = resolve(cpu, address, size);
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

    const Resolution result = resolve(cpu, address, size);
    if (result.status != Status::Ok || result.region == nullptr) {
        return result.status;
    }
    if (!has_permission(result.region->permissions, Readable)) {
        return Status::PermissionDenied;
    }

    const auto &bytes = result.region->backing->bytes;
    const auto &provenance = result.region->backing->write_provenance;
    const uint32_t address_offset = address - result.region->base;

    for (uint32_t index = 0u; index < size; ++index) {
        const uint32_t byte_index =
            backing_index(*result.region, address_offset + index);
        if (provenance[byte_index] == 0u) {
            return Status::MissingWriteProvenance;
        }
    }

    for (uint32_t index = 0u; index < size; ++index) {
        const uint32_t byte_index =
            backing_index(*result.region, address_offset + index);
        if (bytes[byte_index] != expected[index]) {
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
    case Status::CpuAccessDenied: return "CPU has no access to this memory region";
    case Status::IdentityMismatch: return "static code identity mismatch";
    case Status::MissingWriteProvenance: return "RAM bytes have no write provenance";
    }
    return "unknown status";
}

} // namespace oracis::memory

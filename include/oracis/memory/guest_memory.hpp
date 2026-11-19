/*
 * Oracis guest-memory region map.
 *
 * This layer performs raw byte-addressed transfers only. ARM-specific
 * unaligned rotation, access timing, MMIO side effects and CPU scheduling
 * belong to higher layers and are intentionally not approximated here.
 */
#ifndef ORACIS_MEMORY_GUEST_MEMORY_HPP
#define ORACIS_MEMORY_GUEST_MEMORY_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "oracis/recomp/arm_cpu_state.h"

namespace oracis::memory {

enum class Status : uint8_t {
    Ok = 0,
    InvalidCpu,
    InvalidWidth,
    InvalidRegion,
    AddressOverflow,
    Overlap,
    Unmapped,
    CrossesRegion,
    PermissionDenied,
    IdentityMismatch,
    MissingWriteProvenance
};

enum Permission : uint8_t {
    Readable   = 1u << 0u,
    Writable   = 1u << 1u,
    Executable = 1u << 2u
};

struct ReadResult {
    Status status = Status::Ok;
    uint32_t value = 0u;

    explicit operator bool() const noexcept { return status == Status::Ok; }
};

class GuestMemory final {
public:
    GuestMemory() = default;

    GuestMemory(const GuestMemory &) = delete;
    GuestMemory &operator=(const GuestMemory &) = delete;
    GuestMemory(GuestMemory &&) noexcept = default;
    GuestMemory &operator=(GuestMemory &&) noexcept = default;

    /*
     * Allocate a region owned by this memory map.
     * The half-open guest range [base, base + size) must not overlap another
     * mapping and must remain within the 32-bit guest address space.
     */
    Status map_ram(uint32_t base,
                   uint32_t size,
                   uint8_t permissions,
                   std::string name);

    /*
     * Map a host-provided image (for example, bytes loaded from the user's
     * local game image). Contents are copied into the backing and marked as
     * materialized. The image must be readable; it may also be executable.
     */
    Status map_image(uint32_t base,
                     const uint8_t *contents,
                     uint32_t size,
                     uint8_t permissions,
                     std::string name);

    /*
     * Map a guest alias onto a previously mapped canonical range. Reads and
     * writes through either address operate on the same backing bytes.
     * Aliases inherit the target region's permissions.
     */
    Status map_alias(uint32_t alias_base,
                     uint32_t canonical_base,
                     uint32_t size,
                     std::string name);

    ReadResult read(NdsCpu cpu, uint32_t address, uint32_t width) const noexcept;
    Status write(NdsCpu cpu, uint32_t address, uint32_t width,
                 uint32_t value) noexcept;

    Status validate_execute(NdsCpu cpu, uint32_t address,
                            uint32_t size) const noexcept;
    Status validate_identity(NdsCpu cpu, uint32_t address, uint32_t size,
                             const uint8_t *expected) const noexcept;

    static const char *status_name(Status status) noexcept;
    std::size_t region_count() const noexcept { return regions_.size(); }

private:
    struct Backing {
        explicit Backing(uint32_t size)
            : bytes(size, 0u), write_provenance(size, 0u) {}
        std::vector<uint8_t> bytes;
        std::vector<uint8_t> write_provenance;
    };

    struct Region {
        uint32_t base = 0u;
        uint32_t size = 0u;
        uint32_t backing_offset = 0u;
        uint8_t permissions = 0u;
        std::string name;
        std::shared_ptr<Backing> backing;
    };

    struct Resolution {
        Status status = Status::Unmapped;
        const Region *region = nullptr;
        uint32_t backing_offset = 0u;
    };

    struct MutableResolution {
        Status status = Status::Unmapped;
        Region *region = nullptr;
        uint32_t backing_offset = 0u;
    };

    static bool valid_cpu(NdsCpu cpu) noexcept;
    static bool valid_range(uint32_t base, uint32_t size) noexcept;
    bool overlaps(uint32_t base, uint32_t size) const noexcept;
    Resolution resolve(uint32_t address, uint32_t size) const noexcept;
    MutableResolution resolve_mutable(uint32_t address,
                                      uint32_t size) noexcept;

    std::vector<Region> regions_;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_GUEST_MEMORY_HPP */

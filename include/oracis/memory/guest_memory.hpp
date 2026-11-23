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
    CpuAccessDenied,
    IdentityMismatch,
    MissingWriteProvenance,
    /* Device input or command that hardware may accept but Oracis does not model. */
    Unsupported
};

enum Permission : uint8_t {
    Readable   = 1u << 0u,
    Writable   = 1u << 1u,
    Executable = 1u << 2u
};

enum CpuAccess : uint8_t {
    CpuAccessNone = 0u,
    CpuAccessArm9 = 1u << 0u,
    CpuAccessArm7 = 1u << 1u,
    CpuAccessBoth = CpuAccessArm9 | CpuAccessArm7,
    CpuAccessInherit = 0xFFu
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
    GuestMemory(GuestMemory &&other) noexcept;
    GuestMemory &operator=(GuestMemory &&other) noexcept;

    /*
     * Allocate a region owned by this memory map.
     * The half-open guest range [base, base + size) must not overlap another
     * mapping and must remain within the 32-bit guest address space.
     */
    Status map_ram(uint32_t base,
                   uint32_t size,
                   uint8_t permissions,
                   std::string name,
                   uint8_t cpu_access = CpuAccessBoth);

    /*
     * Map a host-provided image (for example, bytes loaded from the user's
     * local game image). Contents are copied into the backing and marked as
     * materialized. The image must be readable; it may also be executable.
     */
    Status map_image(uint32_t base,
                     const uint8_t *contents,
                     uint32_t size,
                     uint8_t permissions,
                     std::string name,
                     uint8_t cpu_access = CpuAccessBoth);

    /*
     * Map a guest alias onto a previously mapped canonical range. Reads and
     * writes through either address operate on the same backing bytes.
     * Aliases inherit the target region's permissions.
     */
    Status map_alias(uint32_t alias_base,
                     uint32_t canonical_base,
                     uint32_t size,
                     std::string name,
                     uint8_t cpu_access = CpuAccessInherit);

    /*
     * Map a repeating mirror of a canonical backing interval. The visible
     * window may be larger than the backing period; each byte wraps within
     * the period. cpu_access may create CPU-disjoint mappings at the same
     * guest address (for example, two distinct Shared WRAM banks).
     */
    Status map_mirror(uint32_t alias_base,
                      uint32_t canonical_base,
                      uint32_t period,
                      uint32_t window_size,
                      std::string name,
                      uint8_t cpu_access);

    /*
     * Map a higher-priority periodic region over an existing mapping. Used for
     * ARM9 TCM, which shadows the normal RAM mirror while enabled.
     */
    Status map_mirror_overlay(uint32_t alias_base,
                              uint32_t canonical_base,
                              uint32_t period,
                              uint32_t window_size,
                              std::string name,
                              uint8_t cpu_access,
                              uint8_t priority,
                              uint8_t read_access = CpuAccessInherit,
                              uint8_t write_access = CpuAccessInherit,
                              uint8_t execute_access = CpuAccessInherit);
    Status replace_mirror_overlay(const char *name,
                                  uint32_t alias_base,
                                  uint32_t canonical_base,
                                  uint32_t period,
                                  uint32_t window_size,
                                  uint8_t cpu_access,
                                  uint8_t priority,
                                  uint8_t read_access = CpuAccessInherit,
                                  uint8_t write_access = CpuAccessInherit,
                                  uint8_t execute_access = CpuAccessInherit);

    /* Remove one guest mapping by its unique region name. */
    bool unmap_region(const std::string &name) noexcept;

    struct CpuAccessUpdate {
        const char *name;
        /* Union of read/write/execute visibility for overlap bookkeeping. */
        uint8_t cpu_access;
        uint8_t read_access = CpuAccessInherit;
        uint8_t write_access = CpuAccessInherit;
        uint8_t execute_access = CpuAccessInherit;
    };

    /*
     * Transactionally replace CPU visibility masks for existing regions.
     * All names must resolve uniquely; the resulting map must contain no
     * overlapping regions visible to the same CPU. Validation completes
     * before any mutation and the commit phase performs no allocation.
     * This is not a cross-thread synchronization primitive.
     */
    Status update_cpu_access(const CpuAccessUpdate *updates,
                             std::size_t count) noexcept;

    ReadResult read(NdsCpu cpu, uint32_t address, uint32_t width) const noexcept;

    /*
     * Read a mapped region on behalf of an internal device. Region and
     * permission checks still apply, but CPU visibility masks are bypassed.
     * This API must not be exposed through the guest CPU bus.
     */
    ReadResult read_device(uint32_t address, uint32_t width) const noexcept;
    Status write(NdsCpu cpu, uint32_t address, uint32_t width,
                 uint32_t value) noexcept;

    /*
     * Copy an arbitrary byte range into an existing writable guest mapping.
     * The complete guest range is validated before mutation; aliases and
     * periodic mirrors preserve their shared backing and byte provenance.
     */
    Status write_bytes(NdsCpu cpu, uint32_t address,
                       const uint8_t *contents, uint32_t size) noexcept;

    Status validate_read(NdsCpu cpu, uint32_t address,
                          uint32_t size) const noexcept;
    Status validate_execute(NdsCpu cpu, uint32_t address,
                            uint32_t size) const noexcept;
    Status validate_write(NdsCpu cpu, uint32_t address,
                          uint32_t size) const noexcept;
    Status validate_identity(NdsCpu cpu, uint32_t address, uint32_t size,
                             const uint8_t *expected) const noexcept;

    static const char *status_name(Status status) noexcept;
    std::size_t region_count() const noexcept { return regions_.size(); }

    /* Coarse epoch for conservative generated-code/link invalidation.
       Zero means exhausted and permanently disables cache validity. */
    uint64_t mutation_generation() const noexcept;

private:
    struct Backing {
        explicit Backing(uint32_t size)
            : bytes(size, 0u), write_provenance(size, 0u) {}
        std::vector<uint8_t> bytes;
        std::vector<uint8_t> write_provenance;
    };

    enum class AccessKind : uint8_t {
        Read,
        Write,
        Execute
    };

    struct Region {
        uint32_t base = 0u;
        uint32_t size = 0u;
        uint32_t backing_offset = 0u;
        uint8_t permissions = 0u;
        uint8_t cpu_access = CpuAccessBoth;
        uint8_t read_access = CpuAccessBoth;
        uint8_t write_access = CpuAccessBoth;
        uint8_t execute_access = CpuAccessBoth;
        uint32_t backing_period = 0u;
        uint8_t priority = 0u;
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
    static uint8_t cpu_access_bit(NdsCpu cpu) noexcept;
    static bool valid_cpu_access(uint8_t cpu_access) noexcept;
    bool overlaps(uint32_t base, uint32_t size,
                  uint8_t cpu_access) const noexcept;
    bool has_region_name(const std::string &name) const noexcept;
    Resolution resolve_any(uint32_t address, uint32_t size) const noexcept;
    Resolution resolve(NdsCpu cpu, uint32_t address, uint32_t size,
                       AccessKind kind) const noexcept;
    MutableResolution resolve_mutable(NdsCpu cpu,
                                      uint32_t address,
                                      uint32_t size,
                                      AccessKind kind) noexcept;
    static uint8_t access_mask(const Region &region,
                               AccessKind kind) noexcept;
    static uint32_t backing_index(const Region &region,
                                  uint32_t address_offset) noexcept;
    void note_mutation() noexcept;

    std::vector<Region> regions_;
    uint64_t mutation_generation_ = 1u;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_GUEST_MEMORY_HPP */

/*
 * ARM946E-S protection-unit register model for the Nintendo DS ARM9.
 *
 * Cacheability and write-buffer attributes are retained for timing models.
 * Oracis currently services all memory accesses directly from GuestMemory:
 * there is no hidden data/instruction cache or asynchronous write buffer.
 */
#ifndef ORACIS_MEMORY_NDS_ARM9_PROTECTION_UNIT_HPP
#define ORACIS_MEMORY_NDS_ARM9_PROTECTION_UNIT_HPP

#include <array>
#include <cstddef>
#include <cstdint>

namespace oracis::memory {

class NdsArm9ProtectionUnit final {
public:
    enum class AccessKind : uint8_t {
        DataRead = 0,
        DataWrite,
        InstructionFetch
    };

    struct Attributes {
        uint8_t region = 0u;
        bool cacheable = false;
        bool bufferable = false;
    };

    static constexpr uint32_t RegionCount = 8u;

    NdsArm9ProtectionUnit() noexcept = default;

    bool write_cacheable(bool instruction, uint32_t value) noexcept;
    uint32_t read_cacheable(bool instruction) const noexcept;

    bool write_bufferable(uint32_t value) noexcept;
    uint32_t read_bufferable() const noexcept;

    bool write_access_permissions(
        bool instruction, bool extended, uint32_t value) noexcept;
    uint32_t read_access_permissions(
        bool instruction, bool extended) const noexcept;

    bool write_region(uint32_t index, uint32_t value) noexcept;
    bool read_region(uint32_t index, uint32_t &value) const noexcept;

    bool can_enable() const noexcept;
    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
    bool enabled() const noexcept { return enabled_; }

    bool check_access(
        uint32_t address, uint32_t size, AccessKind kind,
        bool privileged) const noexcept;

    bool attributes(uint32_t address, bool instruction,
                    Attributes &result) const noexcept;

private:
    static bool valid_access_permission(uint8_t value) noexcept;
    static bool decode_region(
        uint32_t value, uint64_t &base, uint64_t &size) noexcept;
    int matching_region(uint32_t address) const noexcept;
    static bool permission_allows(
        uint8_t permission, bool write, bool privileged) noexcept;

    uint32_t data_cacheable_ = 0u;
    uint32_t instruction_cacheable_ = 0u;
    uint32_t data_bufferable_ = 0u;
    std::array<uint32_t, RegionCount> regions_{};
    std::array<uint8_t, RegionCount> data_permissions_{};
    std::array<uint8_t, RegionCount> instruction_permissions_{};
    bool data_permissions_programmed_ = false;
    bool instruction_permissions_programmed_ = false;
    bool enabled_ = false;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_ARM9_PROTECTION_UNIT_HPP */

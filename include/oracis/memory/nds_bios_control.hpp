/*
 * User-supplied Nintendo DS BIOS ROM mappings and ARM7 BIOSPROT behavior.
 *
 * This device maps only the CPU-visible BIOS windows. It does not execute
 * BIOS instructions or ship firmware bytes; callers must supply local dumps.
 */
#ifndef ORACIS_MEMORY_NDS_BIOS_CONTROL_HPP
#define ORACIS_MEMORY_NDS_BIOS_CONTROL_HPP

#include <cstddef>
#include <cstdint>

#include "oracis/memory/guest_memory.hpp"

namespace oracis::memory {

class NdsBiosControl final {
public:
    static constexpr uint32_t Arm7BiosBase = UINT32_C(0x00000000);
    static constexpr uint32_t Arm7BiosBytes = UINT32_C(0x00004000);
    static constexpr uint32_t Arm9BiosBase = UINT32_C(0xFFFF0000);
    static constexpr uint32_t Arm9BiosBytes = UINT32_C(0x00001000);
    static constexpr uint32_t BiosProtectionAddress = UINT32_C(0x04000308);

    explicit NdsBiosControl(GuestMemory &memory) noexcept : memory_(memory) {}

    NdsBiosControl(const NdsBiosControl &) = delete;
    NdsBiosControl &operator=(const NdsBiosControl &) = delete;

    Status load_arm7_bios(const uint8_t *bytes, uint32_t size) noexcept;
    Status load_arm9_bios(const uint8_t *bytes, uint32_t size) noexcept;
    Status unload_arm7_bios() noexcept;
    Status unload_arm9_bios() noexcept;
    Status uninstall() noexcept;

    bool arm7_bios_loaded() const noexcept { return arm7_bios_loaded_; }
    bool arm9_bios_loaded() const noexcept { return arm9_bios_loaded_; }
    uint32_t protection_boundary() const noexcept {
        return protection_boundary_;
    }
    bool protection_locked() const noexcept { return protection_locked_; }

    /*
     * Bus read hook. ARM7 BIOS reads outside the permitted PC windows return
     * 0xFF per byte, as the retail protection circuit does. Other addresses
     * return Unmapped so the regular bus/device map can handle them.
     */
    ReadResult read(
        NdsCpu cpu, uint32_t address, uint32_t width,
        uint32_t current_pc) const noexcept;
    Status write(
        NdsCpu cpu, uint32_t address, uint32_t width,
        uint32_t value) noexcept;

    GuestMemory &backing_memory() noexcept { return memory_; }
    const GuestMemory &backing_memory() const noexcept { return memory_; }

private:
    static bool range_contains(
        uint32_t base, uint32_t size,
        uint32_t address, uint32_t width) noexcept;
    bool arm7_byte_read_allowed(
        uint32_t offset, uint32_t current_pc) const noexcept;

    GuestMemory &memory_;
    bool arm7_bios_loaded_ = false;
    bool arm9_bios_loaded_ = false;
    uint32_t protection_boundary_ = 0u;
    bool protection_locked_ = false;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_BIOS_CONTROL_HPP */

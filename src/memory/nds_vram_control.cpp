#include "oracis/memory/nds_vram_control.hpp"

#include <array>

namespace oracis::memory {
namespace {

constexpr uint32_t kVramBackingBase = UINT32_C(0xF1100000);
constexpr uint32_t kVramBackingSize =
    NdsVramControl::BankSize * UINT32_C(2);
constexpr uint8_t kVramPermissions =
    static_cast<uint8_t>(Readable | Writable | Executable);

constexpr const char *kVramCWindow0 = "NDS VRAM C ARM7 window 0";
constexpr const char *kVramCWindow1 = "NDS VRAM C ARM7 window 1";
constexpr const char *kVramDWindow0 = "NDS VRAM D ARM7 window 0";
constexpr const char *kVramDWindow1 = "NDS VRAM D ARM7 window 1";
constexpr const char *kVramCLcdcWindow = "NDS VRAM C ARM9 LCDC window";
constexpr const char *kVramDLcdcWindow = "NDS VRAM D ARM9 LCDC window";

} // namespace

int NdsVramControl::arm7_window_index(uint8_t control) noexcept
{
    /* Enable=1 and MST=010 select the ARM7 Work RAM destination. */
    if ((control & UINT8_C(0x87)) != UINT8_C(0x82)) return -1;

    /* For ARM7, OFS.1 must be zero; OFS.0 selects one of two 128KB slots. */
    const uint8_t offset = static_cast<uint8_t>((control >> 3u) & 0x03u);
    return offset <= 1u ? static_cast<int>(offset) : -1;
}

bool NdsVramControl::allocated_to_arm7(uint8_t control) noexcept
{
    /* VRAMSTAT reports enable+MST only; its bits do not reflect OFS. */
    return (control & UINT8_C(0x87)) == UINT8_C(0x82);
}

bool NdsVramControl::lcdc_enabled(uint8_t control) noexcept
{
    /* C/D use fixed physical LCDC slots; their OFS bits are not used. */
    return (control & UINT8_C(0x87)) == UINT8_C(0x80);
}

uint8_t NdsVramControl::status() const noexcept
{
    return static_cast<uint8_t>(
        (allocated_to_arm7(control_c_) ? 0x01u : 0u) |
        (allocated_to_arm7(control_d_) ? 0x02u : 0u));
}

Status NdsVramControl::install()
{
    if (installed_) return Status::InvalidRegion;

    Status result = memory_.map_ram(
        kVramBackingBase,
        kVramBackingSize,
        kVramPermissions,
        "NDS VRAM C/D physical banks (internal)",
        CpuAccessNone);
    if (result != Status::Ok) return result;

    struct Window {
        uint32_t address;
        uint32_t backing;
        const char *name;
    };
    const Window windows[] = {
        {UINT32_C(0x06000000), kVramBackingBase, kVramCWindow0},
        {UINT32_C(0x06020000), kVramBackingBase, kVramCWindow1},
        {UINT32_C(0x06000000), kVramBackingBase + BankSize, kVramDWindow0},
        {UINT32_C(0x06020000), kVramBackingBase + BankSize, kVramDWindow1},
        {UINT32_C(0x06840000), kVramBackingBase, kVramCLcdcWindow},
        {UINT32_C(0x06860000), kVramBackingBase + BankSize, kVramDLcdcWindow}
    };

    /* Inactive aliases are preallocated; register writes only switch masks. */
    for (const Window &window : windows) {
        result = memory_.map_alias(
            window.address, window.backing, BankSize,
            window.name, CpuAccessNone);
        if (result != Status::Ok) return result;
    }

    installed_ = true;
    return Status::Ok;
}

Status NdsVramControl::apply_controls(
    uint8_t control_c,
    uint8_t control_d) noexcept
{
    const int c_window = arm7_window_index(control_c);
    const int d_window = arm7_window_index(control_d);
    const auto arm7_access_for = [](int selected, int window) noexcept {
        return selected == window
            ? static_cast<uint8_t>(CpuAccessArm7)
            : static_cast<uint8_t>(CpuAccessNone);
    };
    const auto lcdc_access_for = [](bool enabled) noexcept {
        return enabled
            ? static_cast<uint8_t>(CpuAccessArm9)
            : static_cast<uint8_t>(CpuAccessNone);
    };

    const std::array<GuestMemory::CpuAccessUpdate, 6> updates{{
        {kVramCWindow0, arm7_access_for(c_window, 0)},
        {kVramCWindow1, arm7_access_for(c_window, 1)},
        {kVramDWindow0, arm7_access_for(d_window, 0)},
        {kVramDWindow1, arm7_access_for(d_window, 1)},
        {kVramCLcdcWindow, lcdc_access_for(lcdc_enabled(control_c))},
        {kVramDLcdcWindow, lcdc_access_for(lcdc_enabled(control_d))}
    }};

    /*
     * Validate all six aliases before mutation. If C and D target the same
     * ARM7 128KB slot, reject the write and preserve the previous mapping.
     */
    return memory_.update_cpu_access(updates.data(), updates.size());
}

ReadResult NdsVramControl::read(
    NdsCpu cpu,
    uint32_t address,
    uint32_t width) const noexcept
{
    if (address == VramStatAddress) {
        /* On ARM9 this address is VRAMCNT_A, outside this partial device. */
        if (cpu == NDS_ARM9) return {Status::Unmapped, 0u};
        if (cpu != NDS_ARM7) return {Status::InvalidCpu, 0u};
        if (width != 1u) return {Status::InvalidWidth, 0u};
        if (!installed_) return {Status::Unmapped, 0u};
        return {Status::Ok, status()};
    }

    if (address == VramCntCAddress || address == VramCntDAddress) {
        if (cpu != NDS_ARM9) return {Status::CpuAccessDenied, 0u};
        if (width != 1u) return {Status::InvalidWidth, 0u};
        if (!installed_) return {Status::Unmapped, 0u};
        return {Status::PermissionDenied, 0u}; /* write-only registers */
    }

    return {Status::Unmapped, 0u};
}

Status NdsVramControl::write(
    NdsCpu cpu,
    uint32_t address,
    uint32_t width,
    uint32_t value) noexcept
{
    if (address == VramStatAddress) {
        /* ARM7 VRAMSTAT is read-only; ARM9 owns VRAMCNT_A here. */
        if (cpu == NDS_ARM9) return Status::Unmapped;
        if (cpu != NDS_ARM7) return Status::InvalidCpu;
        if (width != 1u) return Status::InvalidWidth;
        if (!installed_) return Status::Unmapped;
        return Status::PermissionDenied;
    }

    if (address != VramCntCAddress && address != VramCntDAddress) {
        return Status::Unmapped;
    }
    if (cpu != NDS_ARM9) return Status::CpuAccessDenied;
    if (width != 1u) return Status::InvalidWidth;
    if (!installed_) return Status::Unmapped;

    /* Bits 5-6 are reserved; keep Enable, OFS and MST only. */
    const uint8_t requested = static_cast<uint8_t>(value & UINT32_C(0x9F));
    const uint8_t next_c =
        address == VramCntCAddress ? requested : control_c_;
    const uint8_t next_d =
        address == VramCntDAddress ? requested : control_d_;

    const Status result = apply_controls(next_c, next_d);
    if (result == Status::Ok) {
        control_c_ = next_c;
        control_d_ = next_d;
    }
    return result;
}

} // namespace oracis::memory

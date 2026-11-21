#include "oracis/memory/nds_vram_control.hpp"

#include <array>

namespace oracis::memory {
namespace {

constexpr uint32_t kVramBackingBase = UINT32_C(0xF1100000);
constexpr uint32_t kBank128K = UINT32_C(0x00020000);
constexpr uint32_t kBank64K = UINT32_C(0x00010000);
constexpr uint32_t kBank32K = UINT32_C(0x00008000);
constexpr uint32_t kBank16K = UINT32_C(0x00004000);
constexpr uint32_t kVramBackingSize = UINT32_C(0x000A4000);
constexpr uint8_t kVramPermissions =
    static_cast<uint8_t>(Readable | Writable | Executable);

struct VramBank {
    uint32_t lcdc_address;
    uint32_t backing_offset;
    uint32_t size;
    uint32_t control_address;
    const char *alias_name;
};

constexpr std::array<VramBank, 9> kVramBanks{{
    {UINT32_C(0x06800000), UINT32_C(0x00000000), kBank128K,
     NdsVramControl::VramCntAAddress, "NDS VRAM A ARM9 LCDC"},
    {UINT32_C(0x06820000), UINT32_C(0x00020000), kBank128K,
     NdsVramControl::VramCntBAddress, "NDS VRAM B ARM9 LCDC"},
    {UINT32_C(0x06840000), UINT32_C(0x00040000), kBank128K,
     NdsVramControl::VramCntCAddress, "NDS VRAM C ARM9 LCDC"},
    {UINT32_C(0x06860000), UINT32_C(0x00060000), kBank128K,
     NdsVramControl::VramCntDAddress, "NDS VRAM D ARM9 LCDC"},
    {UINT32_C(0x06880000), UINT32_C(0x00080000), kBank64K,
     NdsVramControl::VramCntEAddress, "NDS VRAM E ARM9 LCDC"},
    {UINT32_C(0x06890000), UINT32_C(0x00090000), kBank16K,
     NdsVramControl::VramCntFAddress, "NDS VRAM F ARM9 LCDC"},
    {UINT32_C(0x06894000), UINT32_C(0x00094000), kBank16K,
     NdsVramControl::VramCntGAddress, "NDS VRAM G ARM9 LCDC"},
    {UINT32_C(0x06898000), UINT32_C(0x00098000), kBank32K,
     NdsVramControl::VramCntHAddress, "NDS VRAM H ARM9 LCDC"},
    {UINT32_C(0x068A0000), UINT32_C(0x000A0000), kBank16K,
     NdsVramControl::VramCntIAddress, "NDS VRAM I ARM9 LCDC"}
}};

/* MST bit 2 is unused by banks A, B, H and I. */
constexpr uint8_t bank_mst_mask(uint8_t bank_index) noexcept
{
    return (bank_index <= 1u || bank_index >= 7u)
        ? UINT8_C(0x03)
        : UINT8_C(0x07);
}

constexpr bool bank_mst_enabled(
    uint8_t control, uint8_t bank_index, uint8_t mst) noexcept
{
    const uint8_t mask = bank_mst_mask(bank_index);
    const uint8_t compare_mask = static_cast<uint8_t>(UINT8_C(0x80) | mask);
    const uint8_t expected = static_cast<uint8_t>(
        UINT8_C(0x80) | (mst & mask));
    return (control & compare_mask) == expected;
}

struct Arm9VideoView {
    uint8_t bank_index;
    uint8_t mst;
    uint8_t ofs_mask;
    uint8_t ofs_value;
    uint32_t address;
    uint32_t size;
    const char *alias_name;
};

/* CPU-visible BG/OBJ windows. Extended palettes and texture memory are
 * display-controller destinations and deliberately have no CPU aliases. */
constexpr std::array<Arm9VideoView, 43> kArm9VideoViews{{
    {0u, 1u, 3u, 0u, UINT32_C(0x06000000), kBank128K, "NDS VRAM A Main BG OFS0"},
    {0u, 1u, 3u, 1u, UINT32_C(0x06020000), kBank128K, "NDS VRAM A Main BG OFS1"},
    {0u, 1u, 3u, 2u, UINT32_C(0x06040000), kBank128K, "NDS VRAM A Main BG OFS2"},
    {0u, 1u, 3u, 3u, UINT32_C(0x06060000), kBank128K, "NDS VRAM A Main BG OFS3"},
    {1u, 1u, 3u, 0u, UINT32_C(0x06000000), kBank128K, "NDS VRAM B Main BG OFS0"},
    {1u, 1u, 3u, 1u, UINT32_C(0x06020000), kBank128K, "NDS VRAM B Main BG OFS1"},
    {1u, 1u, 3u, 2u, UINT32_C(0x06040000), kBank128K, "NDS VRAM B Main BG OFS2"},
    {1u, 1u, 3u, 3u, UINT32_C(0x06060000), kBank128K, "NDS VRAM B Main BG OFS3"},
    {2u, 1u, 3u, 0u, UINT32_C(0x06000000), kBank128K, "NDS VRAM C Main BG OFS0"},
    {2u, 1u, 3u, 1u, UINT32_C(0x06020000), kBank128K, "NDS VRAM C Main BG OFS1"},
    {2u, 1u, 3u, 2u, UINT32_C(0x06040000), kBank128K, "NDS VRAM C Main BG OFS2"},
    {2u, 1u, 3u, 3u, UINT32_C(0x06060000), kBank128K, "NDS VRAM C Main BG OFS3"},
    {3u, 1u, 3u, 0u, UINT32_C(0x06000000), kBank128K, "NDS VRAM D Main BG OFS0"},
    {3u, 1u, 3u, 1u, UINT32_C(0x06020000), kBank128K, "NDS VRAM D Main BG OFS1"},
    {3u, 1u, 3u, 2u, UINT32_C(0x06040000), kBank128K, "NDS VRAM D Main BG OFS2"},
    {3u, 1u, 3u, 3u, UINT32_C(0x06060000), kBank128K, "NDS VRAM D Main BG OFS3"},
    {0u, 2u, 3u, 0u, UINT32_C(0x06400000), kBank128K, "NDS VRAM A Main OBJ OFS0"},
    {0u, 2u, 3u, 1u, UINT32_C(0x06420000), kBank128K, "NDS VRAM A Main OBJ OFS1"},
    {1u, 2u, 3u, 0u, UINT32_C(0x06400000), kBank128K, "NDS VRAM B Main OBJ OFS0"},
    {1u, 2u, 3u, 1u, UINT32_C(0x06420000), kBank128K, "NDS VRAM B Main OBJ OFS1"},
    {2u, 4u, 0u, 0u, UINT32_C(0x06200000), kBank128K, "NDS VRAM C Sub BG"},
    {3u, 4u, 0u, 0u, UINT32_C(0x06600000), kBank128K, "NDS VRAM D Sub OBJ"},
    {4u, 1u, 0u, 0u, UINT32_C(0x06000000), kBank64K, "NDS VRAM E Main BG"},
    {4u, 2u, 0u, 0u, UINT32_C(0x06400000), kBank64K, "NDS VRAM E Main OBJ"},
    {5u, 1u, 3u, 0u, UINT32_C(0x06000000), kBank16K, "NDS VRAM F Main BG OFS0"},
    {5u, 1u, 3u, 1u, UINT32_C(0x06004000), kBank16K, "NDS VRAM F Main BG OFS1"},
    {5u, 1u, 3u, 2u, UINT32_C(0x06010000), kBank16K, "NDS VRAM F Main BG OFS2"},
    {5u, 1u, 3u, 3u, UINT32_C(0x06014000), kBank16K, "NDS VRAM F Main BG OFS3"},
    {5u, 2u, 3u, 0u, UINT32_C(0x06400000), kBank16K, "NDS VRAM F Main OBJ OFS0"},
    {5u, 2u, 3u, 1u, UINT32_C(0x06404000), kBank16K, "NDS VRAM F Main OBJ OFS1"},
    {5u, 2u, 3u, 2u, UINT32_C(0x06410000), kBank16K, "NDS VRAM F Main OBJ OFS2"},
    {5u, 2u, 3u, 3u, UINT32_C(0x06414000), kBank16K, "NDS VRAM F Main OBJ OFS3"},
    {6u, 1u, 3u, 0u, UINT32_C(0x06000000), kBank16K, "NDS VRAM G Main BG OFS0"},
    {6u, 1u, 3u, 1u, UINT32_C(0x06004000), kBank16K, "NDS VRAM G Main BG OFS1"},
    {6u, 1u, 3u, 2u, UINT32_C(0x06010000), kBank16K, "NDS VRAM G Main BG OFS2"},
    {6u, 1u, 3u, 3u, UINT32_C(0x06014000), kBank16K, "NDS VRAM G Main BG OFS3"},
    {6u, 2u, 3u, 0u, UINT32_C(0x06400000), kBank16K, "NDS VRAM G Main OBJ OFS0"},
    {6u, 2u, 3u, 1u, UINT32_C(0x06404000), kBank16K, "NDS VRAM G Main OBJ OFS1"},
    {6u, 2u, 3u, 2u, UINT32_C(0x06410000), kBank16K, "NDS VRAM G Main OBJ OFS2"},
    {6u, 2u, 3u, 3u, UINT32_C(0x06414000), kBank16K, "NDS VRAM G Main OBJ OFS3"},
    {7u, 1u, 0u, 0u, UINT32_C(0x06200000), kBank32K, "NDS VRAM H Sub BG"},
    {8u, 1u, 0u, 0u, UINT32_C(0x06208000), kBank16K, "NDS VRAM I Sub BG"},
    {8u, 2u, 0u, 0u, UINT32_C(0x06600000), kBank16K, "NDS VRAM I Sub OBJ"}
}};

constexpr uint32_t kVramCBacking =
    kVramBackingBase + UINT32_C(0x00040000);
constexpr uint32_t kVramDBacking =
    kVramBackingBase + UINT32_C(0x00060000);

constexpr const char *kVramCWindow0 = "NDS VRAM C ARM7 window 0";
constexpr const char *kVramCWindow1 = "NDS VRAM C ARM7 window 1";
constexpr const char *kVramDWindow0 = "NDS VRAM D ARM7 window 0";
constexpr const char *kVramDWindow1 = "NDS VRAM D ARM7 window 1";

} // namespace

int NdsVramControl::bank_index_for_address(uint32_t address) noexcept
{
    for (std::size_t index = 0u; index < kVramBanks.size(); ++index) {
        if (kVramBanks[index].control_address == address) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

int NdsVramControl::arm9_vram_bank_index_for_address(
    uint32_t address) const noexcept
{
    for (std::size_t index = 0u; index < kVramBanks.size(); ++index) {
        const VramBank &bank = kVramBanks[index];
        const uint64_t end =
            static_cast<uint64_t>(bank.lcdc_address) + bank.size;
        if (lcdc_enabled(controls_[index], static_cast<uint8_t>(index)) &&
            address >= bank.lcdc_address &&
            static_cast<uint64_t>(address) < end) {
            return static_cast<int>(index);
        }
    }

    for (const Arm9VideoView &view : kArm9VideoViews) {
        const uint8_t control = controls_[view.bank_index];
        const uint8_t ofs = static_cast<uint8_t>((control >> 3u) & 0x03u);
        const bool active = bank_mst_enabled(
                control, view.bank_index, view.mst) &&
            ((ofs & view.ofs_mask) == view.ofs_value);
        const uint64_t end =
            static_cast<uint64_t>(view.address) + view.size;
        if (active && address >= view.address &&
            static_cast<uint64_t>(address) < end) {
            return static_cast<int>(view.bank_index);
        }
    }
    return -1;
}

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

bool NdsVramControl::lcdc_enabled(
    uint8_t control, uint8_t bank_index) noexcept
{
    /* LCDC is a fixed physical slot; OFS and unused MST bits are ignored. */
    return bank_mst_enabled(control, bank_index, 0u);
}

uint8_t NdsVramControl::status() const noexcept
{
    return static_cast<uint8_t>(
        (allocated_to_arm7(controls_[2]) ? 0x01u : 0u) |
        (allocated_to_arm7(controls_[3]) ? 0x02u : 0u));
}

ReadResult NdsVramControl::read_lcdc_bank_halfword(
    uint8_t bank_index, uint32_t byte_offset) const noexcept
{
    /* VRAM display selects only the four 128 KiB banks A-D. */
    if (bank_index >= 4u || (byte_offset & UINT32_C(1)) != 0u ||
        byte_offset > BankSize - UINT32_C(2)) {
        return {Status::InvalidRegion, 0u};
    }
    if (!lcdc_enabled(controls_[bank_index], bank_index)) {
        return {Status::Unmapped, 0u};
    }

    const uint64_t address =
        static_cast<uint64_t>(kVramBackingBase) +
        kVramBanks[bank_index].backing_offset + byte_offset;
    if (address > UINT32_MAX) return {Status::AddressOverflow, 0u};
    return memory_.read_device(static_cast<uint32_t>(address), 2u);
}

ReadResult NdsVramControl::read_extended_bg_palette_slot(
    unsigned engine_index,
    uint8_t palette_slot,
    uint8_t bg_palette,
    uint8_t color_index) const noexcept
{
    if (engine_index > 1u || palette_slot >= 4u || bg_palette >= 16u) {
        return {Status::InvalidRegion, 0u};
    }

    const uint32_t slot_offset =
        static_cast<uint32_t>(palette_slot) * UINT32_C(0x2000) +
        static_cast<uint32_t>(bg_palette) * UINT32_C(0x0200) +
        static_cast<uint32_t>(color_index) * UINT32_C(2);

    uint64_t backing_offset = 0u;
    unsigned matching_banks = 0u;
    const auto select_bank = [&](
        unsigned bank_index, uint32_t bank_relative_offset) noexcept {
        backing_offset =
            static_cast<uint64_t>(kVramBanks[bank_index].backing_offset) +
            bank_relative_offset;
        ++matching_banks;
    };

    if (engine_index == 0u) {
        /* VRAM E holds all four Main Engine A BG extended-palette slots. */
        if (bank_mst_enabled(controls_[4], 4u, 4u)) {
            select_bank(4u, slot_offset);
        }

        /*
         * F/G map a 16 KiB pair: OFS=0 selects slots 0/1, OFS=1 slots 2/3.
         * If both banks claim the selected pair, the backing is ambiguous.
         */
        for (unsigned bank_index = 5u; bank_index <= 6u; ++bank_index) {
            const uint8_t control = controls_[bank_index];
            const uint8_t offset = static_cast<uint8_t>(
                (control >> 3u) & UINT8_C(3));
            if (bank_mst_enabled(
                    control, static_cast<uint8_t>(bank_index), 4u) &&
                offset == static_cast<uint8_t>(palette_slot >> 1u)) {
                const uint32_t bank_relative_offset =
                    (static_cast<uint32_t>(palette_slot) & UINT32_C(1)) *
                        UINT32_C(0x2000) +
                    static_cast<uint32_t>(bg_palette) * UINT32_C(0x0200) +
                    static_cast<uint32_t>(color_index) * UINT32_C(2);
                select_bank(bank_index, bank_relative_offset);
            }
        }
    } else {
        /* VRAM H MST=2 supplies all four Sub Engine B BG palette slots. */
        if (bank_mst_enabled(controls_[7], 7u, 2u)) {
            select_bank(7u, slot_offset);
        }
    }

    if (matching_banks == 0u) return {Status::Unmapped, 0u};
    if (matching_banks > 1u) return {Status::Overlap, 0u};

    const uint64_t address =
        static_cast<uint64_t>(kVramBackingBase) + backing_offset;
    if (address > UINT32_MAX) return {Status::AddressOverflow, 0u};
    return memory_.read_device(static_cast<uint32_t>(address), 2u);
}

ReadResult NdsVramControl::read_extended_bg_palette(
    unsigned engine_index,
    unsigned bg_index,
    uint8_t bg_palette,
    uint8_t color_index) const noexcept
{
    if (bg_index < 2u || bg_index > 3u) {
        return {Status::InvalidRegion, 0u};
    }
    return read_extended_bg_palette_slot(
        engine_index, static_cast<uint8_t>(bg_index),
        bg_palette, color_index);
}

Status NdsVramControl::install()
{
    if (installed_) return Status::InvalidRegion;

    Status result = memory_.map_ram(
        kVramBackingBase,
        kVramBackingSize,
        kVramPermissions,
        "NDS VRAM A-I physical banks (internal)",
        CpuAccessNone);
    if (result != Status::Ok) return result;

    for (const VramBank &bank : kVramBanks) {
        result = memory_.map_alias(
            bank.lcdc_address,
            kVramBackingBase + bank.backing_offset,
            bank.size,
            bank.alias_name,
            CpuAccessNone);
        if (result != Status::Ok) return result;
    }
    for (const Arm9VideoView &view : kArm9VideoViews) {
        const VramBank &bank = kVramBanks[view.bank_index];
        result = memory_.map_alias(
            view.address,
            kVramBackingBase + bank.backing_offset,
            view.size,
            view.alias_name,
            CpuAccessNone);
        if (result != Status::Ok) return result;
    }

    struct Arm7Window {
        uint32_t address;
        uint32_t backing;
        const char *name;
    };
    const Arm7Window arm7_windows[] = {
        {UINT32_C(0x06000000), kVramCBacking, kVramCWindow0},
        {UINT32_C(0x06020000), kVramCBacking, kVramCWindow1},
        {UINT32_C(0x06000000), kVramDBacking, kVramDWindow0},
        {UINT32_C(0x06020000), kVramDBacking, kVramDWindow1}
    };
    for (const Arm7Window &window : arm7_windows) {
        result = memory_.map_alias(
            window.address, window.backing, kBank128K,
            window.name, CpuAccessNone);
        if (result != Status::Ok) return result;
    }

    installed_ = true;
    return Status::Ok;
}

Status NdsVramControl::apply_controls(
    const std::array<uint8_t, 9> &controls) noexcept
{
    const int c_window = arm7_window_index(controls[2]);
    const int d_window = arm7_window_index(controls[3]);
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

    std::array<GuestMemory::CpuAccessUpdate, 56> updates{};
    for (std::size_t index = 0u; index < kVramBanks.size(); ++index) {
        updates[index] = {
            kVramBanks[index].alias_name,
            lcdc_access_for(lcdc_enabled(
                controls[index], static_cast<uint8_t>(index)))
        };
    }
    for (std::size_t index = 0u; index < kArm9VideoViews.size(); ++index) {
        const Arm9VideoView &view = kArm9VideoViews[index];
        const uint8_t control = controls[view.bank_index];
        const uint8_t ofs = static_cast<uint8_t>((control >> 3u) & 0x03u);
        const bool enabled = bank_mst_enabled(
                control, view.bank_index, view.mst) &&
            ((ofs & view.ofs_mask) == view.ofs_value);
        updates[9u + index] = {
            view.alias_name,
            lcdc_access_for(enabled)
        };
    }

    updates[52] = {kVramCWindow0, arm7_access_for(c_window, 0)};
    updates[53] = {kVramCWindow1, arm7_access_for(c_window, 1)};
    updates[54] = {kVramDWindow0, arm7_access_for(d_window, 0)};
    updates[55] = {kVramDWindow1, arm7_access_for(d_window, 1)};

    /*
     * Validate all aliases before mutation. If C and D target the same ARM7
     * 128KB slot, reject the write and preserve the previous complete map.
     */
    return memory_.update_cpu_access(updates.data(), updates.size());
}

ReadResult NdsVramControl::read(
    NdsCpu cpu,
    uint32_t address,
    uint32_t width) const noexcept
{
    if (cpu != NDS_ARM9 && cpu != NDS_ARM7) {
        return {Status::InvalidCpu, 0u};
    }

    if (address == VramStatAddress && cpu == NDS_ARM7) {
        if (width != 1u) return {Status::InvalidWidth, 0u};
        if (!installed_) return {Status::Unmapped, 0u};
        return {Status::Ok, status()};
    }

    /* 0x04000241 is WRAMSTAT for ARM7; its device is checked before VRAM. */
    if (address == VramCntBAddress && cpu == NDS_ARM7) {
        return {Status::Unmapped, 0u};
    }

    if (bank_index_for_address(address) < 0) {
        return {Status::Unmapped, 0u};
    }
    if (cpu != NDS_ARM9) return {Status::CpuAccessDenied, 0u};
    if (width != 1u) return {Status::InvalidWidth, 0u};
    if (!installed_) return {Status::Unmapped, 0u};
    return {Status::PermissionDenied, 0u}; /* VRAMCNT registers are write-only */
}

Status NdsVramControl::write(
    NdsCpu cpu,
    uint32_t address,
    uint32_t width,
    uint32_t value) noexcept
{
    if (cpu != NDS_ARM9 && cpu != NDS_ARM7) return Status::InvalidCpu;

    if (address == VramStatAddress && cpu == NDS_ARM7) {
        if (width != 1u) return Status::InvalidWidth;
        if (!installed_) return Status::Unmapped;
        return Status::PermissionDenied;
    }

    /* Let NdsWramControl own ARM7 writes to its read-only WRAMSTAT register. */
    if (address == VramCntBAddress && cpu == NDS_ARM7) {
        return Status::Unmapped;
    }

    const int bank_index = bank_index_for_address(address);
    if (bank_index < 0) {
        /* GBATEK: LCDC VRAM ignores byte stores. ARM7 C/D Work RAM is the
         * documented exception and is deliberately left to GuestMemory. */
        if (cpu == NDS_ARM9 && width == 1u) {
            const int mapped_index = arm9_vram_bank_index_for_address(address);
            if (mapped_index >= 0) {
                return Status::Ok;
            }
        }
        return Status::Unmapped;
    }
    if (cpu != NDS_ARM9) return Status::CpuAccessDenied;
    if (width != 1u) return Status::InvalidWidth;
    if (!installed_) return Status::Unmapped;

    /* Bits 5-6 are reserved; preserve enable, OFS and MST only. */
    const uint8_t requested = static_cast<uint8_t>(value & UINT32_C(0x9F));
    std::array<uint8_t, 9> next_controls = controls_;
    next_controls[static_cast<std::size_t>(bank_index)] = requested;

    const Status result = apply_controls(next_controls);
    if (result == Status::Ok) controls_ = next_controls;
    return result;
}

} // namespace oracis::memory

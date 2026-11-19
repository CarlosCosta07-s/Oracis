#include "oracis/memory/nds_retail_map.hpp"

namespace oracis::memory {
namespace {

constexpr uint8_t kRamPermissions =
    static_cast<uint8_t>(Readable | Writable | Executable);

constexpr uint32_t kSharedWramBackingBase = UINT32_C(0xF0000000);
constexpr const char *kSharedWramBackingName =
    "NDS Shared WRAM physical banks (internal)";

bool valid_shared_wram_mode(SharedWramMode mode) noexcept
{
    switch (mode) {
    case SharedWramMode::Arm9All32K:
    case SharedWramMode::Arm9Bank1Arm7Bank0:
    case SharedWramMode::Arm9Bank0Arm7Bank1:
    case SharedWramMode::Arm7All32K:
        return true;
    }
    return false;
}

bool remove_shared_wram_views(GuestMemory &memory) noexcept
{
    bool removed = false;
    const char *const names[] = {
        "ARM9 Shared WRAM 32KB window",
        "ARM9 Shared WRAM bank 1 window",
        "ARM9 Shared WRAM bank 0 window",
        "ARM7 Shared WRAM bank 0 window",
        "ARM7 Shared WRAM bank 1 window",
        "ARM7 Shared WRAM 32KB window",
        "ARM7 private WRAM mirror in deallocated Shared WRAM window"
    };

    for (const char *name : names) {
        removed = memory.unmap_region(name) || removed;
    }
    return removed;
}

Status map_shared_wram_views(GuestMemory &memory, SharedWramMode mode)
{
    Status status = Status::Ok;

    switch (mode) {
    case SharedWramMode::Arm9All32K:
        status = memory.map_mirror(
            NdsRetailRamMap::SharedWramBase,
            kSharedWramBackingBase,
            NdsRetailRamMap::SharedWramSize,
            NdsRetailRamMap::Arm9SharedWramWindow,
            "ARM9 Shared WRAM 32KB window",
            CpuAccessArm9);
        if (status != Status::Ok) return status;

        /*
         * WRAMCNT mode 0 allocates no Shared WRAM to ARM7. Its window mirrors
         * ARM7-private WRAM at 0x03800000 instead.
         */
        return memory.map_mirror(
            NdsRetailRamMap::SharedWramBase,
            NdsRetailRamMap::Arm7WramBase,
            NdsRetailRamMap::Arm7WramSize,
            NdsRetailRamMap::Arm7SharedWramWindow,
            "ARM7 private WRAM mirror in deallocated Shared WRAM window",
            CpuAccessArm7);

    case SharedWramMode::Arm9Bank1Arm7Bank0:
        status = memory.map_mirror(
            NdsRetailRamMap::SharedWramBase,
            kSharedWramBackingBase + NdsRetailRamMap::SharedWramBankSize,
            NdsRetailRamMap::SharedWramBankSize,
            NdsRetailRamMap::Arm9SharedWramWindow,
            "ARM9 Shared WRAM bank 1 window",
            CpuAccessArm9);
        if (status != Status::Ok) return status;

        return memory.map_mirror(
            NdsRetailRamMap::SharedWramBase,
            kSharedWramBackingBase,
            NdsRetailRamMap::SharedWramBankSize,
            NdsRetailRamMap::Arm7SharedWramWindow,
            "ARM7 Shared WRAM bank 0 window",
            CpuAccessArm7);

    case SharedWramMode::Arm9Bank0Arm7Bank1:
        status = memory.map_mirror(
            NdsRetailRamMap::SharedWramBase,
            kSharedWramBackingBase,
            NdsRetailRamMap::SharedWramBankSize,
            NdsRetailRamMap::Arm9SharedWramWindow,
            "ARM9 Shared WRAM bank 0 window",
            CpuAccessArm9);
        if (status != Status::Ok) return status;

        return memory.map_mirror(
            NdsRetailRamMap::SharedWramBase,
            kSharedWramBackingBase + NdsRetailRamMap::SharedWramBankSize,
            NdsRetailRamMap::SharedWramBankSize,
            NdsRetailRamMap::Arm7SharedWramWindow,
            NdsRetailRamMap::Arm7SharedWramWindow,
            CpuAccessArm7);

    case SharedWramMode::Arm7All32K:
        return memory.map_mirror(
            NdsRetailRamMap::SharedWramBase,
            kSharedWramBackingBase,
            NdsRetailRamMap::SharedWramSize,
            NdsRetailRamMap::Arm7SharedWramWindow,
            "ARM7 Shared WRAM 32KB window",
            CpuAccessArm7);
    }

    return Status::InvalidRegion;
}

Status create_shared_wram_backing(GuestMemory &memory)
{
    return memory.map_ram(
        kSharedWramBackingBase,
        NdsRetailRamMap::SharedWramSize,
        kRamPermissions,
        kSharedWramBackingName,
        CpuAccessNone);
}

} // namespace

Status NdsRetailRamMap::install(
    GuestMemory &memory,
    SharedWramMode shared_wram_mode)
{
    if (memory.region_count() != 0u ||
        !valid_shared_wram_mode(shared_wram_mode)) {
        return Status::InvalidRegion;
    }

    Status status = memory.map_ram(
        MainRamBase,
        MainRamSize,
        kRamPermissions,
        "NDS retail main RAM (shared ARM9/ARM7)",
        CpuAccessBoth);
    if (status != Status::Ok) return status;

    status = memory.map_alias(
        MainRamMirror1, MainRamBase, MainRamSize,
        "NDS main RAM mirror 1", CpuAccessBoth);
    if (status != Status::Ok) return status;

    status = memory.map_alias(
        MainRamMirror2, MainRamBase, MainRamSize,
        "NDS main RAM mirror 2", CpuAccessBoth);
    if (status != Status::Ok) return status;

    status = memory.map_alias(
        MainRamMirror3, MainRamBase, MainRamSize,
        "NDS main RAM mirror 3", CpuAccessBoth);
    if (status != Status::Ok) return status;

    status = memory.map_ram(
        Arm7WramBase,
        Arm7WramSize,
        kRamPermissions,
        "NDS ARM7 private WRAM",
        CpuAccessArm7);
    if (status != Status::Ok) return status;

    status = create_shared_wram_backing(memory);
    if (status != Status::Ok) return status;

    return map_shared_wram_views(memory, shared_wram_mode);
}

Status NdsRetailRamMap::set_shared_wram_mode(
    GuestMemory &memory,
    SharedWramMode shared_wram_mode)
{
    if (!valid_shared_wram_mode(shared_wram_mode)) {
        return Status::InvalidRegion;
    }

    /*
     * Only the guest-visible windows are removed. The two physical banks at
     * kSharedWramBackingBase and ARM7-private WRAM remain mapped and retain
     * their bytes while access windows are rebuilt for the new WRAMCNT mode.
     */
    if (!remove_shared_wram_views(memory)) return Status::Unmapped;

    return map_shared_wram_views(memory, shared_wram_mode);
}

} // namespace oracis::memory

#include "oracis/memory/nds_retail_map.hpp"

#include <array>

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

std::array<GuestMemory::CpuAccessUpdate, 7>
shared_wram_access_updates(SharedWramMode mode) noexcept
{
    const auto access = [](bool enabled, uint8_t cpu) noexcept {
        return static_cast<uint8_t>(enabled ? cpu : CpuAccessNone);
    };

    return {{
        {"ARM9 Shared WRAM 32KB window",
         access(mode == SharedWramMode::Arm9All32K, CpuAccessArm9)},
        {"ARM9 Shared WRAM bank 1 window",
         access(mode == SharedWramMode::Arm9Bank1Arm7Bank0, CpuAccessArm9)},
        {"ARM9 Shared WRAM bank 0 window",
         access(mode == SharedWramMode::Arm9Bank0Arm7Bank1, CpuAccessArm9)},
        {"ARM7 Shared WRAM bank 0 window",
         access(mode == SharedWramMode::Arm9Bank1Arm7Bank0, CpuAccessArm7)},
        {"ARM7 Shared WRAM bank 1 window",
         access(mode == SharedWramMode::Arm9Bank0Arm7Bank1, CpuAccessArm7)},
        {"ARM7 Shared WRAM 32KB window",
         access(mode == SharedWramMode::Arm7All32K, CpuAccessArm7)},
        {"ARM7 private WRAM mirror in deallocated Shared WRAM window",
         access(mode == SharedWramMode::Arm9All32K, CpuAccessArm7)}
    }};
}

Status map_shared_wram_views(GuestMemory &memory, SharedWramMode mode)
{
    struct View {
        uint32_t alias_base;
        uint32_t canonical_base;
        uint32_t period;
        uint32_t window_size;
        const char *name;
        uint8_t cpu_access;
    };

    const auto updates = shared_wram_access_updates(mode);
    const View views[] = {
        {NdsRetailRamMap::SharedWramBase,
         kSharedWramBackingBase,
         NdsRetailRamMap::SharedWramSize,
         NdsRetailRamMap::Arm9SharedWramWindow,
         updates[0].name, updates[0].cpu_access},
        {NdsRetailRamMap::SharedWramBase,
         kSharedWramBackingBase + NdsRetailRamMap::SharedWramBankSize,
         NdsRetailRamMap::SharedWramBankSize,
         NdsRetailRamMap::Arm9SharedWramWindow,
         updates[1].name, updates[1].cpu_access},
        {NdsRetailRamMap::SharedWramBase,
         kSharedWramBackingBase,
         NdsRetailRamMap::SharedWramBankSize,
         NdsRetailRamMap::Arm9SharedWramWindow,
         updates[2].name, updates[2].cpu_access},
        {NdsRetailRamMap::SharedWramBase,
         kSharedWramBackingBase,
         NdsRetailRamMap::SharedWramBankSize,
         NdsRetailRamMap::Arm7SharedWramWindow,
         updates[3].name, updates[3].cpu_access},
        {NdsRetailRamMap::SharedWramBase,
         kSharedWramBackingBase + NdsRetailRamMap::SharedWramBankSize,
         NdsRetailRamMap::SharedWramBankSize,
         NdsRetailRamMap::Arm7SharedWramWindow,
         updates[4].name, updates[4].cpu_access},
        {NdsRetailRamMap::SharedWramBase,
         kSharedWramBackingBase,
         NdsRetailRamMap::SharedWramSize,
         NdsRetailRamMap::Arm7SharedWramWindow,
         updates[5].name, updates[5].cpu_access},
        {NdsRetailRamMap::SharedWramBase,
         NdsRetailRamMap::Arm7WramBase,
         NdsRetailRamMap::Arm7WramSize,
         NdsRetailRamMap::Arm7SharedWramWindow,
         updates[6].name, updates[6].cpu_access}
    };

    /*
     * Install every possible view once. Inactive mappings use CpuAccessNone;
     * later WRAMCNT writes only switch masks, never allocate or remove views.
     */
    for (const View &view : views) {
        const Status status = memory.map_mirror(
            view.alias_base,
            view.canonical_base,
            view.period,
            view.window_size,
            view.name,
            view.cpu_access);
        if (status != Status::Ok) return status;
    }

    return Status::Ok;
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
     * All possible views were allocated during install. Validate the full
     * resulting CPU map, then switch every visibility mask without allocation.
     */
    const auto updates = shared_wram_access_updates(shared_wram_mode);
    return memory.update_cpu_access(updates.data(), updates.size());
}

} // namespace oracis::memory

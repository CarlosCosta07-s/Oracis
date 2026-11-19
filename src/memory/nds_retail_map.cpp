#include "oracis/memory/nds_retail_map.hpp"

namespace oracis::memory {
namespace {

constexpr uint8_t kRamPermissions =
    static_cast<uint8_t>(Readable | Writable | Executable);

constexpr uint32_t kSharedWramBackingBase = UINT32_C(0xF0000000);

Status install_shared_wram(GuestMemory &memory, SharedWramMode mode)
{
    /*
     * Allocate the two physical 16KB banks at an internal, CPU-inaccessible
     * backing address. The guest sees only the mirrors installed below.
     */
    Status status = memory.map_ram(
        kSharedWramBackingBase,
        NdsRetailRamMap::SharedWramSize,
        kRamPermissions,
        "NDS Shared WRAM physical banks (internal)",
        CpuAccessNone);
    if (status != Status::Ok) return status;

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
         * WRAMCNT mode 0 allocates no Shared WRAM to ARM7. On the ARM7
         * window, the hardware instead mirrors its private 64KB WRAM.
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
            "ARM7 Shared WRAM bank 1 window",
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

} // namespace

Status NdsRetailRamMap::install(
    GuestMemory &memory,
    SharedWramMode shared_wram_mode)
{
    if (memory.region_count() != 0u) return Status::InvalidRegion;

    switch (shared_wram_mode) {
    case SharedWramMode::Arm9All32K:
    case SharedWramMode::Arm9Bank1Arm7Bank0:
    case SharedWramMode::Arm9Bank0Arm7Bank1:
    case SharedWramMode::Arm7All32K:
        break;
    default:
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

    return install_shared_wram(memory, shared_wram_mode);
}

} // namespace oracis::memory

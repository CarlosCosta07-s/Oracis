#include "oracis/memory/nds_video_memory.hpp"

namespace oracis::memory {
namespace {

constexpr uint8_t kVideoMemoryPermissions =
    static_cast<uint8_t>(Readable | Writable | Executable);
constexpr uint8_t kVideoMemoryCpuAccess = CpuAccessArm9;

constexpr const char *kPaletteRegionName = "NDS standard palette RAM";
constexpr const char *kOamRegionName = "NDS standard OAM";

} // namespace

bool NdsVideoMemory::contains_address(uint32_t address) noexcept
{
    const uint64_t palette_end =
        static_cast<uint64_t>(PaletteBase) + PaletteSize;
    const uint64_t oam_end =
        static_cast<uint64_t>(OamBase) + OamSize;
    return (address >= PaletteBase &&
            static_cast<uint64_t>(address) < palette_end) ||
           (address >= OamBase &&
            static_cast<uint64_t>(address) < oam_end);
}

Status NdsVideoMemory::install()
{
    if (installed_) return Status::InvalidRegion;

    const Status palette_status = memory_.map_ram(
        PaletteBase, PaletteSize, kVideoMemoryPermissions,
        kPaletteRegionName, kVideoMemoryCpuAccess);
    if (palette_status != Status::Ok) return palette_status;

    const Status oam_status = memory_.map_ram(
        OamBase, OamSize, kVideoMemoryPermissions,
        kOamRegionName, kVideoMemoryCpuAccess);
    if (oam_status != Status::Ok) {
        memory_.unmap_region(kPaletteRegionName);
        return oam_status;
    }

    installed_ = true;
    return Status::Ok;
}

Status NdsVideoMemory::write(
    NdsCpu cpu, uint32_t address, uint32_t width, uint32_t value) noexcept
{
    if (!installed_ || !contains_address(address)) {
        return Status::Unmapped;
    }
    if (cpu != NDS_ARM9 && cpu != NDS_ARM7) {
        return Status::InvalidCpu;
    }
    if (width != 1u && width != 2u && width != 4u) {
        return Status::InvalidWidth;
    }
    if (cpu != NDS_ARM9) return Status::CpuAccessDenied;

    /* NDS palette and OAM ignore STRB; this is not a read-modify-write. */
    if (width == 1u) return Status::Ok;

    return memory_.write(cpu, address, width, value);
}

} // namespace oracis::memory

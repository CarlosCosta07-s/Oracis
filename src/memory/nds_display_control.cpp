#include "oracis/memory/nds_display_control.hpp"

namespace oracis::memory {
namespace {

constexpr uint32_t kDispcntBytes = 4u;
constexpr uint32_t kBgRegisterBytes = NdsDisplayControl::BgCount * 2u;

} // namespace

bool NdsDisplayControl::valid_cpu(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 || cpu == NDS_ARM7;
}

bool NdsDisplayControl::locate_register(
    uint32_t address,
    unsigned &engine_index,
    uint32_t &register_offset) noexcept
{
    uint32_t base = 0u;
    if (address >= EngineABase &&
        address < EngineABase + UINT32_C(0x1000)) {
        engine_index = 0u;
        base = EngineABase;
    } else if (address >= EngineBBase &&
               address < EngineBBase + UINT32_C(0x1000)) {
        engine_index = 1u;
        base = EngineBBase;
    } else {
        return false;
    }

    register_offset = address - base;
    if (register_offset < kDispcntBytes) return true;
    return register_offset >= BgControlOffset &&
           register_offset < BgControlOffset + kBgRegisterBytes;
}

Status NdsDisplayControl::install() noexcept
{
    if (installed_) return Status::InvalidRegion;
    engines_ = {};
    installed_ = true;
    return Status::Ok;
}

ReadResult NdsDisplayControl::read(
    NdsCpu cpu, uint32_t address, uint32_t width) const noexcept
{
    unsigned engine_index = 0u;
    uint32_t offset = 0u;
    if (!locate_register(address, engine_index, offset)) {
        return {Status::Unmapped, 0u};
    }
    if (!installed_) return {Status::Unmapped, 0u};
    if (!valid_cpu(cpu)) return {Status::InvalidCpu, 0u};

    /*
     * The two 2D register blocks are in the ARM9 I/O map. The ARM7 uses a
     * different I/O map at these addresses; let its own devices inspect them.
     */
    if (cpu != NDS_ARM9) return {Status::Unmapped, 0u};

    const EngineState &engine = engines_[engine_index];
    if (offset == DispcntOffset) {
        if (width != 4u) return {Status::InvalidWidth, 0u};
        return {Status::Ok, engine.dispcnt};
    }

    const uint32_t bg_offset = offset - BgControlOffset;
    if ((bg_offset & 1u) == 0u && width == 2u) {
        const unsigned bg_index = static_cast<unsigned>(bg_offset / 2u);
        return {Status::Ok, engine.bgcnt[bg_index]};
    }
    if ((bg_offset & 3u) == 0u && width == 4u &&
        bg_offset + 4u <= kBgRegisterBytes) {
        const unsigned first_bg = static_cast<unsigned>(bg_offset / 2u);
        return {
            Status::Ok,
            static_cast<uint32_t>(engine.bgcnt[first_bg]) |
                (static_cast<uint32_t>(engine.bgcnt[first_bg + 1u]) << 16u)
        };
    }
    return {Status::InvalidWidth, 0u};
}

Status NdsDisplayControl::write(
    NdsCpu cpu,
    uint32_t address,
    uint32_t width,
    uint32_t value) noexcept
{
    unsigned engine_index = 0u;
    uint32_t offset = 0u;
    if (!locate_register(address, engine_index, offset)) {
        return Status::Unmapped;
    }
    if (!installed_) return Status::Unmapped;
    if (!valid_cpu(cpu)) return Status::InvalidCpu;
    if (cpu != NDS_ARM9) return Status::Unmapped;

    EngineState &engine = engines_[engine_index];
    if (offset == DispcntOffset) {
        if (width != 4u) return Status::InvalidWidth;
        engine.dispcnt = engine_index == 0u
            ? value
            : (value & EngineBDispcntSupportedMask);
        return Status::Ok;
    }

    const uint32_t bg_offset = offset - BgControlOffset;
    if ((bg_offset & 1u) == 0u && width == 2u) {
        const unsigned bg_index = static_cast<unsigned>(bg_offset / 2u);
        engine.bgcnt[bg_index] = static_cast<uint16_t>(value);
        return Status::Ok;
    }
    if ((bg_offset & 3u) == 0u && width == 4u &&
        bg_offset + 4u <= kBgRegisterBytes) {
        const unsigned first_bg = static_cast<unsigned>(bg_offset / 2u);
        engine.bgcnt[first_bg] = static_cast<uint16_t>(value);
        engine.bgcnt[first_bg + 1u] =
            static_cast<uint16_t>(value >> 16u);
        return Status::Ok;
    }
    return Status::InvalidWidth;
}

} // namespace oracis::memory

#include "oracis/memory/nds_external_memory_control.hpp"
namespace oracis::memory {
bool NdsExternalMemoryControl::valid_cpu(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 || cpu == NDS_ARM7;
}
unsigned NdsExternalMemoryControl::cpu_index(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 ? 0u : 1u;
}
ReadResult NdsExternalMemoryControl::read(
    NdsCpu cpu, uint32_t address, uint32_t width) const noexcept
{
    if (address != Address) return {Status::Unmapped, 0u};
    if (!valid_cpu(cpu)) return {Status::InvalidCpu, 0u};
    if (width != 2u) return {Status::InvalidWidth, 0u};
    const unsigned index = cpu_index(cpu);
    return {Status::Ok, static_cast<uint32_t>(
        (local_timing_[index] & GbaTimingMask) | shared_control_)};
}
Status NdsExternalMemoryControl::write(
    NdsCpu cpu, uint32_t address, uint32_t width, uint32_t value) noexcept
{
    if (address != Address) return Status::Unmapped;
    if (!valid_cpu(cpu)) return Status::InvalidCpu;
    if (width != 2u) return Status::InvalidWidth;
    local_timing_[cpu_index(cpu)] =
        static_cast<uint16_t>(value) & GbaTimingMask;
    /* Shared access-right and bus-mode fields are ARM9-owned. */
    if (cpu == NDS_ARM9) {
        shared_control_ = static_cast<uint16_t>(value) & SharedControlMask;
    }
    return Status::Ok;
}

bool NdsExternalMemoryControl::is_gba_slot_range(
    uint32_t address, uint32_t size) noexcept
{
    if (size == 0u || address < GbaSlotRomBase) return false;
    return static_cast<uint64_t>(address) + size <= GbaSlotEnd;
}

ReadResult NdsExternalMemoryControl::read_gba_slot(
    NdsCpu cpu, uint32_t address, uint32_t width,
    bool sequential_access) const noexcept
{
    if (!valid_cpu(cpu)) return {Status::InvalidCpu, 0u};
    if (width != 1u && width != 2u && width != 4u) {
        return {Status::InvalidWidth, 0u};
    }
    if (!is_gba_slot_range(address, width)) {
        return {Status::Unmapped, 0u};
    }

    /* EXMEMCNT.7 selects one owner; the non-owner sees a zero-filled bus. */
    const NdsCpu owner =
        (shared_control_ & GbaAccessArm7Mask) != 0u
            ? NDS_ARM7 : NDS_ARM9;
    if (cpu != owner) return {Status::Ok, 0u};

    const auto read_halfword = [this, cpu](
        uint32_t halfword_address, bool sequential) noexcept -> uint16_t {
        if (halfword_address >= GbaSlotRomBase &&
            halfword_address < GbaSlotSramBase) {
            /*
             * No GBA cartridge is mounted. GBATEK documents the selected
             * ROM bus as Addr/2, optionally ORed with FE08h at 10-cycle
             * first-access timing, or FFFFh at 18 cycles. Sequential timing
             * is 6/4 cycles and exposes Addr/2 without the first-access mask.
             */
            const uint16_t address_data = static_cast<uint16_t>(
                halfword_address >> 1u);
            if (sequential) return address_data;
            const uint16_t first_access =
                static_cast<uint16_t>((local_timing_[cpu_index(cpu)] >> 2u) & 3u);
            switch (first_access) {
            case 0u: return static_cast<uint16_t>(
                address_data | UINT16_C(0xFE08));
            case 1u:
            case 2u: return address_data;
            case 3u: return UINT16_MAX;
            }
        }
        /* The unpopulated SRAM aperture reads high-Z (all ones). */
        if (halfword_address >= GbaSlotSramBase &&
            halfword_address < GbaSlotEnd) {
            return UINT16_MAX;
        }
        return 0u;
    };

    const uint32_t aligned = address & ~UINT32_C(1);
    if (width == 1u) {
        const uint16_t halfword = read_halfword(aligned, sequential_access);
        const uint32_t shift = (address & 1u) * 8u;
        return {Status::Ok, static_cast<uint32_t>((halfword >> shift) & 0xFFu)};
    }
    if (width == 2u) {
        return {Status::Ok, read_halfword(aligned, sequential_access)};
    }

    const uint32_t word_aligned = address & ~UINT32_C(3);
    const uint16_t low = read_halfword(word_aligned, sequential_access);
    const uint16_t high = read_halfword(word_aligned + 2u, true);
    return {Status::Ok, static_cast<uint32_t>(low) |
        (static_cast<uint32_t>(high) << 16u)};
}

Status NdsExternalMemoryControl::write_gba_slot(
    NdsCpu cpu, uint32_t address, uint32_t width,
    uint32_t value) const noexcept
{
    (void)value;
    if (!valid_cpu(cpu)) return Status::InvalidCpu;
    if (width != 1u && width != 2u && width != 4u) {
        return Status::InvalidWidth;
    }
    if (!is_gba_slot_range(address, width)) return Status::Unmapped;

    /* This machine profile has no inserted GBA cartridge, so writes have no
       physical target and are ignored rather than creating synthetic RAM. */
    return Status::Ok;
}
} // namespace oracis::memory

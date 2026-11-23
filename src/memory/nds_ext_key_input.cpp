#include "oracis/memory/nds_ext_key_input.hpp"

namespace oracis::memory {

uint16_t NdsExtKeyInput::register_value() const noexcept
{
    /* Buttons and the pen are active-low; the hinge bit is active-high. */
    uint16_t value = UnusedHighBits;
    if ((pressed_buttons_ & HostButtonX) == 0u) value |= ButtonXBit;
    if ((pressed_buttons_ & HostButtonY) == 0u) value |= ButtonYBit;
    if ((pressed_buttons_ & HostButtonDebug) == 0u) value |= DebugButtonBit;
    if (!pen_down_) value |= PenBit;
    if (hinge_closed_) value |= HingeClosedBit;
    return value;
}

ReadResult NdsExtKeyInput::read(NdsCpu cpu,
                                uint32_t address,
                                uint32_t width) const noexcept
{
    if (address < RegisterAddress || address >= RegisterAddress + 2u) {
        return {Status::Unmapped, 0u};
    }
    if (cpu != NDS_ARM9 && cpu != NDS_ARM7) return {Status::InvalidCpu, 0u};
    if (cpu != NDS_ARM7) return {Status::CpuAccessDenied, 0u};

    const uint32_t offset = address - RegisterAddress;
    const uint16_t value = register_value();
    if (width == 2u) {
        if (offset != 0u) return {Status::InvalidWidth, 0u};
        return {Status::Ok, value};
    }
    if (width == 1u) {
        return {Status::Ok,
                static_cast<uint32_t>((value >> (8u * offset)) & 0xFFu)};
    }
    return {Status::InvalidWidth, 0u};
}

Status NdsExtKeyInput::write(NdsCpu cpu,
                             uint32_t address,
                             uint32_t width,
                             uint32_t value) noexcept
{
    (void)value;
    if (address < RegisterAddress || address >= RegisterAddress + 2u) {
        return Status::Unmapped;
    }
    if (cpu != NDS_ARM9 && cpu != NDS_ARM7) return Status::InvalidCpu;
    if (cpu != NDS_ARM7) return Status::CpuAccessDenied;
    if (width != 1u && width != 2u) return Status::InvalidWidth;
    return Status::PermissionDenied;
}

void NdsExtKeyInput::set_pressed_buttons(uint8_t host_mask) noexcept
{
    pressed_buttons_ = static_cast<uint8_t>(host_mask & HostButtonMask);
}

} // namespace oracis::memory

#include "oracis/memory/nds_keypad_control.hpp"

namespace oracis::memory {

bool NdsKeypadControl::valid_cpu(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 || cpu == NDS_ARM7;
}

unsigned NdsKeypadControl::cpu_index(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 ? 0u : 1u;
}

NdsCpu NdsKeypadControl::cpu_from_index(unsigned index) noexcept
{
    return index == 0u ? NDS_ARM9 : NDS_ARM7;
}

bool NdsKeypadControl::condition_matches(
    uint16_t control, uint16_t pressed_keys) noexcept
{
    const uint16_t selected = control & KeyMask;

    /* An empty selection does not represent a keypress condition. */
    if (selected == 0u) return false;

    const uint16_t pressed = pressed_keys & selected;
    if ((control & IrqAndMask) != 0u) {
        return pressed == selected;
    }
    return pressed != 0u;
}

void NdsKeypadControl::update_irq_line(unsigned index) noexcept
{
    CpuKeyControl &state = cpu_controls_[index];
    const bool enabled = (state.value & IrqEnableMask) != 0u;
    const bool matched = condition_matches(state.value, pressed_keys_);
    const bool active = enabled && matched;

    /*
     * Latch one request when the enabled KEYCNT condition becomes active.
     * Keeping the active state avoids generating repeated requests while a
     * guest holds the same key combination and acknowledges IF.
     */
    if (active && !state.irq_line_active) {
        (void)interrupts_.request_irq(
            cpu_from_index(index), KeypadIrqMask);
    }
    state.irq_line_active = active;
}

ReadResult NdsKeypadControl::read(
    NdsCpu cpu,
    uint32_t address,
    uint32_t width) const noexcept
{
    if (address != KeyInputAddress && address != KeyControlAddress) {
        return {Status::Unmapped, 0u};
    }
    if (!valid_cpu(cpu)) return {Status::InvalidCpu, 0u};
    if (width != 2u) return {Status::InvalidWidth, 0u};

    if (address == KeyInputAddress) {
        /* KEYINPUT bits 0-9 are active-low; reserved bits 10-15 read as 0. */
        const uint16_t released = static_cast<uint16_t>(
            KeyMask & static_cast<uint16_t>(~pressed_keys_));
        return {Status::Ok, static_cast<uint32_t>(released)};
    }

    return {
        Status::Ok,
        static_cast<uint32_t>(cpu_controls_[cpu_index(cpu)].value)
    };
}

Status NdsKeypadControl::write(
    NdsCpu cpu,
    uint32_t address,
    uint32_t width,
    uint32_t value) noexcept
{
    if (address != KeyInputAddress && address != KeyControlAddress) {
        return Status::Unmapped;
    }
    if (!valid_cpu(cpu)) return Status::InvalidCpu;
    if (width != 2u) return Status::InvalidWidth;
    if (address == KeyInputAddress) return Status::PermissionDenied;

    CpuKeyControl &state = cpu_controls_[cpu_index(cpu)];
    state.value = static_cast<uint16_t>(value & KeyControlMask);
    update_irq_line(cpu_index(cpu));
    return Status::Ok;
}

void NdsKeypadControl::set_pressed_keys(uint16_t pressed_keys) noexcept
{
    pressed_keys_ = static_cast<uint16_t>(pressed_keys & KeyMask);
    update_irq_line(0u);
    update_irq_line(1u);
}

uint16_t NdsKeypadControl::key_control(NdsCpu cpu) const noexcept
{
    if (!valid_cpu(cpu)) return 0u;
    return cpu_controls_[cpu_index(cpu)].value;
}

} // namespace oracis::memory

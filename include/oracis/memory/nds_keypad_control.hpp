/*
 * Nintendo DS keypad input and interrupt-control device.
 *
 * The ten physical buttons are shared by ARM9 and ARM7. KEYCNT and its
 * keypad IRQ condition are independently maintained for each processor.
 */
#ifndef ORACIS_MEMORY_NDS_KEYPAD_CONTROL_HPP
#define ORACIS_MEMORY_NDS_KEYPAD_CONTROL_HPP

#include <array>
#include <cstdint>

#include "oracis/memory/guest_memory.hpp"
#include "oracis/memory/nds_interrupt_control.hpp"

namespace oracis::memory {

class NdsKeypadControl final {
public:
    static constexpr uint32_t KeyInputAddress = UINT32_C(0x04000130);
    static constexpr uint32_t KeyControlAddress = UINT32_C(0x04000132);

    static constexpr uint16_t KeyMask = UINT16_C(0x03FF);
    static constexpr uint16_t KeyControlMask = UINT16_C(0xC3FF);
    static constexpr uint16_t IrqEnableMask = UINT16_C(1) << 14u;
    static constexpr uint16_t IrqAndMask = UINT16_C(1) << 15u;
    static constexpr uint32_t KeypadIrqMask = UINT32_C(1) << 12u;

    explicit NdsKeypadControl(
        GuestMemory &memory,
        NdsInterruptControl &interrupts) noexcept
        : memory_(memory), interrupts_(interrupts) {}

    NdsKeypadControl(const NdsKeypadControl &) = delete;
    NdsKeypadControl &operator=(const NdsKeypadControl &) = delete;

    ReadResult read(NdsCpu cpu,
                    uint32_t address,
                    uint32_t width) const noexcept;

    Status write(NdsCpu cpu,
                 uint32_t address,
                 uint32_t width,
                 uint32_t value) noexcept;

    /*
     * Host-facing input update. Bits 0-9 use a host-friendly active-high
     * representation (1 = pressed); hardware KEYINPUT reads them active-low.
     */
    void set_pressed_keys(uint16_t pressed_keys) noexcept;

    uint16_t pressed_keys() const noexcept { return pressed_keys_; }
    uint16_t key_control(NdsCpu cpu) const noexcept;

    GuestMemory &backing_memory() const noexcept { return memory_; }
    NdsInterruptControl &interrupt_control() const noexcept {
        return interrupts_;
    }

private:
    struct CpuKeyControl {
        uint16_t value = 0u;
        bool irq_line_active = false;
    };

    static bool valid_cpu(NdsCpu cpu) noexcept;
    static unsigned cpu_index(NdsCpu cpu) noexcept;
    static NdsCpu cpu_from_index(unsigned index) noexcept;

    static bool condition_matches(
        uint16_t control, uint16_t pressed_keys) noexcept;

    void update_irq_line(unsigned index) noexcept;

    GuestMemory &memory_;
    NdsInterruptControl &interrupts_;
    uint16_t pressed_keys_ = 0u;
    std::array<CpuKeyControl, 2> cpu_controls_{};
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_KEYPAD_CONTROL_HPP */

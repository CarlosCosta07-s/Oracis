/*
 * Nintendo DS extended key input (EXTKEYIN / REG_KEYXY), ARM7 only.
 *
 * The register at 0x04000136 carries the buttons that are not part of
 * KEYINPUT plus the pen and lid state:
 *   bit 0 X button (0 = pressed)      bit 6 pen (0 = touching the screen)
 *   bit 1 Y button (0 = pressed)      bit 7 hinge (1 = closed)
 *   bit 3 debug button (0 = pressed)  bits 2, 4, 5 unused, read as 1
 * Bits 8-15 read as zero. The unused-bit values and the open-hinge default
 * follow the public hardware description and are not yet checked against
 * hardware captures.
 *
 * The register is read-only; writes are refused like the keypad's KEYINPUT.
 * The pen bit only reports contact. Coordinates travel through the SPI
 * touchscreen controller (NdsSpiControl).
 */
#ifndef ORACIS_MEMORY_NDS_EXT_KEY_INPUT_HPP
#define ORACIS_MEMORY_NDS_EXT_KEY_INPUT_HPP

#include <cstdint>

#include "oracis/memory/guest_memory.hpp"

namespace oracis::memory {

class NdsExtKeyInput final {
public:
    static constexpr uint32_t RegisterAddress = UINT32_C(0x04000136);

    static constexpr uint16_t ButtonXBit = UINT16_C(1) << 0u;
    static constexpr uint16_t ButtonYBit = UINT16_C(1) << 1u;
    static constexpr uint16_t DebugButtonBit = UINT16_C(1) << 3u;
    static constexpr uint16_t PenBit = UINT16_C(1) << 6u;
    static constexpr uint16_t HingeClosedBit = UINT16_C(1) << 7u;
    static constexpr uint16_t UnusedHighBits = UINT16_C(0x0034);

    /* Host-facing masks (1 = pressed) for set_pressed_buttons(). */
    static constexpr uint8_t HostButtonX = 0x01u;
    static constexpr uint8_t HostButtonY = 0x02u;
    static constexpr uint8_t HostButtonDebug = 0x04u;
    static constexpr uint8_t HostButtonMask = 0x07u;

    NdsExtKeyInput() noexcept = default;

    NdsExtKeyInput(const NdsExtKeyInput &) = delete;
    NdsExtKeyInput &operator=(const NdsExtKeyInput &) = delete;

    ReadResult read(NdsCpu cpu,
                    uint32_t address,
                    uint32_t width) const noexcept;

    Status write(NdsCpu cpu,
                 uint32_t address,
                 uint32_t width,
                 uint32_t value) noexcept;

    void set_pressed_buttons(uint8_t host_mask) noexcept;
    void set_pen_down(bool down) noexcept { pen_down_ = down; }
    void set_hinge_closed(bool closed) noexcept { hinge_closed_ = closed; }

    uint8_t pressed_buttons() const noexcept { return pressed_buttons_; }
    bool pen_down() const noexcept { return pen_down_; }
    bool hinge_closed() const noexcept { return hinge_closed_; }

    /* Value the guest sees when it reads the full 16-bit register. */
    uint16_t register_value() const noexcept;

private:
    uint8_t pressed_buttons_ = 0u;
    bool pen_down_ = false;
    bool hinge_closed_ = false;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_EXT_KEY_INPUT_HPP */

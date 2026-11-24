/*
 * Nintendo DS serial peripheral interface (SPICNT/SPIDATA), ARM7 only.
 *
 * One SPI bus is shared by three devices selected through SPICNT bits 8-9:
 *   0 power management, 1 firmware flash, 2 touchscreen controller.
 *
 * Byte semantics are resolved when SPIDATA is written; the result becomes
 * visible and the optional IRQ fires after the transfer time (8 or 16 bits at
 * bus clock / 8, 16, 32 or 64 per bit) has elapsed through
 * advance_bus_cycles(). Writing SPIDATA while busy is refused instead of
 * guessed. Anything the devices may accept on hardware but Oracis does not
 * model (flash programming/erase, JEDEC ID, overlapped touchscreen
 * conversions, reserved device 3) returns Status::Unsupported with a
 * diagnostic so a boot never continues on invented data.
 *
 * Firmware contents are never embedded. The host supplies the user's own
 * image with set_firmware_image(); without one every firmware access is
 * refused. The flash is read-only: READ (03h), RDSR (05h), WREN (06h) and
 * WRDI (04h) are modeled.
 *
 * Touchscreen: a control byte with bit 7 set starts a conversion and the next
 * two bytes return the result MSB first after one null bit (12-bit mode) or
 * the 8-bit equivalent. Pen-up readings (X = 0, Y = 0xFFF) and Z/temperature
 * channel behavior are assumptions pending hardware captures; power-down and
 * differential/single-ended mode bits are ignored.
 */
#ifndef ORACIS_MEMORY_NDS_SPI_CONTROL_HPP
#define ORACIS_MEMORY_NDS_SPI_CONTROL_HPP

#include <array>
#include <cstddef>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "oracis/memory/guest_memory.hpp"
#include "oracis/memory/nds_interrupt_control.hpp"

namespace oracis::memory {

class NdsSpiControl final {
public:
    static constexpr uint32_t ControlAddress = UINT32_C(0x040001C0);
    static constexpr uint32_t DataAddress = UINT32_C(0x040001C2);

    static constexpr uint16_t BaudMask = UINT16_C(0x0003);
    static constexpr uint16_t BusyBit = UINT16_C(1) << 7u;
    static constexpr unsigned DeviceShift = 8u;
    static constexpr uint16_t DeviceMask = UINT16_C(0x0300);
    static constexpr uint16_t Size16Bit = UINT16_C(1) << 10u;
    static constexpr uint16_t HoldBit = UINT16_C(1) << 11u;
    static constexpr uint16_t IrqBit = UINT16_C(1) << 14u;
    static constexpr uint16_t EnableBit = UINT16_C(1) << 15u;
    static constexpr uint16_t ControlWriteMask = UINT16_C(0xCF03);
    static constexpr uint32_t SpiIrqMask = UINT32_C(1) << 23u;

    static constexpr uint8_t DevicePower = 0u;
    static constexpr uint8_t DeviceFirmware = 1u;
    static constexpr uint8_t DeviceTouch = 2u;

    static constexpr size_t MaxFirmwareBytes = size_t{1} << 24u;
    static constexpr uint8_t PowerRegisterCount = 5u;

    explicit NdsSpiControl(NdsInterruptControl &interrupts) noexcept
        : interrupts_(interrupts) {}

    NdsSpiControl(const NdsSpiControl &) = delete;
    NdsSpiControl &operator=(const NdsSpiControl &) = delete;

    ReadResult read(NdsCpu cpu, uint32_t address, uint32_t width) const noexcept;
    Status write(NdsCpu cpu, uint32_t address, uint32_t width,
                 uint32_t value) noexcept;

    /* Counts bus cycles toward the completion of the active transfer. */
    void advance_bus_cycles(uint64_t cycles) noexcept;

    /* Aborts an in-flight transfer and releases the selected device. */
    void reset_transfer() noexcept;

    /* Host-provided firmware image (the user's own dump). Copied on success. */
    Status set_firmware_image(const uint8_t *data, size_t size) noexcept;
    void clear_firmware_image() noexcept;
    bool firmware_loaded() const noexcept { return !firmware_.empty(); }
    const uint8_t *firmware_data() const noexcept {
        return firmware_.empty() ? nullptr : firmware_.data();
    }
    std::size_t firmware_size() const noexcept { return firmware_.size(); }

    /* Raw 12-bit touchscreen ADC coordinates; pen-up reads X=0, Y=0xFFF. */
    void set_touch(bool pressed, uint16_t x12, uint16_t y12) noexcept;
    /* Other ADC channels (0 temp0, 3 Z1, 4 Z2, 6 AUX, 7 temp1); 1/5 use set_touch. */
    Status set_adc_channel(uint8_t channel, uint16_t value12) noexcept;

    /* Battery status bit of power register 1: true reports a low battery. */
    void set_battery_low(bool low) noexcept;
    uint8_t power_register(uint8_t index) const noexcept;
    /* Latched when the guest sets the system power-off bit (register 0 bit 6). */
    bool power_off_requested() const noexcept { return power_off_requested_; }

    uint16_t control() const noexcept {
        return static_cast<uint16_t>(
            control_ | (busy_ ? BusyBit : UINT16_C(0)));
    }
    uint16_t data() const noexcept { return data_; }
    bool busy() const noexcept { return busy_; }

    /* Last rejected request; nullptr when nothing was rejected. */
    const char *last_diagnostic() const noexcept { return diagnostic_; }

    NdsInterruptControl &interrupt_control() const noexcept {
        return interrupts_;
    }

private:
    static constexpr int NoDevice = -1;

    enum class FlashPhase : uint8_t { Command, Address, Reading, StatusRead };
    enum class PowerPhase : uint8_t { Command, Data };
    enum class TouchPhase : uint8_t { Control, HighByte, LowByte };

    Status start_transfer(uint16_t value) noexcept;
    Status process_byte(uint8_t device, uint8_t input,
                        uint8_t &output) noexcept;
    Status process_flash(uint8_t input, uint8_t &output) noexcept;
    Status process_power(uint8_t input, uint8_t &output) noexcept;
    Status process_touch(uint8_t input, uint8_t &output) noexcept;
    uint16_t touch_sample(uint8_t channel) const noexcept;
    void release_device(int device) noexcept;
    void abort_transfer() noexcept;

    NdsInterruptControl &interrupts_;

    uint16_t control_ = 0u;
    uint16_t data_ = 0u;
    uint16_t pending_data_ = 0u;
    bool busy_ = false;
    uint64_t remaining_cycles_ = 0u;
    int open_device_ = NoDevice;

    /* Firmware flash. */
    std::vector<uint8_t> firmware_;
    FlashPhase flash_phase_ = FlashPhase::Command;
    uint32_t flash_address_ = 0u;
    uint8_t flash_address_bytes_ = 0u;
    bool flash_write_enabled_ = false;

    /* Power management. */
    PowerPhase power_phase_ = PowerPhase::Command;
    uint8_t power_index_ = 0u;
    bool power_read_ = false;
    std::array<uint8_t, PowerRegisterCount> power_registers_{};
    bool power_off_requested_ = false;

    /* Touchscreen controller. */
    TouchPhase touch_phase_ = TouchPhase::Control;
    uint16_t touch_result_ = 0u;
    bool touch_eight_bit_ = false;
    bool pen_down_ = false;
    uint16_t touch_x_ = 0u;
    uint16_t touch_y_ = 0xFFFu;
    std::array<uint16_t, 8> adc_channels_{};

    const char *diagnostic_ = nullptr;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_SPI_CONTROL_HPP */

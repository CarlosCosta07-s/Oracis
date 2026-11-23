#include "oracis/memory/nds_spi_control.hpp"

namespace oracis::memory {

namespace {

constexpr uint32_t SpiBase = NdsSpiControl::ControlAddress;
constexpr uint32_t SpiEnd = NdsSpiControl::DataAddress + 2u;

bool valid_cpu(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 || cpu == NDS_ARM7;
}

} // namespace

ReadResult NdsSpiControl::read(NdsCpu cpu, uint32_t address,
                               uint32_t width) const noexcept
{
    if (address < SpiBase || address >= SpiEnd) return {Status::Unmapped, 0u};
    if (!valid_cpu(cpu)) return {Status::InvalidCpu, 0u};
    if (cpu != NDS_ARM7) return {Status::CpuAccessDenied, 0u};

    const uint32_t offset = address - SpiBase;
    const uint16_t reg = offset < 2u ? control() : data_;
    if (width == 2u) {
        if ((offset & 1u) != 0u) return {Status::InvalidWidth, 0u};
        return {Status::Ok, reg};
    }
    if (width == 1u) {
        const unsigned lane = offset & 1u;
        return {Status::Ok, static_cast<uint32_t>((reg >> (8u * lane)) & 0xFFu)};
    }
    return {Status::InvalidWidth, 0u};
}

Status NdsSpiControl::write(NdsCpu cpu, uint32_t address, uint32_t width,
                            uint32_t value) noexcept
{
    if (address < SpiBase || address >= SpiEnd) return Status::Unmapped;
    if (!valid_cpu(cpu)) return Status::InvalidCpu;
    if (cpu != NDS_ARM7) return Status::CpuAccessDenied;

    const uint32_t offset = address - SpiBase;
    if (width != 1u && width != 2u) return Status::InvalidWidth;
    if (width == 2u && (offset & 1u) != 0u) return Status::InvalidWidth;

    if (offset < 2u) {
        uint16_t next = 0u;
        if (width == 2u) {
            next = static_cast<uint16_t>(value);
        } else if (offset == 0u) {
            next = static_cast<uint16_t>(
                (control_ & 0xFF00u) | (value & 0x00FFu));
        } else {
            next = static_cast<uint16_t>(
                (control_ & 0x00FFu) | ((value & 0x00FFu) << 8u));
        }
        const bool was_enabled = (control_ & EnableBit) != 0u;
        control_ = static_cast<uint16_t>(next & ControlWriteMask);
        if (was_enabled && (control_ & EnableBit) == 0u) abort_transfer();
        return Status::Ok;
    }

    /* SPIDATA: a byte write is only defined on the low byte. */
    if (width == 1u && offset != 2u) return Status::InvalidWidth;
    return start_transfer(static_cast<uint16_t>(
        width == 1u ? (value & 0xFFu) : (value & 0xFFFFu)));
}

void NdsSpiControl::advance_bus_cycles(uint64_t cycles) noexcept
{
    if (!busy_) return;
    if (cycles < remaining_cycles_) {
        remaining_cycles_ -= cycles;
        return;
    }
    busy_ = false;
    remaining_cycles_ = 0u;
    data_ = pending_data_;
    if ((control_ & IrqBit) != 0u) {
        (void)interrupts_.request_irq(NDS_ARM7, SpiIrqMask);
    }
}

void NdsSpiControl::release_device(int device) noexcept
{
    switch (device) {
    case DeviceFirmware:
        flash_phase_ = FlashPhase::Command;
        flash_address_ = 0u;
        flash_address_bytes_ = 0u;
        break;
    case DevicePower:
        power_phase_ = PowerPhase::Command;
        break;
    case DeviceTouch:
        touch_phase_ = TouchPhase::Control;
        break;
    default:
        break;
    }
}

void NdsSpiControl::abort_transfer() noexcept
{
    busy_ = false;
    remaining_cycles_ = 0u;
    release_device(open_device_);
    open_device_ = NoDevice;
}

void NdsSpiControl::reset_transfer() noexcept
{
    abort_transfer();
}

Status NdsSpiControl::start_transfer(uint16_t value) noexcept
{
    /* With the bus disabled SPIDATA writes do not start anything. */
    if ((control_ & EnableBit) == 0u) return Status::Ok;

    if (busy_) {
        diagnostic_ = "SPIDATA written while an SPI transfer is in progress";
        return Status::PermissionDenied;
    }

    const uint8_t device = static_cast<uint8_t>(
        (control_ & DeviceMask) >> DeviceShift);
    if (device > DeviceTouch) {
        diagnostic_ = "SPI device 3 is reserved and not modeled";
        return Status::Unsupported;
    }

    /* Moving chip select to another device ends the previous sequence. */
    if (open_device_ != NoDevice && open_device_ != device) {
        release_device(open_device_);
        open_device_ = NoDevice;
    }

    const bool wide = (control_ & Size16Bit) != 0u;
    uint16_t result = 0u;
    uint8_t high = 0u;
    uint8_t low = 0u;
    Status status = Status::Ok;
    if (wide) {
        status = process_byte(device, static_cast<uint8_t>(value >> 8u), high);
        if (status == Status::Ok) {
            status = process_byte(device, static_cast<uint8_t>(value), low);
        }
        result = static_cast<uint16_t>((high << 8u) | low);
    } else {
        status = process_byte(device, static_cast<uint8_t>(value), low);
        result = low;
    }
    if (status != Status::Ok) {
        release_device(device);
        open_device_ = NoDevice;
        return status;
    }

    if ((control_ & HoldBit) != 0u) {
        open_device_ = device;
    } else {
        release_device(device);
        open_device_ = NoDevice;
    }

    const unsigned bits = wide ? 16u : 8u;
    const unsigned cycles_per_bit = 8u << (control_ & BaudMask);
    busy_ = true;
    pending_data_ = result;
    remaining_cycles_ = static_cast<uint64_t>(bits) * cycles_per_bit;
    return Status::Ok;
}

Status NdsSpiControl::process_byte(uint8_t device, uint8_t input,
                                   uint8_t &output) noexcept
{
    output = 0u;
    switch (device) {
    case DevicePower: return process_power(input, output);
    case DeviceFirmware: return process_flash(input, output);
    default: return process_touch(input, output);
    }
}

Status NdsSpiControl::process_flash(uint8_t input, uint8_t &output) noexcept
{
    if (firmware_.empty()) {
        diagnostic_ = "firmware image was not provided by the host";
        return Status::Unsupported;
    }

    switch (flash_phase_) {
    case FlashPhase::Command:
        switch (input) {
        case 0x03u:
            flash_phase_ = FlashPhase::Address;
            flash_address_ = 0u;
            flash_address_bytes_ = 0u;
            return Status::Ok;
        case 0x05u:
            flash_phase_ = FlashPhase::StatusRead;
            return Status::Ok;
        case 0x06u:
            flash_write_enabled_ = true;
            return Status::Ok;
        case 0x04u:
            flash_write_enabled_ = false;
            return Status::Ok;
        default:
            diagnostic_ =
                "firmware flash command is not modeled (read-only: 03h, 05h, 04h, 06h)";
            return Status::Unsupported;
        }
    case FlashPhase::Address:
        flash_address_ = (flash_address_ << 8u) | input;
        if (++flash_address_bytes_ == 3u) flash_phase_ = FlashPhase::Reading;
        return Status::Ok;
    case FlashPhase::Reading:
        if (flash_address_ >= firmware_.size()) {
            diagnostic_ = "firmware read ran past the end of the supplied image";
            return Status::Unsupported;
        }
        output = firmware_[flash_address_++];
        return Status::Ok;
    case FlashPhase::StatusRead:
        /* Write-in-progress is never set because programming is not modeled. */
        output = flash_write_enabled_ ? 0x02u : 0x00u;
        return Status::Ok;
    }
    return Status::Unsupported;
}

Status NdsSpiControl::process_power(uint8_t input, uint8_t &output) noexcept
{
    if (power_phase_ == PowerPhase::Command) {
        const uint8_t index = static_cast<uint8_t>(input & 0x7Fu);
        if (index >= PowerRegisterCount) {
            diagnostic_ = "power-management register index is not modeled";
            return Status::Unsupported;
        }
        power_index_ = index;
        power_read_ = (input & 0x80u) != 0u;
        power_phase_ = PowerPhase::Data;
        return Status::Ok;
    }

    power_phase_ = PowerPhase::Command;
    if (power_read_) {
        output = power_registers_[power_index_];
        return Status::Ok;
    }
    /* Register 1 reports battery status and ignores writes. */
    if (power_index_ != 1u) {
        power_registers_[power_index_] = input;
        if (power_index_ == 0u && (input & 0x40u) != 0u) {
            power_off_requested_ = true;
        }
    }
    return Status::Ok;
}

uint16_t NdsSpiControl::touch_sample(uint8_t channel) const noexcept
{
    switch (channel) {
    case 1u: return pen_down_ ? touch_y_ : 0x0FFFu;
    case 5u: return pen_down_ ? touch_x_ : 0x0000u;
    default: return static_cast<uint16_t>(adc_channels_[channel] & 0x0FFFu);
    }
}

Status NdsSpiControl::process_touch(uint8_t input, uint8_t &output) noexcept
{
    if (touch_phase_ == TouchPhase::Control) {
        /* Clocks without a start bit leave the converter idle. */
        if ((input & 0x80u) == 0u) return Status::Ok;
        touch_eight_bit_ = (input & 0x08u) != 0u;
        touch_result_ = touch_sample(static_cast<uint8_t>((input >> 4u) & 0x07u));
        touch_phase_ = TouchPhase::HighByte;
        return Status::Ok;
    }

    if ((input & 0x80u) != 0u) {
        diagnostic_ = "touchscreen conversion overlapped by a new control byte";
        touch_phase_ = TouchPhase::Control;
        return Status::Unsupported;
    }

    if (touch_phase_ == TouchPhase::HighByte) {
        output = touch_eight_bit_
            ? static_cast<uint8_t>(((touch_result_ >> 4u) >> 1u) & 0x7Fu)
            : static_cast<uint8_t>((touch_result_ >> 5u) & 0x7Fu);
        touch_phase_ = TouchPhase::LowByte;
        return Status::Ok;
    }

    output = touch_eight_bit_
        ? static_cast<uint8_t>(((touch_result_ >> 4u) << 7u) & 0x80u)
        : static_cast<uint8_t>((touch_result_ << 3u) & 0xF8u);
    touch_phase_ = TouchPhase::Control;
    return Status::Ok;
}

Status NdsSpiControl::set_firmware_image(const uint8_t *data, size_t size) noexcept
{
    if (data == nullptr || size == 0u || size > MaxFirmwareBytes) {
        diagnostic_ = "firmware image is empty or exceeds the 24-bit flash address space";
        return Status::InvalidRegion;
    }
    try {
        firmware_.assign(data, data + size);
    } catch (...) {
        firmware_.clear();
        diagnostic_ = "firmware image could not be allocated";
        return Status::InvalidRegion;
    }
    flash_write_enabled_ = false;
    if (open_device_ == DeviceFirmware) {
        release_device(DeviceFirmware);
        open_device_ = NoDevice;
    }
    return Status::Ok;
}

void NdsSpiControl::clear_firmware_image() noexcept
{
    firmware_.clear();
    flash_write_enabled_ = false;
    if (open_device_ == DeviceFirmware) {
        release_device(DeviceFirmware);
        open_device_ = NoDevice;
    }
}

void NdsSpiControl::set_touch(bool pressed, uint16_t x12, uint16_t y12) noexcept
{
    pen_down_ = pressed;
    touch_x_ = static_cast<uint16_t>(x12 & 0x0FFFu);
    touch_y_ = static_cast<uint16_t>(y12 & 0x0FFFu);
}

Status NdsSpiControl::set_adc_channel(uint8_t channel, uint16_t value12) noexcept
{
    if (channel > 7u) return Status::InvalidRegion;
    if (channel == 1u || channel == 5u) {
        diagnostic_ = "ADC channels 1 and 5 are driven by set_touch()";
        return Status::Unsupported;
    }
    adc_channels_[channel] = static_cast<uint16_t>(value12 & 0x0FFFu);
    return Status::Ok;
}

void NdsSpiControl::set_battery_low(bool low) noexcept
{
    power_registers_[1] = low ? 0x01u : 0x00u;
}

uint8_t NdsSpiControl::power_register(uint8_t index) const noexcept
{
    return index < PowerRegisterCount ? power_registers_[index] : 0u;
}

} // namespace oracis::memory

#include "oracis/memory/nds_interrupt_control.hpp"
#include "oracis/memory/nds_spi_control.hpp"
#include "oracis/recomp/runtime_arm.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using oracis::memory::GuestMemory;
using oracis::memory::NdsInterruptControl;
using oracis::memory::NdsSpiControl;
using oracis::memory::Status;

int fail(const char *message)
{
    std::fprintf(stderr, "nds_spi_control_test: %s\n", message);
    return 1;
}

constexpr uint8_t Power = NdsSpiControl::DevicePower;
constexpr uint8_t Firmware = NdsSpiControl::DeviceFirmware;
constexpr uint8_t Touch = NdsSpiControl::DeviceTouch;

/* Drives SPICNT/SPIDATA like ARM7 code and lets the transfer complete. */
class Driver {
public:
    explicit Driver(NdsSpiControl &spi) : spi_(spi) {}

    uint16_t control(uint8_t device, bool hold, bool wide = false,
                     unsigned baud = 0u, bool irq = false) const
    {
        return static_cast<uint16_t>(
            NdsSpiControl::EnableBit |
            (static_cast<uint16_t>(device) << NdsSpiControl::DeviceShift) |
            (hold ? NdsSpiControl::HoldBit : 0u) |
            (wide ? NdsSpiControl::Size16Bit : 0u) |
            (irq ? NdsSpiControl::IrqBit : 0u) | baud);
    }

    Status set_control(uint16_t value)
    {
        return spi_.write(NDS_ARM7, NdsSpiControl::ControlAddress, 2u, value);
    }

    Status start(uint16_t data)
    {
        return spi_.write(NDS_ARM7, NdsSpiControl::DataAddress, 2u, data);
    }

    /* One completed transfer; returns the SPIDATA result. */
    uint16_t transfer(uint8_t device, uint16_t data, bool hold,
                      bool wide = false)
    {
        last = set_control(control(device, hold, wide));
        if (last != Status::Ok) return 0u;
        last = start(data);
        if (last != Status::Ok) return 0u;
        spi_.advance_bus_cycles(1024u);
        return spi_.read(NDS_ARM7, NdsSpiControl::DataAddress, 2u).value;
    }

    uint8_t byte(uint8_t device, uint8_t data, bool hold)
    {
        return static_cast<uint8_t>(transfer(device, data, hold));
    }

    Status last = Status::Ok;

private:
    NdsSpiControl &spi_;
};

std::vector<uint8_t> make_image(size_t size)
{
    std::vector<uint8_t> image(size);
    for (size_t i = 0u; i < size; ++i) {
        image[i] = static_cast<uint8_t>((i * 7u) ^ 0x5Au);
    }
    return image;
}

/* Reads `count` bytes of firmware from `address` with the 03h command. */
bool flash_read(Driver &driver, uint32_t address, std::vector<uint8_t> &out,
                size_t count)
{
    (void)driver.byte(Firmware, 0x03u, true);
    (void)driver.byte(Firmware, static_cast<uint8_t>(address >> 16u), true);
    (void)driver.byte(Firmware, static_cast<uint8_t>(address >> 8u), true);
    (void)driver.byte(Firmware, static_cast<uint8_t>(address), true);
    out.clear();
    for (size_t i = 0u; i < count; ++i) {
        out.push_back(driver.byte(Firmware, 0u, i + 1u < count));
        if (driver.last != Status::Ok) return false;
    }
    return true;
}

uint16_t touch_read(Driver &driver, uint8_t control_byte)
{
    (void)driver.byte(Touch, control_byte, true);
    const uint8_t high = driver.byte(Touch, 0u, true);
    const uint8_t low = driver.byte(Touch, 0u, false);
    if ((control_byte & 0x08u) != 0u) {
        return static_cast<uint16_t>((high << 1u) | (low >> 7u));
    }
    return static_cast<uint16_t>((high << 5u) | (low >> 3u));
}

} // namespace

int main()
{
    GuestMemory memory;
    NdsInterruptControl interrupts(memory);
    NdsSpiControl spi(interrupts);
    Driver driver(spi);

    /* Reset state. */
    if (spi.control() != 0u || spi.data() != 0u || spi.busy()) {
        return fail("SPI did not initialize idle");
    }

    /* Writable mask and read-only busy bit. */
    (void)driver.set_control(0xFFFFu);
    if (spi.control() != NdsSpiControl::ControlWriteMask) {
        return fail("SPICNT write mask is wrong");
    }
    (void)driver.set_control(0u);

    /* Byte-lane access to SPICNT. */
    (void)spi.write(NDS_ARM7, NdsSpiControl::ControlAddress + 1u, 1u, 0x81u);
    if (spi.control() != 0x8100u ||
        spi.read(NDS_ARM7, NdsSpiControl::ControlAddress + 1u, 1u).value != 0x81u) {
        return fail("SPICNT high-byte lane access is wrong");
    }
    (void)driver.set_control(0u);

    /* SPIDATA is ignored while the bus is disabled. */
    if (driver.start(0x03u) != Status::Ok || spi.busy()) {
        return fail("SPIDATA write while disabled started a transfer");
    }

    /* Firmware without a host image fails closed. */
    (void)driver.byte(Firmware, 0x03u, true);
    if (driver.last != Status::Unsupported || spi.last_diagnostic() == nullptr) {
        return fail("firmware access without an image was accepted");
    }

    /* Invalid image requests. */
    const std::vector<uint8_t> image = make_image(0x400u);
    if (spi.set_firmware_image(nullptr, 4u) != Status::InvalidRegion ||
        spi.set_firmware_image(image.data(), 0u) != Status::InvalidRegion ||
        spi.set_firmware_image(image.data(),
                               NdsSpiControl::MaxFirmwareBytes + 1u) !=
            Status::InvalidRegion) {
        return fail("invalid firmware image was accepted");
    }
    if (spi.set_firmware_image(image.data(), image.size()) != Status::Ok ||
        !spi.firmware_loaded()) {
        return fail("valid firmware image was rejected");
    }

    /*
     * A host image replacement must not splice two flash images into one
     * chip-select transaction. The lock remains until CS is released, even
     * after the current byte transfer has completed.
     */
    (void)driver.set_control(driver.control(Firmware, true));
    if (driver.start(0x03u) != Status::Ok || !spi.busy() ||
        spi.set_firmware_image(image.data(), image.size()) !=
            Status::PermissionDenied) {
        return fail("firmware replacement was accepted during an active SPI transfer");
    }
    spi.advance_bus_cycles(64u);
    if (spi.busy() ||
        spi.set_firmware_image(image.data(), image.size()) !=
            Status::PermissionDenied) {
        return fail("firmware replacement was accepted while flash chip select remained held");
    }
    spi.reset_transfer();
    if (spi.set_firmware_image(image.data(), image.size()) != Status::Ok) {
        return fail("firmware image could not be replaced after chip select release");
    }

    /* Transfer timing: 8 bits at bus/8 per bit = 64 cycles. */
    (void)driver.set_control(driver.control(Firmware, true));
    (void)driver.start(0x03u);
    if (!spi.busy() || (spi.control() & NdsSpiControl::BusyBit) == 0u) {
        return fail("transfer did not set the busy bit");
    }
    spi.advance_bus_cycles(63u);
    if (!spi.busy()) return fail("transfer completed one cycle early");
    spi.advance_bus_cycles(1u);
    if (spi.busy() || (spi.control() & NdsSpiControl::BusyBit) != 0u) {
        return fail("transfer did not complete after 64 cycles");
    }
    spi.reset_transfer(); /* releases the held flash command */

    /* Slowest baud and 16-bit size scale the duration. */
    (void)driver.set_control(driver.control(Firmware, true, true, 3u));
    (void)driver.start(0x0300u);
    spi.advance_bus_cycles(16u * 64u - 1u);
    if (!spi.busy()) return fail("16-bit/512 kHz transfer completed early");
    spi.advance_bus_cycles(1u);
    if (spi.busy()) return fail("16-bit/512 kHz transfer did not complete");
    spi.reset_transfer();

    /* Writing SPIDATA while busy is refused. */
    (void)driver.set_control(driver.control(Firmware, true));
    (void)driver.start(0x03u);
    if (driver.start(0x00u) != Status::PermissionDenied) {
        return fail("SPIDATA write while busy was accepted");
    }
    spi.reset_transfer();

    /* Completion IRQ. */
    {
        const uint32_t before = interrupts.read(
            NDS_ARM7, NdsInterruptControl::IfAddress, 4u).value;
        (void)driver.set_control(driver.control(Firmware, false, false, 0u, true));
        (void)driver.start(0x05u);
        spi.advance_bus_cycles(64u);
        const uint32_t after = interrupts.read(
            NDS_ARM7, NdsInterruptControl::IfAddress, 4u).value;
        if ((before & NdsSpiControl::SpiIrqMask) != 0u ||
            (after & NdsSpiControl::SpiIrqMask) == 0u) {
            return fail("completion did not request the SPI IRQ");
        }
        (void)interrupts.write(NDS_ARM7, NdsInterruptControl::IfAddress, 4u,
                               NdsSpiControl::SpiIrqMask);
        (void)driver.set_control(driver.control(Firmware, false));
        (void)driver.start(0x05u);
        spi.advance_bus_cycles(64u);
        if ((interrupts.read(NDS_ARM7, NdsInterruptControl::IfAddress, 4u).value &
             NdsSpiControl::SpiIrqMask) != 0u) {
            return fail("IRQ fired with the IRQ-enable bit clear");
        }
    }

    /* Disabling the bus aborts the transfer without an IRQ. */
    (void)driver.set_control(driver.control(Firmware, false, false, 0u, true));
    (void)driver.start(0x05u);
    (void)driver.set_control(0u);
    spi.advance_bus_cycles(1024u);
    if (spi.busy() || (interrupts.read(NDS_ARM7, NdsInterruptControl::IfAddress, 4u).value &
                       NdsSpiControl::SpiIrqMask) != 0u) {
        return fail("disabling SPI did not abort the transfer");
    }

    /* Firmware READ streams bytes from the supplied image. */
    {
        std::vector<uint8_t> got;
        if (!flash_read(driver, 0x10u, got, 6u)) return fail("firmware read failed");
        for (size_t i = 0u; i < got.size(); ++i) {
            if (got[i] != image[0x10u + i]) return fail("firmware READ returned wrong bytes");
        }
        /* CS release restarted the command state: a new READ starts cleanly. */
        if (!flash_read(driver, 0x3FEu, got, 2u) || got[0] != image[0x3FEu] ||
            got[1] != image[0x3FFu]) {
            return fail("second firmware READ did not restart at the new address");
        }
        /* Reading past the end is refused rather than wrapped. */
        if (flash_read(driver, 0x3FFu, got, 2u) ||
            driver.last != Status::Unsupported) {
            return fail("firmware read past the image end was accepted");
        }
    }

    /* RDSR / WREN / WRDI. */
    {
        (void)driver.byte(Firmware, 0x05u, true);
        if (driver.byte(Firmware, 0u, false) != 0x00u) return fail("RDSR was not idle");
        (void)driver.byte(Firmware, 0x06u, false);
        (void)driver.byte(Firmware, 0x05u, true);
        if (driver.byte(Firmware, 0u, false) != 0x02u) return fail("WREN did not set WEL");
        (void)driver.byte(Firmware, 0x04u, false);
        (void)driver.byte(Firmware, 0x05u, true);
        if (driver.byte(Firmware, 0u, false) != 0x00u) return fail("WRDI did not clear WEL");
    }

    /* Programming/erase/JEDEC are not modeled and the state recovers. */
    for (uint8_t command : {uint8_t{0x0A}, uint8_t{0x02}, uint8_t{0xDB}, uint8_t{0x9F}}) {
        (void)driver.byte(Firmware, command, true);
        if (driver.last != Status::Unsupported) return fail("unmodeled flash command was accepted");
    }
    {
        std::vector<uint8_t> got;
        if (!flash_read(driver, 0u, got, 1u) || got[0] != image[0u]) {
            return fail("flash did not recover after a rejected command");
        }
    }

    /* Reserved device 3. */
    (void)driver.set_control(static_cast<uint16_t>(
        NdsSpiControl::EnableBit | (3u << NdsSpiControl::DeviceShift)));
    if (driver.start(0u) != Status::Unsupported) return fail("reserved SPI device 3 was accepted");

    /* Power management. */
    {
        (void)driver.byte(Power, 0x00u, true);       /* write register 0 */
        (void)driver.byte(Power, 0x0Cu, false);
        (void)driver.byte(Power, 0x80u, true);       /* read register 0 */
        if (driver.byte(Power, 0u, false) != 0x0Cu) return fail("power register 0 did not read back");
        if (spi.power_off_requested()) return fail("power-off latched without bit 6");
        (void)driver.byte(Power, 0x00u, true);
        (void)driver.byte(Power, 0x40u, false);
        if (!spi.power_off_requested()) return fail("power-off bit was not latched");

        spi.set_battery_low(true);
        (void)driver.byte(Power, 0x81u, true);
        if (driver.byte(Power, 0u, false) != 0x01u) return fail("battery status was not reported");
        (void)driver.byte(Power, 0x01u, true);       /* register 1 is read-only */
        (void)driver.byte(Power, 0xFFu, false);
        if (spi.power_register(1u) != 0x01u) return fail("read-only power register was overwritten");

        (void)driver.byte(Power, 0x05u, true);
        if (driver.last != Status::Unsupported) return fail("unmodeled power register was accepted");
    }

    /* Touchscreen: pen up. */
    if (touch_read(driver, 0xD0u) != 0x000u) return fail("pen-up X was not zero");
    if (touch_read(driver, 0x90u) != 0xFFFu) return fail("pen-up Y was not 0xFFF");

    /* Pen down, 12-bit and 8-bit. */
    spi.set_touch(true, 0x123u, 0x456u);
    if (touch_read(driver, 0xD0u) != 0x123u) return fail("touch X conversion is wrong");
    if (touch_read(driver, 0x90u) != 0x456u) return fail("touch Y conversion is wrong");
    if (touch_read(driver, 0xD8u) != 0x12u) return fail("8-bit touch X conversion is wrong");
    spi.set_touch(true, 0xFFFFu, 0xFFFFu);
    if (touch_read(driver, 0xD0u) != 0xFFFu) return fail("touch value was not limited to 12 bits");

    /* Auxiliary channels and their guard rails. */
    if (spi.set_adc_channel(6u, 0x0ABCu) != Status::Ok ||
        touch_read(driver, 0xE0u) != 0x0ABCu) {
        return fail("AUX ADC channel did not return the host value");
    }
    if (spi.set_adc_channel(1u, 0u) != Status::Unsupported ||
        spi.set_adc_channel(5u, 0u) != Status::Unsupported ||
        spi.set_adc_channel(8u, 0u) != Status::InvalidRegion) {
        return fail("ADC channel validation is wrong");
    }

    /* Idle clocks without a start bit do nothing. */
    if (driver.byte(Touch, 0x00u, false) != 0x00u) return fail("idle touchscreen clocks returned data");

    /* 16-bit transfers split the conversion over two words. */
    {
        spi.set_touch(true, 0x2A5u, 0x111u);
        const uint16_t first = driver.transfer(Touch, 0xD000u, true, true);
        const uint16_t second = driver.transfer(Touch, 0x0000u, false, true);
        const uint16_t value = static_cast<uint16_t>(
            ((first & 0xFFu) << 5u) | ((second >> 8u) >> 3u));
        if (value != 0x2A5u) return fail("16-bit touchscreen conversion is wrong");
    }

    /* A new control byte during a conversion is refused. */
    (void)driver.byte(Touch, 0xD0u, true);
    (void)driver.byte(Touch, 0x90u, true);
    if (driver.last != Status::Unsupported) return fail("overlapped touch conversion was accepted");

    /* Moving chip select to another device ends the touch sequence. */
    (void)driver.byte(Touch, 0xD0u, true);
    (void)driver.byte(Power, 0x80u, true);
    (void)driver.byte(Power, 0x00u, false);
    if (driver.byte(Touch, 0x00u, false) != 0x00u) return fail("touch conversion survived a device switch");

    /* reset_transfer aborts an active transfer. */
    (void)driver.set_control(driver.control(Touch, false));
    (void)driver.start(0xD0u);
    spi.reset_transfer();
    if (spi.busy()) return fail("reset_transfer left the bus busy");

    /* Access control and width checks. */
    if (spi.read(NDS_ARM9, NdsSpiControl::ControlAddress, 2u).status != Status::CpuAccessDenied ||
        spi.write(NDS_ARM9, NdsSpiControl::DataAddress, 2u, 0u) != Status::CpuAccessDenied) {
        return fail("ARM9 was allowed to reach the ARM7-only SPI registers");
    }
    if (spi.read(NDS_ARM7, NdsSpiControl::ControlAddress, 4u).status != Status::InvalidWidth ||
        spi.read(NDS_ARM7, NdsSpiControl::ControlAddress + 1u, 2u).status != Status::InvalidWidth ||
        spi.write(NDS_ARM7, NdsSpiControl::DataAddress + 1u, 1u, 0u) != Status::InvalidWidth) {
        return fail("invalid SPI access width or alignment was accepted");
    }
    if (spi.read(NDS_ARM7, NdsSpiControl::DataAddress + 4u, 2u).status != Status::Unmapped ||
        spi.read(static_cast<NdsCpu>(77), NdsSpiControl::ControlAddress, 2u).status != Status::InvalidCpu) {
        return fail("unmapped address or invalid CPU was accepted");
    }

    /* Clearing the selected flash cancels its transfer without a late IRQ. */
    const uint32_t irq_before_clear = interrupts.read(
        NDS_ARM7, NdsInterruptControl::IfAddress, 4u).value;
    (void)driver.set_control(driver.control(Firmware, true, false, 0u, true));
    if (driver.start(0x03u) != Status::Ok || !spi.busy()) {
        return fail("could not start flash transfer before clearing its image");
    }
    spi.clear_firmware_image();
    spi.advance_bus_cycles(64u);
    const uint32_t irq_after_clear = interrupts.read(
        NDS_ARM7, NdsInterruptControl::IfAddress, 4u).value;
    if (spi.firmware_loaded() || spi.busy() ||
        ((irq_after_clear ^ irq_before_clear) & NdsSpiControl::SpiIrqMask) != 0u) {
        return fail("clearing firmware did not abort the active transfer cleanly");
    }

    std::puts("nds_spi_control_test: ok");
    return 0;
}

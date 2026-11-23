/*
 * Integration of the ARM7 serial peripherals (RTC and SPI) with NdsMachine:
 * bus routing through the generated-code ABI, system-clock driven SPI timing,
 * the SPI IRQ and a clean bind/unbind cycle. Uses only synthetic data.
 */
#include "oracis/machine/nds_machine.hpp"

#include "oracis/memory/guest_memory_abi.h"
#include "oracis/memory/nds_rtc.hpp"
#include "oracis/memory/nds_spi_control.hpp"
#include "oracis/recomp/runtime_arm.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using oracis::machine::NdsMachine;
using oracis::machine::NdsMachineStatus;
using oracis::memory::NdsInterruptControl;
using oracis::memory::NdsRtc;
using oracis::memory::NdsRtcDateTime;
using oracis::memory::NdsSpiControl;
using oracis::memory::SharedWramMode;

int fail(const char *message)
{
    std::fprintf(stderr, "nds_machine_serial_test: %s\n", message);
    return 1;
}

void rtc_put(uint8_t value) { bus_write_u8(NdsRtc::RegisterAddress, value); }

void rtc_send(uint8_t byte)
{
    for (unsigned bit = 0u; bit < 8u; ++bit) {
        const uint8_t data = static_cast<uint8_t>((byte >> bit) & 1u);
        rtc_put(static_cast<uint8_t>(0x74u | data));
        rtc_put(static_cast<uint8_t>(0x76u | data));
    }
}

uint8_t rtc_receive()
{
    uint8_t byte = 0u;
    for (unsigned bit = 0u; bit < 8u; ++bit) {
        rtc_put(0x64u);
        rtc_put(0x66u);
        byte = static_cast<uint8_t>(
            byte | ((bus_read_u8(NdsRtc::RegisterAddress) & 1u) << bit));
    }
    return byte;
}

bool advance(NdsMachine &machine, uint64_t bus_cycles)
{
    return machine.report_idle_bus_cycles(NDS_ARM9, bus_cycles) ==
               NdsMachineStatus::Ok &&
           machine.report_idle_bus_cycles(NDS_ARM7, bus_cycles) ==
               NdsMachineStatus::Ok;
}

uint16_t spi_control(uint8_t device, bool hold, bool irq)
{
    return static_cast<uint16_t>(
        NdsSpiControl::EnableBit |
        (static_cast<uint16_t>(device) << NdsSpiControl::DeviceShift) |
        (hold ? NdsSpiControl::HoldBit : 0u) |
        (irq ? NdsSpiControl::IrqBit : 0u));
}

} // namespace

int main()
{
    NdsMachine machine(SharedWramMode::Arm9All32K);
    if (machine.initialize() != NdsMachineStatus::Ok) {
        return fail("machine initialization failed with the serial devices bound");
    }
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7)) {
        return fail("could not select the ARM7 for serial access");
    }

    /* RTC through the C ABI bus: read the date/time register. */
    if (machine.rtc().set_date_time(NdsRtcDateTime{26u, 11u, 23u, 1u, 9u, 30u, 15u}) !=
        oracis::memory::Status::Ok) {
        return fail("host date/time was rejected");
    }
    rtc_put(0x70u);
    rtc_put(0x74u);
    rtc_send(static_cast<uint8_t>(0x06u | (NdsRtc::RegDateTime << 4u) | 0x80u));
    uint8_t bytes[7]{};
    for (auto &byte : bytes) byte = rtc_receive();
    rtc_put(0x70u);
    const uint8_t expected[7] = {0x26, 0x11, 0x23, 0x01, 0x09, 0x30, 0x15};
    for (unsigned i = 0u; i < 7u; ++i) {
        if (bytes[i] != expected[i]) return fail("RTC date/time did not read through the bus");
    }

    /* SPI firmware read through the bus, timed by the system clock. */
    std::vector<uint8_t> image(0x100u);
    for (size_t i = 0u; i < image.size(); ++i) {
        image[i] = static_cast<uint8_t>(0xA0u + i);
    }
    if (machine.spi_control().set_firmware_image(image.data(), image.size()) !=
        oracis::memory::Status::Ok) {
        return fail("firmware image was rejected");
    }

    const auto spi_byte = [&](uint8_t value, bool hold, bool irq) -> int {
        bus_write_u16(NdsSpiControl::ControlAddress, spi_control(NdsSpiControl::DeviceFirmware, hold, irq));
        bus_write_u16(NdsSpiControl::DataAddress, value);
        if ((bus_read_u16(NdsSpiControl::ControlAddress) & NdsSpiControl::BusyBit) == 0u) {
            return -1; /* a transfer must be visibly busy right after the write */
        }
        if (!advance(machine, 63u)) return -2;
        if ((bus_read_u16(NdsSpiControl::ControlAddress) & NdsSpiControl::BusyBit) == 0u) {
            return -3; /* completed before 64 bus cycles */
        }
        if (!advance(machine, 1u)) return -2;
        if ((bus_read_u16(NdsSpiControl::ControlAddress) & NdsSpiControl::BusyBit) != 0u) {
            return -4; /* still busy after 64 bus cycles */
        }
        return static_cast<int>(bus_read_u16(NdsSpiControl::DataAddress) & 0xFFu);
    };

    if (spi_byte(0x03u, true, false) < 0 || spi_byte(0x00u, true, false) < 0 ||
        spi_byte(0x00u, true, false) < 0 || spi_byte(0x04u, true, false) < 0) {
        return fail("firmware READ command/address phase failed through the bus");
    }
    if (spi_byte(0x00u, true, false) != image[0x04u] ||
        spi_byte(0x00u, false, false) != image[0x05u]) {
        return fail("firmware bytes did not stream through the bus");
    }

    /* The completion IRQ reaches the ARM7 interrupt controller. */
    const uint32_t flags_before = machine.interrupt_control().read(
        NDS_ARM7, NdsInterruptControl::IfAddress, 4u).value;
    if ((flags_before & NdsSpiControl::SpiIrqMask) != 0u) {
        return fail("SPI IRQ flag was already set");
    }
    if (spi_byte(0x05u, true, true) < 0) return fail("RDSR command failed");
    const uint32_t flags_after = machine.interrupt_control().read(
        NDS_ARM7, NdsInterruptControl::IfAddress, 4u).value;
    if ((flags_after & NdsSpiControl::SpiIrqMask) == 0u) {
        return fail("SPI completion did not raise the ARM7 IRQ");
    }
    if (machine.interrupt_control().read(
            NDS_ARM9, NdsInterruptControl::IfAddress, 4u).value &
        NdsSpiControl::SpiIrqMask) {
        return fail("SPI IRQ leaked into the ARM9 interrupt controller");
    }
    (void)spi_byte(0x00u, false, false); /* release the flash select */

    if (machine.shutdown() != NdsMachineStatus::Ok) {
        return fail("machine did not shut down with the serial devices bound");
    }

    /* A second machine can bind the same process-global bus slots again. */
    NdsMachine second(SharedWramMode::Arm9All32K);
    if (second.initialize() != NdsMachineStatus::Ok ||
        second.shutdown() != NdsMachineStatus::Ok) {
        return fail("serial devices stayed bound after shutdown");
    }

    std::puts("nds_machine_serial_test: ok");
    return 0;
}

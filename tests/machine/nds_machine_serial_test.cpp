/*
 * Integration of the ARM7 serial peripherals (RTC and SPI) with NdsMachine:
 * bus routing through the generated-code ABI, system-clock driven SPI timing,
 * the SPI IRQ and a clean bind/unbind cycle. Uses only synthetic data.
 */
#include "oracis/machine/nds_machine.hpp"

#include "oracis/memory/guest_memory_abi.h"
#include "oracis/memory/nds_rtc.hpp"
#include "oracis/memory/nds_game_card_key1.hpp"
#include "oracis/memory/nds_spi_control.hpp"
#include "oracis/recomp/runtime_arm.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using oracis::machine::NdsMachine;
using oracis::machine::NdsMachineStatus;
using oracis::memory::NdsGameCardKey1;
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

void store_u16(std::vector<uint8_t> &bytes, size_t offset, uint16_t value)
{
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1u] = static_cast<uint8_t>(value >> 8u);
}

void store_u32(std::vector<uint8_t> &bytes, size_t offset, uint32_t value)
{
    for (uint32_t index = 0u; index < 4u; ++index) {
        bytes[offset + index] =
            static_cast<uint8_t>(value >> (index * 8u));
    }
}

uint16_t firmware_crc(
    const std::array<uint8_t, 4> &arm9,
    const std::array<uint8_t, 4> &arm7)
{
    uint16_t crc = UINT16_C(0xFFFF);
    for (const auto *part : {&arm9, &arm7}) {
        for (const uint8_t byte : *part) {
            crc = static_cast<uint16_t>(crc ^ byte);
            for (uint32_t bit = 0u; bit < 8u; ++bit) {
                crc = static_cast<uint16_t>(
                    (crc >> 1u) ^
                    ((crc & 1u) != 0u ? UINT16_C(0xA001) : UINT16_C(0)));
            }
        }
    }
    return crc;
}

std::vector<uint8_t> make_encrypted_firmware(
    const std::array<uint8_t, NdsGameCardKey1::KeyTableBytes> &key_table)
{
    constexpr uint32_t FirmwareId = UINT32_C(0x5043414D); /* MACP */
    constexpr std::array<uint8_t, 4> arm9 = {
        UINT8_C(0xA1), UINT8_C(0xA2), UINT8_C(0xA3), UINT8_C(0xA4)
    };
    constexpr std::array<uint8_t, 4> arm7 = {
        UINT8_C(0xB1), UINT8_C(0xB2), UINT8_C(0xB3), UINT8_C(0xB4)
    };
    std::vector<uint8_t> firmware(0x290u, 0u);
    store_u16(firmware, 0x0Cu, 4u);
    store_u16(firmware, 0x0Eu, UINT16_C(0x8024));
    store_u16(firmware, 0x10u, UINT16_C(0xA0));
    store_u16(firmware, 0x12u, UINT16_C(0x90));
    store_u16(firmware, 0x14u, UINT16_C(0x042D));
    store_u32(firmware, 0x08u, FirmwareId);

    NdsGameCardKey1 cipher;
    if (cipher.load_key_table(
            key_table.data(), NdsGameCardKey1::KeyTableBytes) !=
            oracis::memory::Status::Ok ||
        cipher.initialize(FirmwareId, 1u, 12u) !=
            oracis::memory::Status::Ok) {
        return {};
    }

    uint8_t rom_info[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    if (cipher.encrypt_block(rom_info, 8u) != oracis::memory::Status::Ok) {
        return {};
    }
    for (uint32_t index = 0u; index < 8u; ++index) {
        firmware[0x18u + index] = rom_info[index];
    }

    if (cipher.initialize(FirmwareId, 2u, 12u) !=
        oracis::memory::Status::Ok) {
        return {};
    }

    const std::array<uint8_t, 16> arm9_compressed = {
        0x10u, 4u, 0u, 0u, 0u, arm9[0], arm9[1], arm9[2],
        arm9[3], 0u, 0u, 0u, 0u, 0u, 0u, 0u
    };
    const std::array<uint8_t, 16> arm7_compressed = {
        0x10u, 4u, 0u, 0u, 0u, arm7[0], arm7[1], arm7[2],
        arm7[3], 0u, 0u, 0u, 0u, 0u, 0u, 0u
    };

    const auto encrypt_stream = [&cipher,&firmware](
        const std::array<uint8_t,16> &source, size_t destination) {
        for (size_t offset = 0u; offset < source.size(); offset += 8u) {
            uint8_t block[8]{};
            for (size_t index = 0u; index < 8u; ++index) {
                block[index] = source[offset + index];
            }
            if (cipher.encrypt_block(block, 8u) !=
                oracis::memory::Status::Ok) return false;
            for (size_t index = 0u; index < 8u; ++index) {
                firmware[destination + offset + index] = block[index];
            }
        }
        return true;
    };
    if (!encrypt_stream(arm9_compressed, 0x200u) ||
        !encrypt_stream(arm7_compressed, 0x280u)) {
        return {};
    }
    store_u16(firmware, 0x06u, firmware_crc(arm9, arm7));
    return firmware;
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

    /*
     * The BIOS-derived KEY1 table and user SPI flash feed the firmware IPL
     * decoder; validated LZ77 parts must be installed in ARM9/ARM7 guest RAM.
     */
    std::array<uint8_t, NdsGameCardKey1::KeyTableBytes> key_table{};
    for (uint32_t index = 0u; index < key_table.size(); ++index) {
        key_table[index] = static_cast<uint8_t>((index * 37u + 11u) & 0xFFu);
    }
    const auto synthetic_firmware = make_encrypted_firmware(key_table);
    if (synthetic_firmware.size() != 0x290u ||
        machine.game_card_control().load_key1_key_table(
            key_table.data(), NdsGameCardKey1::KeyTableBytes) !=
            oracis::memory::Status::Ok ||
        machine.spi_control().set_firmware_image(
            synthetic_firmware.data(), synthetic_firmware.size()) !=
            oracis::memory::Status::Ok ||
        machine.load_firmware_boot_code() != NdsMachineStatus::Ok ||
        !machine.firmware_boot_code_loaded()) {
        return fail("machine failed to decrypt and install synthetic firmware IPL parts");
    }

    const auto loaded_arm9 = machine.guest_memory().read(
        NDS_ARM9, UINT32_C(0x023FEE00), 4u);
    const auto loaded_arm7 = machine.guest_memory().read(
        NDS_ARM7, UINT32_C(0x0380F700), 4u);
    if (loaded_arm9.status != oracis::memory::Status::Ok ||
        loaded_arm9.value != UINT32_C(0xA4A3A2A1) ||
        loaded_arm7.status != oracis::memory::Status::Ok ||
        loaded_arm7.value != UINT32_C(0xB4B3B2B1)) {
        return fail("firmware IPL parts were not copied to the correct guest CPUs");
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

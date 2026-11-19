#include "oracis/memory/guest_memory_abi.h"

#include "oracis/memory/guest_memory.hpp"
#include "oracis/memory/nds_wram_control.hpp"
#include "oracis/memory/nds_interrupt_control.hpp"
#include "oracis/memory/nds_timer_control.hpp"
#include "oracis/memory/nds_keypad_control.hpp"
#include "oracis/recomp/runtime_arm.h"
#include "oracis/recomp/static_dispatch_abi.h"

#include <cstdio>
#include <cstdlib>

namespace {

oracis::memory::GuestMemory *g_guest_memory = nullptr;
oracis::memory::NdsWramControl *g_wram_control = nullptr;
oracis::memory::NdsInterruptControl *g_interrupt_control = nullptr;
oracis::memory::NdsTimerControl *g_timer_control = nullptr;
oracis::memory::NdsKeypadControl *g_keypad_control = nullptr;
uint16_t g_host_keypad_pressed_keys = 0u;

[[noreturn]] void bus_failure(const char *operation,
                              uint32_t address,
                              uint32_t width,
                              oracis::memory::Status status)
{
    std::fprintf(stderr,
        "[Oracis bus] %s failed: CPU=%u address=0x%08X width=%u "
        "status=%s PC=0x%08X\n",
        operation,
        static_cast<unsigned>(g_nds_active),
        address,
        width,
        oracis::memory::GuestMemory::status_name(status),
        oracis_active_cpu_state ? oracis_active_cpu_state->R[15] : 0u);
    std::abort();
}

oracis::memory::GuestMemory &bound_memory(const char *operation,
                                         uint32_t address,
                                         uint32_t width)
{
    if (g_guest_memory == nullptr) {
        bus_failure(operation, address, width,
                    oracis::memory::Status::Unmapped);
    }
    return *g_guest_memory;
}

uint32_t read_bus(uint32_t address, uint32_t width, const char *operation)
{
    auto &memory = bound_memory(operation, address, width);
    if (g_keypad_control != nullptr) {
        const auto mmio = g_keypad_control->read(
            g_nds_active, address, width);
        if (mmio.status != oracis::memory::Status::Unmapped) {
            if (!mmio) bus_failure(operation, address, width, mmio.status);
            return mmio.value;
        }
    }
    if (g_timer_control != nullptr) {
        const auto mmio = g_timer_control->read(
            g_nds_active, address, width);
        if (mmio.status != oracis::memory::Status::Unmapped) {
            if (!mmio) bus_failure(operation, address, width, mmio.status);
            return mmio.value;
        }
    }
    if (g_interrupt_control != nullptr) {
        const auto mmio = g_interrupt_control->read(
            g_nds_active, address, width);
        if (mmio.status != oracis::memory::Status::Unmapped) {
            if (!mmio) bus_failure(operation, address, width, mmio.status);
            return mmio.value;
        }
    }
    if (g_wram_control != nullptr) {
        const auto mmio = g_wram_control->read(
            g_nds_active, address, width);
        if (mmio.status != oracis::memory::Status::Unmapped) {
            if (!mmio) bus_failure(operation, address, width, mmio.status);
            return mmio.value;
        }
    }
    const auto result = memory.read(g_nds_active, address, width);
    if (!result) bus_failure(operation, address, width, result.status);
    return result.value;
}

void write_bus(uint32_t address,
               uint32_t width,
               uint32_t value,
               const char *operation)
{
    auto &memory = bound_memory(operation, address, width);
    if (g_keypad_control != nullptr) {
        const auto mmio = g_keypad_control->write(
            g_nds_active, address, width, value);
        if (mmio != oracis::memory::Status::Unmapped) {
            if (mmio != oracis::memory::Status::Ok) {
                bus_failure(operation, address, width, mmio);
            }
            return;
        }
    }
    if (g_timer_control != nullptr) {
        const auto mmio = g_timer_control->write(
            g_nds_active, address, width, value);
        if (mmio != oracis::memory::Status::Unmapped) {
            if (mmio != oracis::memory::Status::Ok) {
                bus_failure(operation, address, width, mmio);
            }
            return;
        }
    }
    if (g_interrupt_control != nullptr) {
        const auto mmio = g_interrupt_control->write(
            g_nds_active, address, width, value);
        if (mmio != oracis::memory::Status::Unmapped) {
            if (mmio != oracis::memory::Status::Ok) {
                bus_failure(operation, address, width, mmio);
            }
            return;
        }
    }
    if (g_wram_control != nullptr) {
        const auto mmio = g_wram_control->write(
            g_nds_active, address, width, value);
        if (mmio != oracis::memory::Status::Unmapped) {
            if (mmio != oracis::memory::Status::Ok) {
                bus_failure(operation, address, width, mmio);
            }
            return;
        }
    }
    const auto status = memory.write(g_nds_active, address, width, value);
    if (status != oracis::memory::Status::Ok) {
        bus_failure(operation, address, width, status);
    }
}

} // namespace

extern "C" int oracis_bus_bind_memory(void *memory)
{
    if (memory == nullptr) return 0;
    auto *guest_memory = static_cast<oracis::memory::GuestMemory *>(memory);
    if (g_guest_memory != nullptr && g_guest_memory != guest_memory) return 0;

    g_guest_memory = guest_memory;
    oracis_static_dispatch_set_validation_callback(
        oracis_bus_validate_dispatch, guest_memory);
    return 1;
}

extern "C" int oracis_bus_unbind_memory(void *memory)
{
    if (memory == nullptr || g_guest_memory !=
            static_cast<oracis::memory::GuestMemory *>(memory) ||
        (g_wram_control != nullptr &&
         &g_wram_control->backing_memory() == g_guest_memory) ||
        (g_interrupt_control != nullptr &&
         &g_interrupt_control->backing_memory() == g_guest_memory) ||
        (g_timer_control != nullptr &&
         &g_timer_control->backing_memory() == g_guest_memory) ||
        (g_keypad_control != nullptr &&
         &g_keypad_control->backing_memory() == g_guest_memory)) {
        return 0;
    }
    g_guest_memory = nullptr;
    oracis_static_dispatch_set_validation_callback(nullptr, nullptr);
    return 1;
}

extern "C" int oracis_bus_bind_wram_control(void *control)
{
    if (control == nullptr || g_guest_memory == nullptr) return 0;
    auto *device = static_cast<oracis::memory::NdsWramControl *>(control);
    if (!device->installed() || &device->backing_memory() != g_guest_memory) {
        return 0;
    }
    if (g_wram_control != nullptr) return 0;
    g_wram_control = device;
    return 1;
}

extern "C" int oracis_bus_unbind_wram_control(void *control)
{
    if (control == nullptr || g_wram_control !=
            static_cast<oracis::memory::NdsWramControl *>(control)) {
        return 0;
    }
    g_wram_control = nullptr;
    return 1;
}

extern "C" int oracis_bus_bind_interrupt_control(void *control)
{
    if (control == nullptr || g_guest_memory == nullptr) return 0;
    auto *device =
        static_cast<oracis::memory::NdsInterruptControl *>(control);
    if (&device->backing_memory() != g_guest_memory) return 0;
    if (g_interrupt_control != nullptr) return 0;
    g_interrupt_control = device;
    return 1;
}

extern "C" int oracis_bus_unbind_interrupt_control(void *control)
{
    if (control == nullptr || g_interrupt_control !=
            static_cast<oracis::memory::NdsInterruptControl *>(control) ||
        (g_timer_control != nullptr &&
         &g_timer_control->interrupt_control() == g_interrupt_control) ||
        (g_keypad_control != nullptr &&
         &g_keypad_control->interrupt_control() == g_interrupt_control)) {
        return 0;
    }
    g_interrupt_control = nullptr;
    return 1;
}

extern "C" int oracis_bus_bind_timer_control(void *control)
{
    if (control == nullptr || g_guest_memory == nullptr ||
        g_interrupt_control == nullptr) {
        return 0;
    }
    auto *device = static_cast<oracis::memory::NdsTimerControl *>(control);
    if (&device->backing_memory() != g_guest_memory ||
        &device->interrupt_control() != g_interrupt_control ||
        g_timer_control != nullptr) {
        return 0;
    }
    g_timer_control = device;
    return 1;
}

extern "C" int oracis_bus_unbind_timer_control(void *control)
{
    if (control == nullptr || g_timer_control !=
            static_cast<oracis::memory::NdsTimerControl *>(control)) {
        return 0;
    }
    g_timer_control = nullptr;
    return 1;
}


extern "C" int oracis_bus_bind_keypad_control(void *control)
{
    if (control == nullptr || g_guest_memory == nullptr ||
        g_interrupt_control == nullptr) {
        return 0;
    }
    auto *device =
        static_cast<oracis::memory::NdsKeypadControl *>(control);
    if (&device->backing_memory() != g_guest_memory ||
        &device->interrupt_control() != g_interrupt_control ||
        g_keypad_control != nullptr) {
        return 0;
    }
    g_keypad_control = device;
    g_keypad_control->set_pressed_keys(g_host_keypad_pressed_keys);
    return 1;
}

extern "C" int oracis_bus_unbind_keypad_control(void *control)
{
    if (control == nullptr || g_keypad_control !=
            static_cast<oracis::memory::NdsKeypadControl *>(control)) {
        return 0;
    }
    g_keypad_control = nullptr;
    return 1;
}

extern "C" void oracis_host_set_keypad_state(uint16_t pressed_keys)
{
    g_host_keypad_pressed_keys = static_cast<uint16_t>(
        pressed_keys & oracis::memory::NdsKeypadControl::KeyMask);
    if (g_keypad_control != nullptr) {
        g_keypad_control->set_pressed_keys(g_host_keypad_pressed_keys);
    }
}

extern "C" int oracis_bus_validate_dispatch(
    const NdsStaticValidation *validation,
    uint32_t pc,
    uint32_t thumb,
    void *user)
{
    auto *memory = static_cast<oracis::memory::GuestMemory *>(user);
    if (memory == nullptr || validation == nullptr ||
        validation->expected == nullptr || thumb > 1u ||
        (validation->dependency_count != 0u &&
         validation->dependencies == nullptr)) {
        return 0;
    }

    const uint32_t instruction_size = thumb != 0u ? 2u : 4u;
    const uint64_t validation_end =
        static_cast<uint64_t>(validation->addr) + validation->size;
    if (validation->size == 0u ||
        validation_end > UINT64_C(0x1'0000'0000) ||
        pc < validation->addr ||
        static_cast<uint64_t>(pc) + instruction_size > validation_end) {
        return 0;
    }

    const NdsCpu cpu = g_nds_active;
    if (memory->validate_execute(
            cpu, validation->addr, validation->size) !=
        oracis::memory::Status::Ok) {
        return 0;
    }
    if (memory->validate_identity(
            cpu, validation->addr, validation->size,
            validation->expected) != oracis::memory::Status::Ok) {
        return 0;
    }

    for (uint32_t index = 0u;
         index < validation->dependency_count;
         ++index) {
        const NdsStaticValidationRange &dependency =
            validation->dependencies[index];
        if (memory->validate_identity(
                cpu, dependency.addr, dependency.size,
                dependency.expected) != oracis::memory::Status::Ok) {
            return 0;
        }
    }
    return 1;
}

extern "C" uint8_t bus_read_u8(uint32_t address)
{
    return static_cast<uint8_t>(read_bus(address, 1u, "bus_read_u8"));
}

extern "C" uint16_t bus_read_u16(uint32_t address)
{
    return static_cast<uint16_t>(read_bus(address, 2u, "bus_read_u16"));
}

extern "C" uint32_t bus_read_u32(uint32_t address)
{
    return read_bus(address, 4u, "bus_read_u32");
}

extern "C" void bus_write_u8(uint32_t address, uint8_t value)
{
    write_bus(address, 1u, value, "bus_write_u8");
}

extern "C" void bus_write_u16(uint32_t address, uint16_t value)
{
    write_bus(address, 2u, value, "bus_write_u16");
}

extern "C" void bus_write_u32(uint32_t address, uint32_t value)
{
    write_bus(address, 4u, value, "bus_write_u32");
}

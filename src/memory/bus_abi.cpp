#include "oracis/memory/guest_memory_abi.h"

#include "oracis/memory/guest_memory.hpp"
#include "oracis/recomp/runtime_arm.h"

#include <cstdio>
#include <cstdlib>

namespace {

oracis::memory::GuestMemory *g_guest_memory = nullptr;

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
    const auto status = memory.write(g_nds_active, address, width, value);
    if (status != oracis::memory::Status::Ok) {
        bus_failure(operation, address, width, status);
    }
}

} // namespace

extern "C" int oracis_bus_bind_memory(void *memory)
{
    if (memory == nullptr) return 0;
    g_guest_memory = static_cast<oracis::memory::GuestMemory *>(memory);
    return 1;
}

extern "C" int oracis_bus_unbind_memory(void *memory)
{
    if (memory == nullptr || g_guest_memory !=
            static_cast<oracis::memory::GuestMemory *>(memory)) {
        return 0;
    }
    g_guest_memory = nullptr;
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

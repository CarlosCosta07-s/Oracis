#include "oracis/memory/nds_retail_map.hpp"

#include <cstdint>
#include <cstdio>

using oracis::memory::GuestMemory;
using oracis::memory::NdsRetailRamMap;
using oracis::memory::SharedWramMode;
using oracis::memory::Status;

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_retail_map_test: %s\n", message);
    return 1;
}

bool expect_cpu_denied(const GuestMemory &memory,
                       NdsCpu cpu,
                       uint32_t address)
{
    return memory.read(cpu, address, 1u).status ==
           Status::CpuAccessDenied;
}

int test_main_ram_and_arm7_private_wram()
{
    GuestMemory memory;
    if (NdsRetailRamMap::install(memory, SharedWramMode::Arm9All32K) !=
        Status::Ok) {
        return fail("retail NDS RAM map install failed");
    }

    if (memory.write(NDS_ARM9, 0x02001234u, 4u,
                     UINT32_C(0x12345678)) != Status::Ok) {
        return fail("ARM9 main RAM write failed");
    }
    if (memory.read(NDS_ARM7, 0x02001234u, 4u).value !=
        UINT32_C(0x12345678)) {
        return fail("main RAM was not shared with ARM7");
    }

    const uint32_t mirrors[] = {
        NdsRetailRamMap::MainRamMirror1,
        NdsRetailRamMap::MainRamMirror2,
        NdsRetailRamMap::MainRamMirror3
    };
    for (uint32_t mirror : mirrors) {
        if (memory.read(NDS_ARM9, mirror + 0x1234u, 4u).value !=
                UINT32_C(0x12345678) ||
            memory.read(NDS_ARM7, mirror + 0x1234u, 4u).value !=
                UINT32_C(0x12345678)) {
            return fail("main RAM mirror did not resolve to shared backing");
        }
    }

    if (memory.write(NDS_ARM7, NdsRetailRamMap::Arm7WramBase,
                     4u, UINT32_C(0xA7A7A7A7)) != Status::Ok) {
        return fail("ARM7 private WRAM write failed");
    }
    if (memory.read(NDS_ARM7, NdsRetailRamMap::Arm7WramBase, 4u).value !=
        UINT32_C(0xA7A7A7A7)) {
        return fail("ARM7 private WRAM readback failed");
    }
    if (!expect_cpu_denied(memory, NDS_ARM9,
                           NdsRetailRamMap::Arm7WramBase)) {
        return fail("ARM9 was allowed to access ARM7-private WRAM");
    }

    /* With WRAMCNT=0, ARM7's Shared-WRAM window aliases private WRAM. */
    if (memory.read(NDS_ARM7, NdsRetailRamMap::SharedWramBase, 4u).value !=
            UINT32_C(0xA7A7A7A7) ||
        memory.read(NDS_ARM7, NdsRetailRamMap::SharedWramBase + 0x10000u, 4u).value !=
            UINT32_C(0xA7A7A7A7)) {
        return fail("deallocated ARM7 Shared-WRAM window did not mirror private WRAM");
    }

    return 0;
}

int test_shared_wram_modes()
{
    /* WRAMCNT mode 0: ARM9 gets both 16KB banks, ARM7 gets none. */
    {
        GuestMemory memory;
        if (NdsRetailRamMap::install(memory, SharedWramMode::Arm9All32K) !=
            Status::Ok) return fail("mode 0 map failed");
        if (memory.write(NDS_ARM9, 0x03000000u, 4u,
                         UINT32_C(0x11112222)) != Status::Ok ||
            memory.write(NDS_ARM9, 0x03004000u, 4u,
                         UINT32_C(0x33334444)) != Status::Ok) {
            return fail("mode 0 writes failed");
        }
        if (memory.read(NDS_ARM9, 0x03000000u, 4u).value !=
                UINT32_C(0x11112222) ||
            memory.read(NDS_ARM9, 0x03004000u, 4u).value !=
                UINT32_C(0x33334444)) {
            return fail("mode 0 did not expose both banks to ARM9");
        }
        if (memory.write(NDS_ARM7, NdsRetailRamMap::Arm7WramBase,
                         4u, UINT32_C(0xA7A7A7A7)) != Status::Ok ||
            memory.read(NDS_ARM7, 0x03000000u, 4u).value !=
                UINT32_C(0xA7A7A7A7)) {
            return fail("mode 0 ARM7 window did not map private WRAM");
        }
        if (memory.read(NDS_ARM9, 0x03008000u, 4u).value !=
            UINT32_C(0x11112222)) {
            return fail("mode 0 Shared WRAM window did not mirror every 32KB");
        }
    }

    /* WRAMCNT mode 1: ARM9 gets physical bank 1, ARM7 gets bank 0. */
    {
        GuestMemory memory;
        if (NdsRetailRamMap::install(
                memory, SharedWramMode::Arm9Bank1Arm7Bank0) != Status::Ok) {
            return fail("mode 1 map failed");
        }
        if (memory.write(NDS_ARM9, 0x03000000u, 4u,
                         UINT32_C(0xBBBB0001)) != Status::Ok ||
            memory.write(NDS_ARM7, 0x03000000u, 4u,
                         UINT32_C(0xAAAA0000)) != Status::Ok) {
            return fail("mode 1 CPU-overlapped writes failed");
        }
        if (memory.read(NDS_ARM9, 0x03000000u, 4u).value !=
                UINT32_C(0xBBBB0001) ||
            memory.read(NDS_ARM7, 0x03000000u, 4u).value !=
                UINT32_C(0xAAAA0000)) {
            return fail("mode 1 did not select separate physical banks per CPU");
        }
        if (memory.read(NDS_ARM9, 0x03004000u, 4u).value !=
                UINT32_C(0xBBBB0001) ||
            memory.read(NDS_ARM7, 0x03004000u, 4u).value !=
                UINT32_C(0xAAAA0000)) {
            return fail("mode 1 did not mirror each 16KB CPU window");
        }
    }

    /* WRAMCNT mode 2 reverses the physical bank assignment. */
    {
        GuestMemory memory;
        if (NdsRetailRamMap::install(
                memory, SharedWramMode::Arm9Bank0Arm7Bank1) != Status::Ok) {
            return fail("mode 2 map failed");
        }
        if (memory.write(NDS_ARM9, 0x03000000u, 4u,
                         UINT32_C(0xAAAA0000)) != Status::Ok ||
            memory.write(NDS_ARM7, 0x03000000u, 4u,
                         UINT32_C(0xBBBB0001)) != Status::Ok) {
            return fail("mode 2 writes failed");
        }
        if (memory.read(NDS_ARM9, 0x03004000u, 4u).value !=
                UINT32_C(0xAAAA0000) ||
            memory.read(NDS_ARM7, 0x03004000u, 4u).value !=
                UINT32_C(0xBBBB0001)) {
            return fail("mode 2 did not mirror its assigned bank");
        }
    }

    /* WRAMCNT mode 3: ARM9 gets none, ARM7 gets both banks contiguously. */
    {
        GuestMemory memory;
        if (NdsRetailRamMap::install(memory, SharedWramMode::Arm7All32K) !=
            Status::Ok) return fail("mode 3 map failed");
        if (memory.write(NDS_ARM7, 0x03000000u, 4u,
                         UINT32_C(0x11112222)) != Status::Ok ||
            memory.write(NDS_ARM7, 0x03004000u, 4u,
                         UINT32_C(0x33334444)) != Status::Ok) {
            return fail("mode 3 writes failed");
        }
        if (memory.read(NDS_ARM7, 0x03000000u, 4u).value !=
                UINT32_C(0x11112222) ||
            memory.read(NDS_ARM7, 0x03004000u, 4u).value !=
                UINT32_C(0x33334444) ||
            memory.read(NDS_ARM7, 0x03008000u, 4u).value !=
                UINT32_C(0x11112222)) {
            return fail("mode 3 did not expose and mirror both ARM7 banks");
        }
        if (!expect_cpu_denied(memory, NDS_ARM9, 0x03000000u))
            return fail("mode 3 unexpectedly exposed Shared WRAM to ARM9");
    }

    return 0;
}

} // namespace

int main()
{
    if (test_main_ram_and_arm7_private_wram() != 0) return 1;
    if (test_shared_wram_modes() != 0) return 1;

    GuestMemory invalid;
    if (NdsRetailRamMap::install(
            invalid, static_cast<SharedWramMode>(7u)) != Status::InvalidRegion) {
        return fail("invalid WRAMCNT mode was accepted");
    }
    if (NdsRetailRamMap::install(
            invalid, SharedWramMode::Arm9All32K) != Status::Ok) {
        return fail("valid map could not be installed on empty memory");
    }
    if (NdsRetailRamMap::install(
            invalid, SharedWramMode::Arm9All32K) != Status::InvalidRegion) {
        return fail("retail map could be installed twice");
    }

    std::puts("retail NDS main RAM, mirrors, ARM7 WRAM and Shared WRAM modes passed");
    return 0;
}

#include "oracis/memory/guest_memory.hpp"

#include <cstdint>
#include <cstdio>

using oracis::memory::GuestMemory;
using oracis::memory::Permission;
using oracis::memory::Status;

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "guest_memory_test: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    GuestMemory memory;
    const uint8_t rwx = static_cast<uint8_t>(
        oracis::memory::Readable |
        oracis::memory::Writable |
        oracis::memory::Executable);

    if (memory.map_ram(0x02000000u, 0x1000u, rwx, "main RAM") != Status::Ok)
        return fail("main RAM mapping failed");
    if (memory.region_count() != 1u)
        return fail("region count did not increase");

    /*
     * CPU-disjoint regions may occupy the same guest address, as happens
     * when ARM9 and ARM7 select different Shared WRAM banks.
     */
    GuestMemory cpu_scoped;
    const uint8_t rw = static_cast<uint8_t>(
        oracis::memory::Readable | oracis::memory::Writable);
    if (cpu_scoped.map_ram(
            0x10000000u, 0x100u, rw, "ARM9-only overlap",
            oracis::memory::CpuAccessArm9) != Status::Ok ||
        cpu_scoped.map_ram(
            0x10000000u, 0x100u, rw, "ARM7-only overlap",
            oracis::memory::CpuAccessArm7) != Status::Ok) {
        return fail("CPU-disjoint overlapping regions were rejected");
    }
    if (cpu_scoped.write(NDS_ARM9, 0x10000000u, 4u,
                         UINT32_C(0x99999999)) != Status::Ok ||
        cpu_scoped.write(NDS_ARM7, 0x10000000u, 4u,
                         UINT32_C(0x77777777)) != Status::Ok) {
        return fail("CPU-disjoint guest writes failed");
    }
    if (cpu_scoped.read(NDS_ARM9, 0x10000000u, 4u).value !=
            UINT32_C(0x99999999) ||
        cpu_scoped.read(NDS_ARM7, 0x10000000u, 4u).value !=
            UINT32_C(0x77777777)) {
        return fail("overlapping CPU views resolved to the wrong backing");
    }
    if (cpu_scoped.read(NDS_ARM9, 0x10000000u, 1u).status ==
            Status::CpuAccessDenied ||
        cpu_scoped.read(NDS_ARM7, 0x10000000u, 1u).status ==
            Status::CpuAccessDenied) {
        return fail("valid CPU-specific mapping was treated as inaccessible");
    }

    const GuestMemory::CpuAccessUpdate conflicting_updates[] = {
        {"ARM9-only overlap", oracis::memory::CpuAccessArm9},
        {"ARM7-only overlap", oracis::memory::CpuAccessArm9}
    };
    if (cpu_scoped.update_cpu_access(
            conflicting_updates, 2u) != Status::Overlap ||
        cpu_scoped.read(NDS_ARM9, 0x10000000u, 4u).value !=
            UINT32_C(0x99999999) ||
        cpu_scoped.read(NDS_ARM7, 0x10000000u, 4u).value !=
            UINT32_C(0x77777777)) {
        return fail("conflicting visibility update was not atomic");
    }

    const GuestMemory::CpuAccessUpdate valid_updates[] = {
        {"ARM9-only overlap", oracis::memory::CpuAccessNone},
        {"ARM7-only overlap", oracis::memory::CpuAccessArm9}
    };
    if (cpu_scoped.update_cpu_access(valid_updates, 2u) != Status::Ok ||
        cpu_scoped.read(NDS_ARM9, 0x10000000u, 4u).value !=
            UINT32_C(0x77777777) ||
        cpu_scoped.read(NDS_ARM7, 0x10000000u, 1u).status !=
            Status::Unmapped) {
        return fail("valid atomic CPU visibility update failed");
    }
    if (cpu_scoped.map_ram(
            0x10000000u, 0x100u, rw, "invalid CPU mask", 0x80u) !=
        Status::InvalidRegion) {
        return fail("invalid CPU access mask was accepted");
    }

    if (memory.write(NDS_ARM9, 0x02000000u, 4u, 0x78563412u) != Status::Ok)
        return fail("32-bit write failed");
    const auto byte0 = memory.read(NDS_ARM9, 0x02000000u, 1u);
    const auto half0 = memory.read(NDS_ARM7, 0x02000000u, 2u);
    const auto word0 = memory.read(NDS_ARM9, 0x02000000u, 4u);
    if (!byte0 || byte0.value != 0x12u ||
        !half0 || half0.value != 0x3412u ||
        !word0 || word0.value != 0x78563412u) {
        return fail("little-endian reads did not preserve byte order");
    }

    if (memory.write(NDS_ARM7, 0x02000003u, 1u, 0xAAu) != Status::Ok)
        return fail("byte write through ARM7 failed");
    const auto shared = memory.read(NDS_ARM9, 0x02000000u, 4u);
    if (!shared || shared.value != 0xAA563412u)
        return fail("ARM9/ARM7 did not share the same RAM backing");

    if (memory.map_alias(0x02400000u, 0x02000000u, 0x1000u,
                         "main RAM mirror") != Status::Ok) {
        return fail("RAM alias mapping failed");
    }
    if (memory.read(NDS_ARM9, 0x02400000u, 4u).value != 0xAA563412u)
        return fail("alias did not read canonical backing");
    if (memory.write(NDS_ARM9, 0x02400001u, 1u, 0xBBu) != Status::Ok ||
        memory.read(NDS_ARM7, 0x02000001u, 1u).value != 0xBBu) {
        return fail("alias write was not visible at canonical address");
    }

    if (memory.validate_execute(NDS_ARM9, 0x02000000u, 4u) != Status::Ok)
        return fail("executable RAM validation failed");

    if (memory.map_ram(0x03000000u, 0x100u,
                       static_cast<uint8_t>(oracis::memory::Readable),
                       "read-only") != Status::Ok) {
        return fail("read-only region mapping failed");
    }
    if (memory.write(NDS_ARM9, 0x03000000u, 1u, 0xFFu) !=
        Status::PermissionDenied) {
        return fail("write permission was not enforced");
    }
    if (memory.validate_execute(NDS_ARM9, 0x03000000u, 2u) !=
        Status::PermissionDenied) {
        return fail("execute permission was not enforced");
    }

    const uint8_t read_only_zeros[] = {0u, 0u};
    if (memory.validate_identity(
            NDS_ARM9, 0x03000000u, sizeof(read_only_zeros),
            read_only_zeros) != Status::MissingWriteProvenance) {
        return fail("uninitialized read-only mapping passed identity validation");
    }

    const uint8_t image_bytes[] = {0xAAu, 0xBBu, 0xCCu, 0xDDu};
    if (memory.map_image(
            0x03100000u, image_bytes, sizeof(image_bytes),
            static_cast<uint8_t>(oracis::memory::Readable |
                                 oracis::memory::Executable),
            "trusted initial image") != Status::Ok) {
        return fail("initialized image mapping failed");
    }
    if (memory.validate_identity(
            NDS_ARM9, 0x03100000u, sizeof(image_bytes), image_bytes) !=
        Status::Ok) {
        return fail("initialized image bytes did not validate");
    }
    const uint8_t wrong_image[] = {0xAAu, 0xBBu, 0u, 0xDDu};
    if (memory.validate_identity(
            NDS_ARM9, 0x03100000u, sizeof(wrong_image), wrong_image) !=
        Status::IdentityMismatch) {
        return fail("initialized image accepted different bytes");
    }
    if (memory.map_image(0x03200000u, nullptr, 4u,
                         oracis::memory::Readable,
                         "null image") != Status::InvalidRegion) {
        return fail("null initial image was accepted");
    }

    if (memory.map_ram(0x02000800u, 0x100u, rwx, "overlap") != Status::Overlap)
        return fail("overlapping mapping was accepted");
    if (memory.map_ram(0x04000000u, 0u, rwx, "zero") != Status::InvalidRegion)
        return fail("zero-sized mapping was accepted");
    if (memory.map_ram(0xFFFFFFF0u, 0x40u, rwx, "overflow") !=
        Status::InvalidRegion) {
        return fail("guest address-space overflow was accepted");
    }

    if (memory.read(NDS_ARM9, 0x05000000u, 4u).status != Status::Unmapped)
        return fail("unmapped read did not fail");
    if (memory.read(NDS_ARM9, 0xFFFFFFFEu, 4u).status != Status::AddressOverflow ||
        memory.write(NDS_ARM7, 0xFFFFFFFFu, 2u, 0xFFFFu) != Status::AddressOverflow) {
        return fail("guest access address overflow was not rejected");
    }
    if (memory.read(NDS_ARM9, 0x02000FFFu, 2u).status != Status::CrossesRegion)
        return fail("cross-region read was not rejected");
    if (memory.read(NDS_ARM9, 0x02000000u, 3u).status != Status::InvalidWidth)
        return fail("unsupported access width was not rejected");
    if (memory.read(static_cast<NdsCpu>(9), 0x02000000u, 1u).status !=
        Status::InvalidCpu) {
        return fail("invalid CPU identifier was not rejected");
    }
    if (memory.map_alias(0x02500000u, 0x02100000u, 0x100u,
                         "bad alias") != Status::Unmapped) {
        return fail("alias of unmapped canonical range was accepted");
    }

    /* Zero-filled writable RAM must not impersonate a live code image. */
    const uint8_t zero_code[] = {0u, 0u, 0u, 0u};
    if (memory.validate_identity(
            NDS_ARM9, 0x02000100u, sizeof(zero_code), zero_code) !=
        Status::MissingWriteProvenance) {
        return fail("zero-filled writable RAM was accepted as validated code");
    }

    if (memory.write(NDS_ARM9, 0x02000100u, 4u, UINT32_C(0x04030201)) !=
        Status::Ok) {
        return fail("code image write for identity test failed");
    }
    const uint8_t written_code[] = {1u, 2u, 3u, 4u};
    const uint8_t stale_code[] = {1u, 2u, 0x99u, 4u};
    if (memory.validate_identity(
            NDS_ARM9, 0x02000100u, sizeof(written_code), written_code) !=
        Status::Ok) {
        return fail("written code bytes did not validate");
    }
    if (memory.validate_identity(
            NDS_ARM9, 0x02000100u, sizeof(stale_code), stale_code) !=
        Status::IdentityMismatch) {
        return fail("changed code bytes passed identity validation");
    }

    std::puts("guest memory mapping, aliases, endian and permission tests passed");
    return 0;
}

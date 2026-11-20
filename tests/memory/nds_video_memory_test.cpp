#include "oracis/memory/nds_video_memory.hpp"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_video_memory_test: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    using oracis::memory::GuestMemory;
    using oracis::memory::NdsVideoMemory;
    using oracis::memory::Status;

    GuestMemory memory;
    NdsVideoMemory video(memory);
    if (video.install() != Status::Ok || !video.installed() ||
        memory.region_count() != 2u) {
        return fail("palette and OAM regions did not install");
    }

    constexpr uint32_t palette_addresses[] = {
        UINT32_C(0x05000000), /* Engine A BG */
        UINT32_C(0x05000200), /* Engine A OBJ */
        UINT32_C(0x05000400), /* Engine B BG */
        UINT32_C(0x05000600)  /* Engine B OBJ */
    };
    constexpr uint32_t palette_markers[] = {
        UINT32_C(0xA0B0C001), UINT32_C(0xA0B0C002),
        UINT32_C(0xB0C0D003), UINT32_C(0xB0C0D004)
    };
    for (unsigned index = 0u; index < 4u; ++index) {
        if (memory.read(
                NDS_ARM9, palette_addresses[index], 4u).status != Status::Ok ||
            video.write(
                NDS_ARM9, palette_addresses[index], 4u,
                palette_markers[index]) != Status::Ok ||
            memory.read(
                NDS_ARM9, palette_addresses[index], 4u).value !=
                palette_markers[index]) {
            return fail("one of the four 512-byte palette banks failed");
        }
    }

    constexpr uint32_t oam_addresses[] = {
        UINT32_C(0x07000000), /* Engine A OAM */
        UINT32_C(0x07000400)  /* Engine B OAM */
    };
    constexpr uint32_t oam_markers[] = {
        UINT32_C(0x11223344), UINT32_C(0x55667788)
    };
    for (unsigned index = 0u; index < 2u; ++index) {
        if (memory.read(
                NDS_ARM9, oam_addresses[index], 4u).status != Status::Ok ||
            video.write(
                NDS_ARM9, oam_addresses[index], 4u,
                oam_markers[index]) != Status::Ok ||
            memory.read(
                NDS_ARM9, oam_addresses[index], 4u).value !=
                oam_markers[index]) {
            return fail("one of the two 1KB OAM banks failed");
        }
    }

    const uint32_t palette_before_byte_store =
        memory.read(NDS_ARM9, palette_addresses[0], 4u).value;
    const uint32_t oam_before_byte_store =
        memory.read(NDS_ARM9, oam_addresses[0], 4u).value;
    if (video.write(
            NDS_ARM9, palette_addresses[0], 1u, UINT32_C(0xFF)) !=
            Status::Ok ||
        video.write(
            NDS_ARM9, oam_addresses[0], 1u, UINT32_C(0xFF)) !=
            Status::Ok ||
        memory.read(NDS_ARM9, palette_addresses[0], 4u).value !=
            palette_before_byte_store ||
        memory.read(NDS_ARM9, oam_addresses[0], 4u).value !=
            oam_before_byte_store) {
        return fail("8-bit stores modified palette or OAM contents");
    }

    if (video.write(
            NDS_ARM9, palette_addresses[0] + NdsVideoMemory::PaletteSize - 2u,
            2u, UINT32_C(0xABCD)) != Status::Ok ||
        memory.read(
            NDS_ARM9, palette_addresses[0] + NdsVideoMemory::PaletteSize - 2u,
            2u).value != UINT32_C(0xABCD) ||
        video.write(
            NDS_ARM9, palette_addresses[0] + NdsVideoMemory::PaletteSize - 2u,
            4u, UINT32_C(0x12345678)) != Status::CrossesRegion) {
        return fail("palette boundary and cross-region validation failed");
    }

    if (memory.read(NDS_ARM7, NdsVideoMemory::PaletteBase, 2u).status !=
            Status::CpuAccessDenied ||
        memory.read(NDS_ARM7, NdsVideoMemory::OamBase, 2u).status !=
            Status::CpuAccessDenied ||
        video.write(
            NDS_ARM7, NdsVideoMemory::PaletteBase, 2u,
            UINT32_C(0x1234)) != Status::CpuAccessDenied ||
        video.write(
            NDS_ARM7, NdsVideoMemory::OamBase, 2u,
            UINT32_C(0x1234)) != Status::CpuAccessDenied) {
        return fail("ARM7 unexpectedly accessed ARM9 video memory");
    }

    if (video.write(
            NDS_ARM9, UINT32_C(0x05000800), 2u,
            UINT32_C(0x1234)) != Status::Unmapped ||
        video.write(
            NDS_ARM9, UINT32_C(0x07000800), 2u,
            UINT32_C(0x1234)) != Status::Unmapped ||
        video.write(
            NDS_ARM9, NdsVideoMemory::PaletteBase, 3u,
            UINT32_C(0x12345678)) != Status::InvalidWidth ||
        video.write(
            static_cast<NdsCpu>(9), NdsVideoMemory::PaletteBase, 2u,
            UINT32_C(0x1234)) != Status::InvalidCpu) {
        return fail("unmapped, invalid-width or invalid-CPU validation failed");
    }

    /* A failed second mapping must remove the first one as well. */
    GuestMemory conflicting_memory;
    if (conflicting_memory.map_ram(
            NdsVideoMemory::OamBase, NdsVideoMemory::OamSize,
            static_cast<uint8_t>(
                oracis::memory::Readable | oracis::memory::Writable),
            "pre-existing OAM conflict",
            oracis::memory::CpuAccessArm9) != Status::Ok) {
        return fail("could not prepare an OAM mapping conflict");
    }
    NdsVideoMemory conflicting_video(conflicting_memory);
    if (conflicting_video.install() != Status::Overlap ||
        conflicting_video.installed() ||
        conflicting_memory.region_count() != 1u ||
        conflicting_memory.read(
            NDS_ARM9, NdsVideoMemory::PaletteBase, 2u).status !=
            Status::Unmapped) {
        return fail("failed installation left a partial palette region behind");
    }

    std::puts("NDS fixed palette/OAM backing, CPU visibility and store widths passed");
    return 0;
}

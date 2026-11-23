#include "oracis/memory/guest_memory.hpp"

#include <cstdint>
#include <cstdio>
#include <utility>

using oracis::memory::GuestMemory;
using oracis::memory::Permission;
using oracis::memory::Status;

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "guest_memory_generation_test: %s\n", message);
    return 1;
}

bool advanced(GuestMemory &memory, uint64_t &previous)
{
    const uint64_t current = memory.mutation_generation();
    if (current == 0u || current == previous) return false;
    previous = current;
    return true;
}

bool unchanged(const GuestMemory &memory, uint64_t expected)
{
    return memory.mutation_generation() == expected;
}

} // namespace

int main()
{
    GuestMemory memory;
    const uint8_t rwx = static_cast<uint8_t>(
        oracis::memory::Readable |
        oracis::memory::Writable |
        oracis::memory::Executable);
    uint64_t generation = memory.mutation_generation();
    if (generation == 0u)
        return fail("new memory map exposed the exhausted epoch sentinel");

    if (memory.map_ram(
            0x02000000u, 0x1000u, rwx, "code backing") != Status::Ok ||
        !advanced(memory, generation)) {
        return fail("RAM mapping did not advance the mutation generation");
    }

    const uint64_t before_read = generation;
    if (memory.read(NDS_ARM9, 0x02000000u, 4u).status != Status::Ok ||
        memory.validate_execute(NDS_ARM9, 0x02000000u, 4u) != Status::Ok ||
        !unchanged(memory, before_read)) {
        return fail("read-only operations changed the mutation generation");
    }

    if (memory.map_ram(
            0x02000800u, 0x100u, rwx, "overlap") != Status::Overlap ||
        !unchanged(memory, generation)) {
        return fail("rejected map operation advanced the mutation generation");
    }

    if (memory.write(
            NDS_ARM9, 0x02000000u, 4u, UINT32_C(0x12345678)) != Status::Ok ||
        !advanced(memory, generation)) {
        return fail("scalar guest write did not advance the generation");
    }

    const uint8_t block[] = {0x11u, 0x22u, 0x33u, 0x44u};
    if (memory.write_bytes(
            NDS_ARM9, 0x02000010u, block,
            static_cast<uint32_t>(sizeof(block))) != Status::Ok ||
        !advanced(memory, generation)) {
        return fail("block guest write did not advance the generation");
    }

    if (memory.write(
            NDS_ARM9, 0x09000000u, 4u, 0u) == Status::Ok ||
        !unchanged(memory, generation)) {
        return fail("rejected guest write advanced the generation");
    }

    if (memory.map_alias(
            0x03000000u, 0x02000000u, 0x1000u, "code alias") != Status::Ok ||
        !advanced(memory, generation)) {
        return fail("alias mapping did not advance the generation");
    }
    if (memory.write(
            NDS_ARM7, 0x03000000u, 4u, UINT32_C(0xAABBCCDD)) != Status::Ok ||
        !advanced(memory, generation) ||
        memory.read(NDS_ARM9, 0x02000000u, 4u).value !=
            UINT32_C(0xAABBCCDD)) {
        return fail("write through alias did not invalidate shared backing");
    }

    if (memory.map_mirror(
            0x04000000u, 0x02000000u, 0x1000u, 0x2000u,
            "code mirror", oracis::memory::CpuAccessInherit) != Status::Ok ||
        !advanced(memory, generation)) {
        return fail("mirror mapping did not advance the generation");
    }
    if (memory.write(
            NDS_ARM9, 0x04001000u, 4u, UINT32_C(0x55667788)) != Status::Ok ||
        !advanced(memory, generation) ||
        memory.read(NDS_ARM7, 0x02000000u, 4u).value !=
            UINT32_C(0x55667788)) {
        return fail("write through periodic mirror did not advance generation");
    }

    if (memory.map_mirror_overlay(
            0x05000000u, 0x02000000u, 0x1000u, 0x1000u,
            "code overlay", oracis::memory::CpuAccessArm9, 1u) != Status::Ok ||
        !advanced(memory, generation)) {
        return fail("executable overlay mapping did not advance generation");
    }
    if (memory.write(
            NDS_ARM9, 0x05000000u, 4u, UINT32_C(0xCAFEBABE)) != Status::Ok ||
        !advanced(memory, generation)) {
        return fail("write through overlay did not advance generation");
    }

    if (memory.replace_mirror_overlay(
            "code overlay", 0x05002000u, 0x02000000u,
            0x1000u, 0x1000u, oracis::memory::CpuAccessArm9, 1u) !=
            Status::Ok ||
        !advanced(memory, generation)) {
        return fail("overlay replacement did not advance generation");
    }

    const GuestMemory::CpuAccessUpdate hide_alias[] = {
        {"code alias", oracis::memory::CpuAccessArm7}
    };
    if (memory.update_cpu_access(hide_alias, 1u) != Status::Ok ||
        !advanced(memory, generation)) {
        return fail("CPU visibility update did not advance generation");
    }
    if (memory.write(
            NDS_ARM9, 0x03000000u, 4u, 0u) == Status::Ok ||
        !unchanged(memory, generation)) {
        return fail("write through hidden alias changed memory or generation");
    }
    if (memory.write(
            NDS_ARM7, 0x03000000u, 4u, UINT32_C(0x01020304)) != Status::Ok ||
        !advanced(memory, generation)) {
        return fail("ARM7 write through visible alias did not advance generation");
    }

    if (!memory.unmap_region("code mirror") ||
        !advanced(memory, generation)) {
        return fail("unmapping a mirror did not advance generation");
    }
    if (memory.unmap_region("missing region") ||
        !unchanged(memory, generation)) {
        return fail("failed unmap advanced the mutation generation");
    }

    const uint8_t image[] = {0xAAu, 0xBBu, 0xCCu, 0xDDu};
    if (memory.map_image(
            0x06000000u, image, sizeof(image),
            static_cast<uint8_t>(Permission::Readable | Permission::Executable),
            "initial image") != Status::Ok ||
        !advanced(memory, generation)) {
        return fail("initial image mapping did not advance generation");
    }

    GuestMemory move_source;
    if (move_source.map_ram(
            0x07000000u, 0x100u, rwx, "move source") != Status::Ok) {
        return fail("move source setup failed");
    }
    const uint64_t source_epoch_before_move =
        move_source.mutation_generation();
    GuestMemory moved(std::move(move_source));
    if (move_source.mutation_generation() == source_epoch_before_move ||
        moved.mutation_generation() == 0u ||
        moved.read(NDS_ARM9, 0x07000000u, 1u).status != Status::Ok) {
        return fail("move construction did not preserve data and invalidate source");
    }

    GuestMemory move_target;
    if (move_target.map_ram(
            0x07100000u, 0x100u, rwx, "move target") != Status::Ok) {
        return fail("move target setup failed");
    }
    const uint64_t target_epoch_before_move =
        move_target.mutation_generation();
    const uint64_t moved_epoch_before_assignment =
        moved.mutation_generation();
    move_target = std::move(moved);
    if (move_target.mutation_generation() == target_epoch_before_move ||
        moved.mutation_generation() == moved_epoch_before_assignment ||
        move_target.read(NDS_ARM9, 0x07000000u, 1u).status != Status::Ok) {
        return fail("move assignment did not invalidate both memory identities");
    }

    std::puts("guest memory mutation generation tests passed");
    return 0;
}

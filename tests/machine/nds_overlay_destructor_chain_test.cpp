#include "oracis/machine/nds_overlay_destructor_chain.hpp"

#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

using oracis::machine::NdsOverlayDestructorChain;
using oracis::machine::NdsOverlayDestructorChainStatus;
using oracis::machine::NdsOverlayDestructorRecord;
using oracis::memory::GuestMemory;

constexpr uint32_t kRamBase = UINT32_C(0x02000000);
constexpr uint32_t kRamSize = UINT32_C(0x10000);
constexpr uint32_t kHeadAddress = kRamBase + UINT32_C(0x100);
constexpr uint32_t kNodeA = kRamBase + UINT32_C(0x200);
constexpr uint32_t kNodeB = kRamBase + UINT32_C(0x220);
constexpr uint32_t kNodeC = kRamBase + UINT32_C(0x240);
constexpr uint32_t kOverlayBase = kRamBase + UINT32_C(0x1000);
constexpr uint32_t kOverlayCodeSize = UINT32_C(0x80);
constexpr uint32_t kOverlayBssSize = UINT32_C(0x80);
constexpr uint32_t kOverlayEnd =
    kOverlayBase + kOverlayCodeSize + kOverlayBssSize;

void require(bool condition)
{
    if (!condition) std::abort();
}

GuestMemory make_memory()
{
    GuestMemory memory;
    require(memory.map_ram(
        kRamBase, kRamSize,
        oracis::memory::Readable |
            oracis::memory::Writable |
            oracis::memory::Executable,
        "overlay-destructor-chain-test",
        oracis::memory::CpuAccessArm9) == oracis::memory::Status::Ok);
    return memory;
}

void write_word(GuestMemory &memory, uint32_t address, uint32_t value)
{
    require(memory.write(NDS_ARM9, address, 4u, value) ==
        oracis::memory::Status::Ok);
}

uint32_t read_word(const GuestMemory &memory, uint32_t address)
{
    const auto result = memory.read(NDS_ARM9, address, 4u);
    require(static_cast<bool>(result));
    return result.value;
}

void write_node(
    GuestMemory &memory,
    uint32_t address,
    uint32_t next,
    uint32_t destructor,
    uint32_t object)
{
    write_word(memory, address, next);
    write_word(memory, address + 4u, destructor);
    write_word(memory, address + 8u, object);
}

void test_detaches_matching_nodes_and_preserves_other_overlay_order()
{
    GuestMemory memory = make_memory();
    NdsOverlayDestructorChain chain(memory);

    /* The first node matches by destructor address with a null object.
       The third matches by object address in the overlay BSS. */
    write_node(
        memory, kNodeA, kNodeB, kOverlayBase + 0x11u, 0u);
    write_node(
        memory, kNodeB, kNodeC, kRamBase + 0x5000u, kRamBase + 0x3000u);
    write_node(
        memory, kNodeC, 0u, kRamBase + 0x6000u, kOverlayBase + 0x90u);
    write_word(memory, kHeadAddress, kNodeA);

    std::vector<NdsOverlayDestructorRecord> detached;
    require(chain.detach_for_overlay(
        NDS_ARM9, kHeadAddress, kOverlayBase,
        kOverlayCodeSize, kOverlayBssSize, detached) ==
        NdsOverlayDestructorChainStatus::Ok);

    require(detached.size() == 2u);
    require(detached[0].chain_node_address == kNodeA);
    require(detached[0].destructor_pointer == kOverlayBase + 0x11u);
    require(detached[0].object_address == 0u);
    require(detached[1].chain_node_address == kNodeC);
    require(detached[1].destructor_pointer == kRamBase + 0x6000u);
    require(detached[1].object_address == kOverlayBase + 0x90u);

    /* Global list retains only B; detached nodes preserve A -> C order. */
    require(read_word(memory, kHeadAddress) == kNodeB);
    require(read_word(memory, kNodeB) == 0u);
    require(read_word(memory, kNodeA) == kNodeC);
    require(read_word(memory, kNodeC) == 0u);
}

void test_keeps_unrelated_nodes_and_leaves_memory_unchanged()
{
    GuestMemory memory = make_memory();
    NdsOverlayDestructorChain chain(memory);

    write_node(
        memory, kNodeA, 0u,
        kRamBase + 0x5000u, kRamBase + 0x6000u);
    write_word(memory, kHeadAddress, kNodeA);

    std::vector<NdsOverlayDestructorRecord> detached{
        {UINT32_C(0xDEADBEEF), UINT32_C(0xCAFEBABE), 0u}
    };
    require(chain.detach_for_overlay(
        NDS_ARM9, kHeadAddress, kOverlayBase,
        kOverlayCodeSize, kOverlayBssSize, detached) ==
        NdsOverlayDestructorChainStatus::Ok);

    require(detached.empty());
    require(read_word(memory, kHeadAddress) == kNodeA);
    require(read_word(memory, kNodeA) == 0u);
    require(read_word(memory, kNodeA + 4u) == kRamBase + 0x5000u);
    require(read_word(memory, kNodeA + 8u) == kRamBase + 0x6000u);
}

void test_detaches_object_with_null_destructor_without_invoking_it()
{
    GuestMemory memory = make_memory();
    NdsOverlayDestructorChain chain(memory);

    write_node(
        memory, kNodeA, 0u, 0u, kOverlayBase + 0x20u);
    write_word(memory, kHeadAddress, kNodeA);

    std::vector<NdsOverlayDestructorRecord> detached;
    require(chain.detach_for_overlay(
        NDS_ARM9, kHeadAddress, kOverlayBase,
        kOverlayCodeSize, kOverlayBssSize, detached) ==
        NdsOverlayDestructorChainStatus::Ok);
    require(detached.size() == 1u);
    require(detached[0].destructor_pointer == 0u);
    require(read_word(memory, kHeadAddress) == 0u);
    require(read_word(memory, kNodeA) == 0u);
}

void test_rejects_cycles_without_mutating_the_chain()
{
    GuestMemory memory = make_memory();
    NdsOverlayDestructorChain chain(memory);

    write_node(
        memory, kNodeA, kNodeB, kOverlayBase + 0x11u, 0u);
    write_node(
        memory, kNodeB, kNodeA, kRamBase + 0x5000u, kRamBase + 0x3000u);
    write_word(memory, kHeadAddress, kNodeA);

    std::vector<NdsOverlayDestructorRecord> detached{
        {UINT32_C(0xDEADBEEF), UINT32_C(0xCAFEBABE), 0u}
    };
    require(chain.detach_for_overlay(
        NDS_ARM9, kHeadAddress, kOverlayBase,
        kOverlayCodeSize, kOverlayBssSize, detached) ==
        NdsOverlayDestructorChainStatus::ChainCycle);

    require(detached.size() == 1u);
    require(read_word(memory, kHeadAddress) == kNodeA);
    require(read_word(memory, kNodeA) == kNodeB);
    require(read_word(memory, kNodeB) == kNodeA);
}

void test_rejects_unmapped_node_without_mutating_the_head()
{
    GuestMemory memory = make_memory();
    NdsOverlayDestructorChain chain(memory);
    constexpr uint32_t UnmappedNode = UINT32_C(0x03000000);
    write_word(memory, kHeadAddress, UnmappedNode);

    std::vector<NdsOverlayDestructorRecord> detached;
    require(chain.detach_for_overlay(
        NDS_ARM9, kHeadAddress, kOverlayBase,
        kOverlayCodeSize, kOverlayBssSize, detached) ==
        NdsOverlayDestructorChainStatus::MemoryUnavailable);
    require(read_word(memory, kHeadAddress) == UnmappedNode);
    require(detached.empty());
}

void test_rejects_invalid_cpu_and_overflowing_overlay_range()
{
    GuestMemory memory = make_memory();
    NdsOverlayDestructorChain chain(memory);
    std::vector<NdsOverlayDestructorRecord> detached;

    require(chain.detach_for_overlay(
        static_cast<NdsCpu>(99u), kHeadAddress, kOverlayBase,
        kOverlayCodeSize, kOverlayBssSize, detached) ==
        NdsOverlayDestructorChainStatus::InvalidCpu);

    require(chain.detach_for_overlay(
        NDS_ARM9, kHeadAddress, UINT32_C(0xFFFFFFF0),
        UINT32_C(0x40), UINT32_C(0x20), detached) ==
        NdsOverlayDestructorChainStatus::InvalidMetadata);
}

void test_register_global_object_pushes_node_and_preserves_existing_chain()
{
    GuestMemory memory = make_memory();
    NdsOverlayDestructorChain chain(memory);

    write_node(
        memory, kNodeA, 0u,
        kRamBase + 0x5000u, kRamBase + 0x6000u);
    write_node(
        memory, kNodeB, UINT32_C(0xDEADBEEF),
        UINT32_C(0xCAFEBABE), UINT32_C(0x12345678));
    write_word(memory, kHeadAddress, kNodeA);

    const uint32_t object = kOverlayBase + 0x90u;
    const uint32_t destructor = kRamBase + 0x7001u;
    require(chain.register_global_object(
        NDS_ARM9, kHeadAddress, object, destructor, kNodeB) ==
        NdsOverlayDestructorChainStatus::Ok);

    require(read_word(memory, kHeadAddress) == kNodeB);
    require(read_word(memory, kNodeB) == kNodeA);
    require(read_word(memory, kNodeB + 4u) == destructor);
    require(read_word(memory, kNodeB + 8u) == object);
    require(read_word(memory, kNodeA) == 0u);
    require(read_word(memory, kNodeA + 4u) == kRamBase + 0x5000u);
    require(read_word(memory, kNodeA + 8u) == kRamBase + 0x6000u);

    std::vector<NdsOverlayDestructorRecord> detached;
    require(chain.detach_for_overlay(
        NDS_ARM9, kHeadAddress, kOverlayBase,
        kOverlayCodeSize, kOverlayBssSize, detached) ==
        NdsOverlayDestructorChainStatus::Ok);
    require(detached.size() == 1u);
    require(detached[0].chain_node_address == kNodeB);
    require(detached[0].destructor_pointer == destructor);
    require(detached[0].object_address == object);
    require(read_word(memory, kHeadAddress) == kNodeA);
}

void test_register_global_object_accepts_an_empty_global_chain()
{
    GuestMemory memory = make_memory();
    NdsOverlayDestructorChain chain(memory);
    write_word(memory, kHeadAddress, 0u);

    const uint32_t object = kOverlayBase + 0x10u;
    const uint32_t destructor = kOverlayBase + 0x21u;
    require(chain.register_global_object(
        NDS_ARM9, kHeadAddress, object, destructor, kNodeC) ==
        NdsOverlayDestructorChainStatus::Ok);

    require(read_word(memory, kHeadAddress) == kNodeC);
    require(read_word(memory, kNodeC) == 0u);
    require(read_word(memory, kNodeC + 4u) == destructor);
    require(read_word(memory, kNodeC + 8u) == object);
}

void test_register_global_object_rejects_duplicate_node_without_mutation()
{
    GuestMemory memory = make_memory();
    NdsOverlayDestructorChain chain(memory);
    write_node(memory, kNodeA, 0u, kOverlayBase + 0x11u, 0u);
    write_word(memory, kHeadAddress, kNodeA);

    const uint32_t old_dtor = read_word(memory, kNodeA + 4u);
    const uint32_t old_obj = read_word(memory, kNodeA + 8u);
    require(chain.register_global_object(
        NDS_ARM9, kHeadAddress, kOverlayBase + 0x20u,
        kOverlayBase + 0x31u, kNodeA) ==
        NdsOverlayDestructorChainStatus::DuplicateNode);

    require(read_word(memory, kHeadAddress) == kNodeA);
    require(read_word(memory, kNodeA) == 0u);
    require(read_word(memory, kNodeA + 4u) == old_dtor);
    require(read_word(memory, kNodeA + 8u) == old_obj);
}

void test_register_global_object_rejects_existing_cycle_without_mutation()
{
    GuestMemory memory = make_memory();
    NdsOverlayDestructorChain chain(memory);
    write_node(memory, kNodeA, kNodeA, kOverlayBase + 0x11u, 0u);
    write_node(
        memory, kNodeB, UINT32_C(0x11111111),
        UINT32_C(0x22222222), UINT32_C(0x33333333));
    write_word(memory, kHeadAddress, kNodeA);

    require(chain.register_global_object(
        NDS_ARM9, kHeadAddress, kOverlayBase + 0x20u,
        kOverlayBase + 0x31u, kNodeB) ==
        NdsOverlayDestructorChainStatus::ChainCycle);

    require(read_word(memory, kHeadAddress) == kNodeA);
    require(read_word(memory, kNodeA) == kNodeA);
    require(read_word(memory, kNodeB) == UINT32_C(0x11111111));
    require(read_word(memory, kNodeB + 4u) == UINT32_C(0x22222222));
    require(read_word(memory, kNodeB + 8u) == UINT32_C(0x33333333));
}

void test_register_global_object_rejects_unmapped_or_invalid_node()
{
    GuestMemory memory = make_memory();
    NdsOverlayDestructorChain chain(memory);
    write_word(memory, kHeadAddress, 0u);

    constexpr uint32_t UnmappedNode = UINT32_C(0x03000000);
    require(chain.register_global_object(
        NDS_ARM9, kHeadAddress, kOverlayBase + 0x20u,
        kOverlayBase + 0x31u, UnmappedNode) ==
        NdsOverlayDestructorChainStatus::MemoryUnavailable);
    require(read_word(memory, kHeadAddress) == 0u);

    require(chain.register_global_object(
        static_cast<NdsCpu>(99u), kHeadAddress,
        kOverlayBase + 0x20u, kOverlayBase + 0x31u, kNodeB) ==
        NdsOverlayDestructorChainStatus::InvalidCpu);
    require(chain.register_global_object(
        NDS_ARM9, kHeadAddress, kOverlayBase + 0x20u,
        kOverlayBase + 0x31u, kHeadAddress) ==
        NdsOverlayDestructorChainStatus::InvalidMetadata);
}

} // namespace

int main()
{
    test_detaches_matching_nodes_and_preserves_other_overlay_order();
    test_keeps_unrelated_nodes_and_leaves_memory_unchanged();
    test_detaches_object_with_null_destructor_without_invoking_it();
    test_rejects_cycles_without_mutating_the_chain();
    test_rejects_unmapped_node_without_mutating_the_head();
    test_rejects_invalid_cpu_and_overflowing_overlay_range();
    test_register_global_object_pushes_node_and_preserves_existing_chain();
    test_register_global_object_accepts_an_empty_global_chain();
    test_register_global_object_rejects_duplicate_node_without_mutation();
    test_register_global_object_rejects_existing_cycle_without_mutation();
    test_register_global_object_rejects_unmapped_or_invalid_node();
    return 0;
}

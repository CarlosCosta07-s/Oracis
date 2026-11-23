#include "oracis/machine/nds_overlay_loader.hpp"
#include "oracis/host/detail/sha1.hpp"

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

using oracis::machine::NdsOverlayImage;
using oracis::machine::NdsOverlayLoader;
using oracis::machine::NdsOverlayStatus;
using oracis::memory::GuestMemory;

namespace {

constexpr uint32_t kRamBase = UINT32_C(0x02000000);
constexpr uint32_t kRamSize = UINT32_C(0x2000);

GuestMemory make_memory()
{
    GuestMemory memory;
    const auto status = memory.map_ram(
        kRamBase, kRamSize,
        oracis::memory::Readable |
            oracis::memory::Writable |
            oracis::memory::Executable,
        "overlay-test-arm9",
        oracis::memory::CpuAccessArm9);
    assert(status == oracis::memory::Status::Ok);
    return memory;
}

std::vector<uint8_t> read_bytes(
    const GuestMemory &memory, uint32_t address, uint32_t size)
{
    std::vector<uint8_t> result;
    result.reserve(size);
    for (uint32_t offset = 0u; offset < size; ++offset) {
        const auto byte = memory.read(NDS_ARM9, address + offset, 1u);
        assert(byte.status == oracis::memory::Status::Ok);
        result.push_back(static_cast<uint8_t>(byte.value));
    }
    return result;
}

std::string hash_of(const std::vector<uint8_t> &bytes)
{
    return oracis::host::detail::sha1_hex(bytes);
}

void test_load_copies_image_and_zeros_bss()
{
    GuestMemory memory = make_memory();
    NdsOverlayLoader loader(memory);
    const std::vector<uint8_t> image_bytes{
        0x10u, 0x20u, 0x30u, 0x40u, 0x50u, 0x60u, 0x70u, 0x80u
    };
    const std::string digest = hash_of(image_bytes);
    const NdsOverlayImage image{
        NDS_ARM9, 12u, kRamBase, 8u, 4u,
        0u, 0u,
        image_bytes.data(), digest.c_str()
    };

    assert(loader.load(image) == NdsOverlayStatus::Ok);
    assert(loader.loaded_count() == 1u);
    assert(loader.is_loaded(NDS_ARM9, 12u));
    assert((read_bytes(memory, kRamBase, 12u) ==
        std::vector<uint8_t>{
            0x10u, 0x20u, 0x30u, 0x40u,
            0x50u, 0x60u, 0x70u, 0x80u,
            0u, 0u, 0u, 0u
        }));
    assert(memory.validate_execute(NDS_ARM9, kRamBase, 8u) ==
        oracis::memory::Status::Ok);
}

void test_hash_failure_does_not_mutate_memory()
{
    GuestMemory memory = make_memory();
    NdsOverlayLoader loader(memory);
    const std::vector<uint8_t> image_bytes{1u, 2u, 3u, 4u};
    const NdsOverlayImage image{
        NDS_ARM9, 1u, kRamBase, 4u, 4u,
        0u, 0u, image_bytes.data(),
        "0000000000000000000000000000000000000000"
    };

    assert(loader.load(image) == NdsOverlayStatus::ImageHashMismatch);
    assert(loader.loaded_count() == 0u);
    assert((read_bytes(memory, kRamBase, 8u) ==
        std::vector<uint8_t>(8u, 0u)));
}

void test_duplicate_identity_and_overlapping_modules()
{
    GuestMemory memory = make_memory();
    NdsOverlayLoader loader(memory);
    const std::vector<uint8_t> bytes(8u, 0xA5u);
    const std::string digest = hash_of(bytes);
    const NdsOverlayImage first{
        NDS_ARM9, 4u, kRamBase, 8u, 0u,
        0u, 0u, bytes.data(), digest.c_str()
    };

    assert(loader.load(first) == NdsOverlayStatus::Ok);
    assert(loader.load(first) == NdsOverlayStatus::AlreadyLoaded);

    const NdsOverlayImage overlap{
        NDS_ARM9, 5u, kRamBase + 4u, 8u, 0u,
        0u, 0u, bytes.data(), digest.c_str()
    };
    assert(loader.load(overlap) == NdsOverlayStatus::Overlap);
    assert(loader.loaded_count() == 1u);
}

void test_unload_requires_quiescence_and_clears_memory()
{
    GuestMemory memory = make_memory();
    NdsOverlayLoader loader(memory);
    const std::vector<uint8_t> bytes(8u, 0x5Au);
    const std::string digest = hash_of(bytes);
    const NdsOverlayImage image{
        NDS_ARM9, 20u, kRamBase + 0x100u, 8u, 4u,
        0u, 0u, bytes.data(), digest.c_str()
    };

    assert(loader.load(image) == NdsOverlayStatus::Ok);
    assert(loader.unload(
        NDS_ARM9, 20u, false, kRamBase) == NdsOverlayStatus::Busy);
    assert(loader.unload(
        NDS_ARM9, 20u, true, kRamBase + 0x100u) == NdsOverlayStatus::Busy);
    assert(loader.is_loaded(NDS_ARM9, 20u));

    assert(loader.unload(
        NDS_ARM9, 20u, true, kRamBase) == NdsOverlayStatus::Ok);
    assert(loader.loaded_count() == 0u);
    assert((read_bytes(memory, kRamBase + 0x100u, 12u) ==
        std::vector<uint8_t>(12u, 0u)));
}

void test_clear_zeros_all_modules_and_resets_inventory()
{
    GuestMemory memory = make_memory();
    NdsOverlayLoader loader(memory);
    const std::vector<uint8_t> bytes_a(8u, 0x31u);
    const std::vector<uint8_t> bytes_b(8u, 0x72u);
    const std::string digest_a = hash_of(bytes_a);
    const std::string digest_b = hash_of(bytes_b);
    const NdsOverlayImage first{
        NDS_ARM9, 30u, kRamBase + 0x200u, 8u, 4u,
        0u, 0u, bytes_a.data(), digest_a.c_str()
    };
    const NdsOverlayImage second{
        NDS_ARM9, 31u, kRamBase + 0x300u, 8u, 4u,
        0u, 0u, bytes_b.data(), digest_b.c_str()
    };

    assert(loader.load(first) == NdsOverlayStatus::Ok);
    assert(loader.load(second) == NdsOverlayStatus::Ok);
    assert(loader.loaded_count() == 2u);
    assert(loader.clear() == NdsOverlayStatus::Ok);
    assert(loader.loaded_count() == 0u);
    assert((read_bytes(memory, kRamBase + 0x200u, 12u) ==
        std::vector<uint8_t>(12u, 0u)));
    assert((read_bytes(memory, kRamBase + 0x300u, 12u) ==
        std::vector<uint8_t>(12u, 0u)));
}

void write_u32_le(
    std::vector<uint8_t> &bytes, std::size_t offset, uint32_t value)
{
    assert(offset <= bytes.size() && bytes.size() - offset >= 4u);
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1u] = static_cast<uint8_t>(value >> 8u);
    bytes[offset + 2u] = static_cast<uint8_t>(value >> 16u);
    bytes[offset + 3u] = static_cast<uint8_t>(value >> 24u);
}

void test_load_retains_static_initializer_order_and_arm_modes()
{
    GuestMemory memory = make_memory();
    NdsOverlayLoader loader(memory);
    constexpr uint32_t OverlayBase = kRamBase + 0x400u;
    std::vector<uint8_t> image_bytes(0x20u, 0u);
    write_u32_le(image_bytes, 0x18u, OverlayBase | 1u);
    write_u32_le(image_bytes, 0x1Cu, OverlayBase + 4u);
    const std::string digest = hash_of(image_bytes);
    const NdsOverlayImage image{
        NDS_ARM9, 40u, OverlayBase, 0x20u, 4u,
        OverlayBase + 0x18u, OverlayBase + 0x20u,
        image_bytes.data(), digest.c_str()
    };

    assert(loader.static_initializers(NDS_ARM9, 40u) == nullptr);
    assert(loader.load(image) == NdsOverlayStatus::Ok);
    const auto *initializers = loader.static_initializers(NDS_ARM9, 40u);
    assert(initializers != nullptr);
    assert(initializers->size() == 2u);
    assert((*initializers)[0].address == OverlayBase);
    assert((*initializers)[0].thumb);
    assert((*initializers)[1].address == OverlayBase + 4u);
    assert(!(*initializers)[1].thumb);
    assert((read_bytes(memory, OverlayBase + 0x20u, 4u) ==
        std::vector<uint8_t>(4u, 0u)));
}

void test_rejects_invalid_static_initializer_pointer_before_writing()
{
    GuestMemory memory = make_memory();
    NdsOverlayLoader loader(memory);
    constexpr uint32_t OverlayBase = kRamBase + 0x600u;
    std::vector<uint8_t> image_bytes(0x10u, 0u);
    write_u32_le(image_bytes, 8u, (OverlayBase + 0x20u) | 1u);
    const std::string digest = hash_of(image_bytes);
    const NdsOverlayImage image{
        NDS_ARM9, 41u, OverlayBase, 0x10u, 4u,
        OverlayBase + 8u, OverlayBase + 12u,
        image_bytes.data(), digest.c_str()
    };

    assert(loader.load(image) == NdsOverlayStatus::InvalidMetadata);
    assert(loader.loaded_count() == 0u);
    assert(loader.static_initializers(NDS_ARM9, 41u) == nullptr);
    assert((read_bytes(memory, OverlayBase, 0x14u) ==
        std::vector<uint8_t>(0x14u, 0u)));
}

void test_zero_static_initializer_terminator_stops_the_sequence()
{
    GuestMemory memory = make_memory();
    NdsOverlayLoader loader(memory);
    constexpr uint32_t OverlayBase = kRamBase + 0x800u;
    std::vector<uint8_t> image_bytes(0x20u, 0u);
    write_u32_le(image_bytes, 0x18u, OverlayBase + 4u);
    write_u32_le(image_bytes, 0x1Cu, UINT32_C(0));
    const std::string digest = hash_of(image_bytes);
    const NdsOverlayImage image{
        NDS_ARM9, 42u, OverlayBase, 0x20u, 0u,
        OverlayBase + 0x18u, OverlayBase + 0x20u,
        image_bytes.data(), digest.c_str()
    };

    assert(loader.load(image) == NdsOverlayStatus::Ok);
    const auto *initializers = loader.static_initializers(NDS_ARM9, 42u);
    assert(initializers != nullptr);
    assert(initializers->size() == 1u);
    assert((*initializers)[0].address == OverlayBase + 4u);
    assert((*initializers)[0].thumb);
}

void test_rejects_invalid_static_init_range()
{
    GuestMemory memory = make_memory();
    NdsOverlayLoader loader(memory);
    const std::vector<uint8_t> bytes(8u, 0x11u);
    const std::string digest = hash_of(bytes);
    const NdsOverlayImage image{
        NDS_ARM9, 22u, kRamBase, 8u, 0u,
        kRamBase + 4u, kRamBase + 12u,
        bytes.data(), digest.c_str()
    };
    assert(loader.load(image) == NdsOverlayStatus::InvalidMetadata);
}

} // namespace

int main()
{
    test_load_copies_image_and_zeros_bss();
    test_hash_failure_does_not_mutate_memory();
    test_duplicate_identity_and_overlapping_modules();
    test_unload_requires_quiescence_and_clears_memory();
    test_clear_zeros_all_modules_and_resets_inventory();
    test_load_retains_static_initializer_order_and_arm_modes();
    test_rejects_invalid_static_initializer_pointer_before_writing();
    test_zero_static_initializer_terminator_stops_the_sequence();
    test_rejects_invalid_static_init_range();
    return 0;
}

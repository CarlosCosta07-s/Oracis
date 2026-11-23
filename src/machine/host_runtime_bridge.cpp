#include "oracis/host/runtime_bridge.h"
#include "oracis/host/detail/sha1.hpp"
#include "oracis/host/detail/nds_cartridge_header.hpp"

#include "oracis/machine/nds_machine.hpp"
#include "oracis/memory/nds_2d_bg_compositor.hpp"
#include "oracis/recomp/static_dispatch.hpp"
#include "oracis/recomp/static_dispatch_abi.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <new>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#ifdef ORACIS_WITH_PLATINUM_STATIC_BANKS
extern "C" {
extern const NdsDispatchEntry
    g_dispatch_pokemon_platinum_usa_rev1_arm9[];
extern const unsigned
    g_dispatch_pokemon_platinum_usa_rev1_arm9_len;
extern const NdsDispatchEntry
    g_dispatch_pokemon_platinum_usa_rev1_arm7[];
extern const unsigned
    g_dispatch_pokemon_platinum_usa_rev1_arm7_len;
}
#endif

#ifdef ORACIS_WITH_PLATINUM_OVERLAY_BANKS
extern "C" int oracis_register_compiled_overlay_banks(void);
#endif

namespace {

std::optional<oracis::machine::NdsMachine> &host_machine_storage() noexcept
{
    static std::optional<oracis::machine::NdsMachine> storage;
    return storage;
}

oracis::machine::NdsMachine &host_machine() noexcept
{
    auto &storage = host_machine_storage();
    if (!storage.has_value()) {
        storage.emplace(oracis::memory::SharedWramMode::Arm9All32K);
    }
    return *storage;
}

int g_last_machine_status =
    static_cast<int>(oracis::machine::NdsMachineStatus::Ok);
std::vector<uint8_t> g_game_card_image;

uint32_t read_u32_le(const uint8_t *bytes) noexcept
{
    return static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8u) |
        (static_cast<uint32_t>(bytes[2]) << 16u) |
        (static_cast<uint32_t>(bytes[3]) << 24u);
}

bool validate_game_card_segment(
    const OracisHostProgramSegment *segment,
    const uint8_t *image,
    uint32_t image_size,
    uint32_t header_offset,
    uint32_t &rom_offset,
    uint64_t &rom_end) noexcept
{
    if (segment == nullptr || image == nullptr || segment->contents == nullptr ||
        segment->size == 0u || segment->entry_thumb > 1u ||
        header_offset > image_size || image_size - header_offset < 16u) {
        return false;
    }

    rom_offset = read_u32_le(image + header_offset);
    const uint32_t raw_entry = read_u32_le(image + header_offset + 4u);
    const uint32_t load_address = read_u32_le(image + header_offset + 8u);
    const uint32_t size = read_u32_le(image + header_offset + 12u);
    const bool entry_thumb = (raw_entry & 1u) != 0u;
    const uint32_t entry_pc = raw_entry & ~UINT32_C(1);
    const uint64_t load_end =
        static_cast<uint64_t>(load_address) + size;
    const uint64_t instruction_size = entry_thumb ? UINT64_C(2) : UINT64_C(4);
    const uint64_t entry_end =
        static_cast<uint64_t>(entry_pc) + instruction_size;
    const uint32_t entry_alignment = entry_thumb ? 2u : 4u;

    rom_end = static_cast<uint64_t>(rom_offset) + size;
    if (rom_offset < UINT32_C(0x200) || size == 0u ||
        rom_end > image_size || load_end > UINT64_C(0x100000000) ||
        entry_pc % entry_alignment != 0u ||
        static_cast<uint64_t>(entry_pc) < load_address ||
        entry_end > load_end) {
        return false;
    }

    if (segment->load_address != load_address ||
        segment->size != size ||
        segment->entry_pc != entry_pc ||
        segment->entry_thumb != (entry_thumb ? 1u : 0u)) {
        return false;
    }

    return std::equal(
        segment->contents,
        segment->contents + segment->size,
        image + rom_offset);
}


struct IrqProviderContext {
    OracisHostIrqContextProvider provider = nullptr;
    void *user = nullptr;
};

bool invoke_irq_provider(
    void *opaque, NdsCpu cpu, const ArmCpuState *state,
    uint32_t *return_address, uint32_t *vector_base)
{
    auto *context = static_cast<IrqProviderContext *>(opaque);
    if (context == nullptr || context->provider == nullptr) return false;
    return context->provider(
        context->user, cpu, state, return_address, vector_base) != 0;
}

int host_overlay_status(oracis::machine::NdsOverlayStatus status) noexcept
{
    using oracis::machine::NdsOverlayStatus;
    switch (status) {
    case NdsOverlayStatus::Ok:
        return ORACIS_HOST_RUNTIME_OK;
    case NdsOverlayStatus::AlreadyLoaded:
        return ORACIS_HOST_RUNTIME_OVERLAY_ALREADY_LOADED;
    case NdsOverlayStatus::NotLoaded:
        return ORACIS_HOST_RUNTIME_OVERLAY_NOT_LOADED;
    case NdsOverlayStatus::InvalidCpu:
    case NdsOverlayStatus::InvalidMetadata:
        return ORACIS_HOST_RUNTIME_OVERLAY_INVALID;
    case NdsOverlayStatus::ImageHashMismatch:
        return ORACIS_HOST_RUNTIME_OVERLAY_HASH_MISMATCH;
    case NdsOverlayStatus::MemoryUnavailable:
        return ORACIS_HOST_RUNTIME_OVERLAY_MEMORY_UNAVAILABLE;
    case NdsOverlayStatus::Overlap:
        return ORACIS_HOST_RUNTIME_OVERLAY_OVERLAP;
    case NdsOverlayStatus::Busy:
        return ORACIS_HOST_RUNTIME_OVERLAY_BUSY;
    case NdsOverlayStatus::WriteFailed:
        return ORACIS_HOST_RUNTIME_OVERLAY_WRITE_FAILED;
    case NdsOverlayStatus::IdentityConflict:
        return ORACIS_HOST_RUNTIME_OVERLAY_IDENTITY_CONFLICT;
    case NdsOverlayStatus::NotInitialized:
        return ORACIS_HOST_RUNTIME_NOT_INITIALIZED;
    case NdsOverlayStatus::ProgramNotLoaded:
        return ORACIS_HOST_RUNTIME_NO_PROGRAM_LOADED;
    }
    return ORACIS_HOST_RUNTIME_OVERLAY_INVALID;
}

bool has_both_program_banks() noexcept
{
    auto &registry = oracis::recomp::global_static_dispatch_registry();
    return registry.bank_count(NDS_ARM9) != 0u &&
           registry.bank_count(NDS_ARM7) != 0u;
}

struct ExpectedLocalSegment {
    NdsCpu cpu;
    uint32_t load_address;
    uint32_t size;
    uint32_t entry_pc;
    const char *sha1;
};

constexpr ExpectedLocalSegment kExpectedArm9{
    NDS_ARM9, 0x02000000u, 0x001023F8u, 0x02000800u,
    "7ec963e737f0f61efcbbf5c7a3d33cbf378143f8"
};
constexpr ExpectedLocalSegment kExpectedArm7{
    NDS_ARM7, 0x02380000u, 0x000277FCu, 0x02380000u,
    "082bb349ae7d057edbbd91a07316b1402b18b803"
};

int read_verified_segment(
    const std::filesystem::path &path,
    const ExpectedLocalSegment &expected,
    std::vector<uint8_t> &contents)
{
    std::error_code error;
    const auto file_status = std::filesystem::status(path, error);
    if (error == std::errc::no_such_file_or_directory ||
        file_status.type() == std::filesystem::file_type::not_found) {
        return ORACIS_HOST_RUNTIME_LOCAL_IMAGE_MISSING;
    }
    if (error) return ORACIS_HOST_RUNTIME_LOCAL_IMAGE_IO_ERROR;
    if (!std::filesystem::is_regular_file(file_status)) {
        return ORACIS_HOST_RUNTIME_LOCAL_IMAGE_INVALID;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) return ORACIS_HOST_RUNTIME_LOCAL_IMAGE_IO_ERROR;

    contents.resize(expected.size);
    stream.read(
        reinterpret_cast<char *>(contents.data()),
        static_cast<std::streamsize>(contents.size()));
    if (stream.gcount() != static_cast<std::streamsize>(contents.size())) {
        return stream.bad()
            ? ORACIS_HOST_RUNTIME_LOCAL_IMAGE_IO_ERROR
            : ORACIS_HOST_RUNTIME_LOCAL_IMAGE_INVALID;
    }
    if (stream.bad()) return ORACIS_HOST_RUNTIME_LOCAL_IMAGE_IO_ERROR;

    char extra_byte = 0;
    stream.read(&extra_byte, 1);
    if (stream.gcount() != 0) {
        return ORACIS_HOST_RUNTIME_LOCAL_IMAGE_INVALID;
    }
    if (stream.bad()) return ORACIS_HOST_RUNTIME_LOCAL_IMAGE_IO_ERROR;

    if (oracis::host::detail::sha1_hex(contents) != expected.sha1) {
        return ORACIS_HOST_RUNTIME_LOCAL_IMAGE_INVALID;
    }
    return ORACIS_HOST_RUNTIME_OK;
}

int read_verified_platinum_header(
    const std::filesystem::path &path,
    std::vector<uint8_t> &contents)
{
    using oracis::host::detail::NdsCartridgeHeaderStoredSize;
    std::error_code error;
    const auto file_status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory ||
        file_status.type() == std::filesystem::file_type::not_found) {
        return ORACIS_HOST_RUNTIME_LOCAL_IMAGE_MISSING;
    }
    if (error) return ORACIS_HOST_RUNTIME_LOCAL_IMAGE_IO_ERROR;
    if (std::filesystem::is_symlink(file_status) ||
        !std::filesystem::is_regular_file(file_status)) {
        return ORACIS_HOST_RUNTIME_LOCAL_IMAGE_INVALID;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) return ORACIS_HOST_RUNTIME_LOCAL_IMAGE_IO_ERROR;
    contents.resize(NdsCartridgeHeaderStoredSize);
    stream.read(
        reinterpret_cast<char *>(contents.data()),
        static_cast<std::streamsize>(contents.size()));
    if (stream.gcount() != static_cast<std::streamsize>(contents.size())) {
        return stream.bad()
            ? ORACIS_HOST_RUNTIME_LOCAL_IMAGE_IO_ERROR
            : ORACIS_HOST_RUNTIME_LOCAL_IMAGE_INVALID;
    }
    if (stream.bad()) return ORACIS_HOST_RUNTIME_LOCAL_IMAGE_IO_ERROR;

    char extra_byte = 0;
    stream.read(&extra_byte, 1);
    if (stream.gcount() != 0 || stream.bad() ||
        !oracis::host::detail::validate_platinum_usa_rev1_boot_header(
            contents.data(), contents.size())) {
        return ORACIS_HOST_RUNTIME_LOCAL_IMAGE_INVALID;
    }
    return ORACIS_HOST_RUNTIME_OK;
}

} // namespace

extern "C" int oracis_host_runtime_initialize(void)
{
    auto &machine = host_machine();
    if (machine.initialized()) {
        return ORACIS_HOST_RUNTIME_ALREADY_INITIALIZED;
    }

    const auto status = machine.initialize();
    g_last_machine_status = static_cast<int>(status);
    return status == oracis::machine::NdsMachineStatus::Ok
        ? ORACIS_HOST_RUNTIME_OK
        : ORACIS_HOST_RUNTIME_MACHINE_ERROR;
}

extern "C" int oracis_host_runtime_shutdown(void)
{
    auto &machine = host_machine();
    if (machine.initialized()) {
        machine.game_card_control().unmount_image();
    }
    std::vector<uint8_t>().swap(g_game_card_image);
    const auto status = machine.shutdown();
    g_last_machine_status = static_cast<int>(status);

    /*
     * The registry stores non-owning table/validation pointers. Once shutdown
     * has stopped both CPU slices, discard every registration (compiled or
     * host-provided) so caller-owned tables cannot outlive this machine.
     * Clear even after a partial unbind failure: shutdown has already ended
     * execution slices, and retaining stale function pointers is unsafe.
     */
    oracis_static_dispatch_clear();
    /*
     * NdsMachine::shutdown unbinds process-global callbacks but intentionally
     * keeps its installed device map. Destroy the stopped instance so a later
     * host initialize can construct a clean machine with fresh devices.
     */
    if (status == oracis::machine::NdsMachineStatus::Ok) {
        host_machine_storage().reset();
    }

    return status == oracis::machine::NdsMachineStatus::Ok
        ? ORACIS_HOST_RUNTIME_OK
        : ORACIS_HOST_RUNTIME_MACHINE_ERROR;
}

extern "C" int oracis_host_runtime_mount_game_card_image(
    const uint8_t *bytes, uint32_t size)
{
    auto &machine = host_machine();
    if (!machine.initialized()) return ORACIS_HOST_RUNTIME_NOT_INITIALIZED;
    if (bytes == nullptr || size < UINT32_C(0x200) ||
        size > UINT32_C(0x20000000)) {
        return ORACIS_HOST_RUNTIME_LOCAL_IMAGE_INVALID;
    }

    try {
        std::vector<uint8_t> candidate(bytes, bytes + size);
        const auto status = machine.game_card_control().mount_image(
            candidate.data(), size);
        if (status != oracis::memory::Status::Ok) {
            return status == oracis::memory::Status::InvalidRegion
                ? ORACIS_HOST_RUNTIME_LOCAL_IMAGE_INVALID
                : ORACIS_HOST_RUNTIME_GAME_CARD_ERROR;
        }
        /* vector::swap preserves the allocation address installed in the device. */
        g_game_card_image.swap(candidate);
        return ORACIS_HOST_RUNTIME_OK;
    } catch (const std::bad_alloc &) {
        return ORACIS_HOST_RUNTIME_LOCAL_IMAGE_IO_ERROR;
    } catch (...) {
        return ORACIS_HOST_RUNTIME_LOCAL_IMAGE_IO_ERROR;
    }
}

extern "C" int oracis_host_runtime_load_game_card_arm7_bios(
    const uint8_t *bytes, uint32_t size)
{
    auto &machine = host_machine();
    if (!machine.initialized()) return ORACIS_HOST_RUNTIME_NOT_INITIALIZED;
    if (bytes == nullptr ||
        size != oracis::memory::NdsGameCardKey1::Arm7BiosBytes) {
        return ORACIS_HOST_RUNTIME_INVALID_ARGUMENT;
    }

    const uint8_t *key_table = bytes +
        oracis::memory::NdsGameCardKey1::Arm7BiosKeyTableOffset;
    const auto status = machine.game_card_control().load_key1_key_table(
        key_table, oracis::memory::NdsGameCardKey1::KeyTableBytes);
    return status == oracis::memory::Status::Ok
        ? ORACIS_HOST_RUNTIME_OK
        : ORACIS_HOST_RUNTIME_GAME_CARD_ERROR;
}

extern "C" int oracis_host_runtime_set_game_card_chip_id(uint32_t chip_id)
{
    auto &machine = host_machine();
    if (!machine.initialized()) return ORACIS_HOST_RUNTIME_NOT_INITIALIZED;
    if (!machine.game_card_control().mounted()) {
        return ORACIS_HOST_RUNTIME_GAME_CARD_ERROR;
    }
    const auto status = machine.game_card_control().set_chip_id(chip_id);
    return status == oracis::memory::Status::Ok
        ? ORACIS_HOST_RUNTIME_OK
        : ORACIS_HOST_RUNTIME_GAME_CARD_ERROR;
}

extern "C" int oracis_host_runtime_decrypt_game_card_secure_area_prefix(
    uint8_t *output, uint32_t output_size, uint32_t *identifier_valid)
{
    auto &machine = host_machine();
    if (!machine.initialized()) return ORACIS_HOST_RUNTIME_NOT_INITIALIZED;
    if (identifier_valid == nullptr) {
        return ORACIS_HOST_RUNTIME_INVALID_ARGUMENT;
    }
    *identifier_valid = 0u;
    if (output == nullptr) return ORACIS_HOST_RUNTIME_INVALID_ARGUMENT;

    bool valid = false;
    const auto status = machine.game_card_control().decrypt_secure_area_prefix(
        output, output_size, valid);
    if (status == oracis::memory::Status::InvalidRegion ||
        status == oracis::memory::Status::InvalidWidth) {
        return ORACIS_HOST_RUNTIME_INVALID_ARGUMENT;
    }
    if (status != oracis::memory::Status::Ok) {
        return ORACIS_HOST_RUNTIME_GAME_CARD_ERROR;
    }
    *identifier_valid = valid ? 1u : 0u;
    return ORACIS_HOST_RUNTIME_OK;
}

extern "C" int oracis_host_runtime_unmount_game_card_image(void)
{
    auto &machine = host_machine();
    if (!machine.initialized()) return ORACIS_HOST_RUNTIME_NOT_INITIALIZED;
    machine.game_card_control().unmount_image();
    std::vector<uint8_t>().swap(g_game_card_image);
    return ORACIS_HOST_RUNTIME_OK;
}

extern "C" int oracis_host_runtime_load_program_segments(
    const OracisHostProgramSegment *arm9,
    const OracisHostProgramSegment *arm7)
{
    auto &machine = host_machine();
    if (!machine.initialized()) return ORACIS_HOST_RUNTIME_NOT_INITIALIZED;
    if (arm9 == nullptr || arm7 == nullptr ||
        arm9->entry_thumb > 1u || arm7->entry_thumb > 1u) {
        g_last_machine_status = static_cast<int>(
            oracis::machine::NdsMachineStatus::InvalidProgramImage);
        return ORACIS_HOST_RUNTIME_MACHINE_ERROR;
    }

    const oracis::machine::NdsProgramSegment arm9_segment{
        NDS_ARM9, arm9->load_address, arm9->size, arm9->entry_pc,
        arm9->entry_thumb != 0u, arm9->contents
    };
    const oracis::machine::NdsProgramSegment arm7_segment{
        NDS_ARM7, arm7->load_address, arm7->size, arm7->entry_pc,
        arm7->entry_thumb != 0u, arm7->contents
    };
    const auto status = machine.load_program_segments(
        arm9_segment, arm7_segment);
    g_last_machine_status = static_cast<int>(status);
    return status == oracis::machine::NdsMachineStatus::Ok
        ? ORACIS_HOST_RUNTIME_OK
        : ORACIS_HOST_RUNTIME_MACHINE_ERROR;
}

extern "C" int oracis_host_runtime_load_game_card_program_segments(
    const OracisHostProgramSegment *arm9,
    const OracisHostProgramSegment *arm7)
{
    auto &machine = host_machine();
    if (!machine.initialized()) return ORACIS_HOST_RUNTIME_NOT_INITIALIZED;
    if (arm9 == nullptr || arm7 == nullptr ||
        arm9->contents == nullptr || arm7->contents == nullptr) {
        return ORACIS_HOST_RUNTIME_INVALID_ARGUMENT;
    }
    if (!machine.game_card_control().mounted() || g_game_card_image.empty()) {
        return ORACIS_HOST_RUNTIME_GAME_CARD_ERROR;
    }

    const uint32_t image_size = static_cast<uint32_t>(g_game_card_image.size());
    const uint8_t *image = g_game_card_image.data();
    uint32_t arm9_rom_offset = 0u;
    uint32_t arm7_rom_offset = 0u;
    uint64_t arm9_rom_end = 0u;
    uint64_t arm7_rom_end = 0u;

    if (!validate_game_card_segment(
            arm9, image, image_size, UINT32_C(0x20),
            arm9_rom_offset, arm9_rom_end) ||
        !validate_game_card_segment(
            arm7, image, image_size, UINT32_C(0x30),
            arm7_rom_offset, arm7_rom_end) ||
        arm9_rom_offset !=
            oracis::memory::NdsGameCardControl::SecureAreaDataOffset ||
        arm9->size <
            oracis::memory::NdsGameCardKey1::SecureAreaEncryptedBytes ||
        (arm9_rom_offset < arm7_rom_end &&
         arm7_rom_offset < arm9_rom_end)) {
        return ORACIS_HOST_RUNTIME_LOCAL_IMAGE_INVALID;
    }

    try {
        std::vector<uint8_t> arm9_bytes(
            arm9->contents, arm9->contents + arm9->size);
        std::array<uint8_t,
            oracis::memory::NdsGameCardKey1::SecureAreaEncryptedBytes>
            secure_area_prefix{};
        bool identifier_valid = false;
        const auto decrypt_status =
            machine.game_card_control().decrypt_secure_area_prefix(
                secure_area_prefix.data(),
                static_cast<uint32_t>(secure_area_prefix.size()),
                identifier_valid);
        if (decrypt_status != oracis::memory::Status::Ok ||
            !identifier_valid) {
            return ORACIS_HOST_RUNTIME_GAME_CARD_ERROR;
        }

        std::copy(
            secure_area_prefix.begin(), secure_area_prefix.end(),
            arm9_bytes.begin());

        const oracis::machine::NdsProgramSegment arm9_segment{
            NDS_ARM9, arm9->load_address, arm9->size, arm9->entry_pc,
            arm9->entry_thumb != 0u, arm9_bytes.data()
        };
        const oracis::machine::NdsProgramSegment arm7_segment{
            NDS_ARM7, arm7->load_address, arm7->size, arm7->entry_pc,
            arm7->entry_thumb != 0u, arm7->contents
        };
        const auto load_status = machine.load_program_segments(
            arm9_segment, arm7_segment,
            image,
            static_cast<uint32_t>(
                oracis::host::detail::NdsCartridgeHeaderStoredSize));
        g_last_machine_status = static_cast<int>(load_status);
        return load_status == oracis::machine::NdsMachineStatus::Ok
            ? ORACIS_HOST_RUNTIME_OK
            : ORACIS_HOST_RUNTIME_MACHINE_ERROR;
    } catch (const std::bad_alloc &) {
        return ORACIS_HOST_RUNTIME_LOCAL_IMAGE_IO_ERROR;
    } catch (...) {
        return ORACIS_HOST_RUNTIME_LOCAL_IMAGE_IO_ERROR;
    }
}

extern "C" int oracis_host_runtime_load_mounted_game_card_program(void)
{
    auto &machine = host_machine();
    if (!machine.initialized()) return ORACIS_HOST_RUNTIME_NOT_INITIALIZED;
    if (!machine.game_card_control().mounted() || g_game_card_image.empty()) {
        return ORACIS_HOST_RUNTIME_GAME_CARD_ERROR;
    }

    const uint32_t image_size = static_cast<uint32_t>(g_game_card_image.size());
    const uint8_t *image = g_game_card_image.data();
    if (image_size < UINT32_C(0x40)) {
        return ORACIS_HOST_RUNTIME_LOCAL_IMAGE_INVALID;
    }

    const uint32_t arm9_rom_offset = read_u32_le(image + UINT32_C(0x20));
    const uint32_t arm9_entry_raw = read_u32_le(image + UINT32_C(0x24));
    const uint32_t arm9_load_address = read_u32_le(image + UINT32_C(0x28));
    const uint32_t arm9_size = read_u32_le(image + UINT32_C(0x2C));
    const uint32_t arm7_rom_offset = read_u32_le(image + UINT32_C(0x30));
    const uint32_t arm7_entry_raw = read_u32_le(image + UINT32_C(0x34));
    const uint32_t arm7_load_address = read_u32_le(image + UINT32_C(0x38));
    const uint32_t arm7_size = read_u32_le(image + UINT32_C(0x3C));
    const uint64_t arm9_rom_end =
        static_cast<uint64_t>(arm9_rom_offset) + arm9_size;
    const uint64_t arm7_rom_end =
        static_cast<uint64_t>(arm7_rom_offset) + arm7_size;

    if (arm9_rom_offset < UINT32_C(0x200) ||
        arm7_rom_offset < UINT32_C(0x200) ||
        arm9_size == 0u || arm7_size == 0u ||
        arm9_rom_end > image_size || arm7_rom_end > image_size) {
        return ORACIS_HOST_RUNTIME_LOCAL_IMAGE_INVALID;
    }

    const OracisHostProgramSegment arm9{
        arm9_load_address,
        arm9_size,
        arm9_entry_raw & ~UINT32_C(1),
        arm9_entry_raw & UINT32_C(1),
        image + arm9_rom_offset
    };
    const OracisHostProgramSegment arm7{
        arm7_load_address,
        arm7_size,
        arm7_entry_raw & ~UINT32_C(1),
        arm7_entry_raw & UINT32_C(1),
        image + arm7_rom_offset
    };
    return oracis_host_runtime_load_game_card_program_segments(
        &arm9, &arm7);
}

extern "C" int oracis_host_runtime_load_local_platinum_segments(
    const char *directory)
{
    if (!host_machine().initialized()) {
        return ORACIS_HOST_RUNTIME_NOT_INITIALIZED;
    }

    try {
        const std::filesystem::path root = directory == nullptr
            ? std::filesystem::path(
                ".oracis/local/pokemon-platinum-usa-rev1")
            : std::filesystem::path(directory);
        if (root.empty()) return ORACIS_HOST_RUNTIME_LOCAL_IMAGE_IO_ERROR;

        std::vector<uint8_t> arm9_bytes;
        std::vector<uint8_t> arm7_bytes;
        std::vector<uint8_t> cartridge_header;
        int status = read_verified_segment(
            root / "arm9.bin", kExpectedArm9, arm9_bytes);
        if (status != ORACIS_HOST_RUNTIME_OK) return status;
        status = read_verified_segment(
            root / "arm7.bin", kExpectedArm7, arm7_bytes);
        if (status != ORACIS_HOST_RUNTIME_OK) return status;
        status = read_verified_platinum_header(
            root / "header.bin", cartridge_header);
        if (status != ORACIS_HOST_RUNTIME_OK) return status;

        const OracisHostProgramSegment arm9{
            kExpectedArm9.load_address,
            kExpectedArm9.size,
            kExpectedArm9.entry_pc,
            0u,
            arm9_bytes.data()
        };
        const OracisHostProgramSegment arm7{
            kExpectedArm7.load_address,
            kExpectedArm7.size,
            kExpectedArm7.entry_pc,
            0u,
            arm7_bytes.data()
        };
        const oracis::machine::NdsProgramSegment arm9_image{
            NDS_ARM9, arm9.load_address, arm9.size, arm9.entry_pc,
            false, arm9.contents
        };
        const oracis::machine::NdsProgramSegment arm7_image{
            NDS_ARM7, arm7.load_address, arm7.size, arm7.entry_pc,
            false, arm7.contents
        };
        const auto load_status = host_machine().load_program_segments(
            arm9_image, arm7_image,
            cartridge_header.data(),
            static_cast<uint32_t>(cartridge_header.size()));
        g_last_machine_status = static_cast<int>(load_status);
        return load_status == oracis::machine::NdsMachineStatus::Ok
            ? ORACIS_HOST_RUNTIME_OK
            : ORACIS_HOST_RUNTIME_MACHINE_ERROR;
    } catch (const std::bad_alloc &) {
        return ORACIS_HOST_RUNTIME_LOCAL_IMAGE_IO_ERROR;
    } catch (const std::filesystem::filesystem_error &) {
        return ORACIS_HOST_RUNTIME_LOCAL_IMAGE_IO_ERROR;
    } catch (...) {
        return ORACIS_HOST_RUNTIME_LOCAL_IMAGE_IO_ERROR;
    }
}

extern "C" int oracis_host_runtime_program_loaded(void)
{
    return host_machine().initialized() && host_machine().program_loaded()
        ? 1 : 0;
}

extern "C" int oracis_host_runtime_render_scanline(
    uint32_t engine_index,
    uint32_t scanline,
    uint16_t *rgb555,
    uint32_t pixel_capacity,
    OracisHostVideoScanlineReport *report)
{
    if (report != nullptr) {
        *report = {};
    }
    if (rgb555 == nullptr || pixel_capacity < 256u ||
        engine_index > 1u || scanline >=
            oracis::memory::NdsDisplayControl::VisibleScanlines) {
        return ORACIS_HOST_RUNTIME_INVALID_ARGUMENT;
    }

    auto &machine = host_machine();
    if (!machine.initialized()) {
        return ORACIS_HOST_RUNTIME_NOT_INITIALIZED;
    }

    const uint32_t dispcnt =
        machine.display_control().dispcnt(engine_index);
    const uint32_t display_mode = (dispcnt >> 16u) & UINT32_C(3);
    const auto apply_master_brightness =
        [&machine, engine_index](uint16_t *pixels) noexcept {
            for (uint32_t x = 0u; x < 256u; ++x) {
                pixels[x] =
                    machine.display_control().apply_master_brightness(
                        engine_index, pixels[x]);
            }
        };

    /* Master brightness is a post-output stage shared by every display mode. */
    if (display_mode == 0u) {
        std::fill_n(rgb555, 256u, UINT16_C(0x7FFF));
        apply_master_brightness(rgb555);
        return ORACIS_HOST_RUNTIME_OK;
    }

    /* Engine A mode 2 bypasses BG/OBJ composition and reads one LCDC bank. */
    if (display_mode == 2u && engine_index == 0u) {
        const uint8_t bank_index = static_cast<uint8_t>(
            (dispcnt >> 18u) & UINT32_C(3));
        std::array<uint16_t, 256> next_line{};
        const uint32_t line_byte_offset = scanline * 256u * 2u;
        for (uint32_t x = 0u; x < 256u; ++x) {
            const auto pixel = machine.vram_control().read_lcdc_bank_halfword(
                bank_index, line_byte_offset + x * 2u);
            if (!pixel) {
                if (report != nullptr) {
                    report->status = static_cast<uint32_t>(
                        oracis::memory::Nds2dComposeStatus::GuestMemoryError);
                    report->memory_status = static_cast<uint32_t>(pixel.status);
                }
                return ORACIS_HOST_RUNTIME_VIDEO_ERROR;
            }
            next_line[x] = static_cast<uint16_t>(
                pixel.value & UINT32_C(0x7FFF));
        }
        apply_master_brightness(next_line.data());
        std::copy(next_line.begin(), next_line.end(), rgb555);
        return ORACIS_HOST_RUNTIME_OK;
    }

    /* Engine B supports only display modes 0 and 1. Mode 2/3 are reserved. */
    if (engine_index == 1u && display_mode > 1u) {
        if (report != nullptr) {
            report->status = static_cast<uint32_t>(
                oracis::memory::Nds2dComposeStatus::UnsupportedBackgroundMode);
        }
        return ORACIS_HOST_RUNTIME_VIDEO_ERROR;
    }

    if (engine_index == 0u && display_mode == 3u) {
        const auto status =
            machine.display_control().copy_main_memory_display_scanline(
                scanline, rgb555, pixel_capacity);
        if (status != oracis::memory::Status::Ok) {
            if (report != nullptr) {
                report->status = static_cast<uint32_t>(
                    oracis::memory::Nds2dComposeStatus::GuestMemoryError);
                report->memory_status = static_cast<uint32_t>(status);
            }
            return ORACIS_HOST_RUNTIME_VIDEO_ERROR;
        }
        apply_master_brightness(rgb555);
        return ORACIS_HOST_RUNTIME_OK;
    }

    const bool engine_a_3d_bg0 =
        engine_index == 0u && display_mode == 1u &&
        (dispcnt & UINT32_C(0x00000108)) == UINT32_C(0x00000108);
    if (engine_a_3d_bg0) {
        const auto raster_status = machine.prepare_3d_bg0_scanline(
            static_cast<uint16_t>(scanline));
        if (raster_status != oracis::memory::Nds3dRasterStatus::Ok) {
            if (report != nullptr) {
                report->three_d_raster_status = static_cast<uint32_t>(raster_status);
            }
            return ORACIS_HOST_RUNTIME_VIDEO_ERROR;
        }
    }

    std::array<oracis::memory::Nds2dCompositedPixel, 256> composed{};
    oracis::memory::Nds2dBgCompositor compositor(
        machine.guest_memory(), &machine.vram_control());
    const oracis::memory::Nds2dComposeResult result = engine_a_3d_bg0
        ? compositor.render_scanline(
            static_cast<unsigned>(engine_index), machine.display_control(),
            static_cast<uint16_t>(scanline), composed,
            machine.three_d_scanline_cache())
        : compositor.render_scanline(
            static_cast<unsigned>(engine_index), machine.display_control(),
            static_cast<uint16_t>(scanline), composed);

    if (report != nullptr) {
        report->status = static_cast<uint32_t>(result.status);
        report->background_index = result.background_index;
        report->fetch_status = static_cast<uint32_t>(result.fetch_status);
        report->memory_status = static_cast<uint32_t>(result.memory_status);
        report->object_fetch_status =
            static_cast<uint32_t>(result.object_fetch_status);
        report->object_index = result.object_index;
    }
    if (result.status ==
        oracis::memory::Nds2dComposeStatus::ThreeDSourceUnavailable) {
        machine.display_control().report_3d_color_buffer_underflow();
    }
    if (result.status != oracis::memory::Nds2dComposeStatus::Ok) {
        return ORACIS_HOST_RUNTIME_VIDEO_ERROR;
    }

    for (std::size_t x = 0u; x < composed.size(); ++x) {
        rgb555[x] = composed[x].rgb555;
    }
    apply_master_brightness(rgb555);
    return ORACIS_HOST_RUNTIME_OK;
}

extern "C" int oracis_host_runtime_validate_entrypoints(
    OracisHostRuntimeEntryPointReport *arm9_report,
    OracisHostRuntimeEntryPointReport *arm7_report)
{
    auto &machine = host_machine();
    if (!machine.initialized()) return ORACIS_HOST_RUNTIME_NOT_INITIALIZED;
    if (arm9_report == nullptr || arm7_report == nullptr) {
        return ORACIS_HOST_RUNTIME_INVALID_ARGUMENT;
    }
    if (!has_both_program_banks()) return ORACIS_HOST_RUNTIME_NO_PROGRAM_BANKS;
    if (!machine.program_loaded()) return ORACIS_HOST_RUNTIME_NO_PROGRAM_LOADED;

    const NdsCpu previous_cpu = g_nds_active;
    const auto inspect = [&machine](
        NdsCpu cpu, OracisHostRuntimeEntryPointReport &report) -> bool {
        report = {};
        if (!oracis_arm_runtime_select_cpu(cpu)) return false;

        const ArmCpuState *state = machine.cpu_state(cpu);
        if (state == nullptr) return false;

        report.pc = state->R[15];
        report.cpsr = state->cpsr;
        uint32_t lookup_status = UINT32_MAX;
        uint32_t candidate_count = 0u;
        const NdsDispatchEntry *inactive = nullptr;
        const NdsDispatchEntry *entry = oracis_static_dispatch_lookup(
            cpu, report.pc, report.cpsr,
            &lookup_status, &candidate_count, &inactive);
        report.lookup_status = lookup_status;
        report.candidate_count = candidate_count;
        report.has_inactive_candidate = inactive != nullptr ? 1u : 0u;
        report.resolved = entry != nullptr ? 1u : 0u;
        return true;
    };

    const bool arm9_inspected = inspect(NDS_ARM9, *arm9_report);
    const bool arm7_inspected = inspect(NDS_ARM7, *arm7_report);
    const bool cpu_restored = oracis_arm_runtime_select_cpu(previous_cpu);
    if (!arm9_inspected || !arm7_inspected || !cpu_restored) {
        return ORACIS_HOST_RUNTIME_MACHINE_ERROR;
    }
    return ORACIS_HOST_RUNTIME_OK;
}

extern "C" int oracis_host_runtime_load_overlay(
    const OracisHostOverlayImage *overlay)
{
    auto &machine = host_machine();
    if (!machine.initialized()) return ORACIS_HOST_RUNTIME_NOT_INITIALIZED;
    if (!machine.program_loaded()) return ORACIS_HOST_RUNTIME_NO_PROGRAM_LOADED;
    if (overlay == nullptr) return ORACIS_HOST_RUNTIME_INVALID_ARGUMENT;

    const oracis::machine::NdsOverlayImage image{
        static_cast<NdsCpu>(overlay->cpu),
        overlay->overlay_id,
        overlay->ram_address,
        overlay->ram_size,
        overlay->bss_size,
        overlay->static_init_start,
        overlay->static_init_end,
        overlay->contents,
        overlay->image_sha1
    };
    return host_overlay_status(machine.load_overlay(image));
}

extern "C" int oracis_host_runtime_unload_overlay(
    uint32_t cpu, uint32_t overlay_id,
    uint32_t no_live_guest_references)
{
    auto &machine = host_machine();
    if (!machine.initialized()) return ORACIS_HOST_RUNTIME_NOT_INITIALIZED;
    if (!machine.program_loaded()) return ORACIS_HOST_RUNTIME_NO_PROGRAM_LOADED;
    if (cpu > static_cast<uint32_t>(NDS_ARM7) ||
        no_live_guest_references > 1u) {
        return ORACIS_HOST_RUNTIME_INVALID_ARGUMENT;
    }
    return host_overlay_status(machine.unload_overlay(
        static_cast<NdsCpu>(cpu), overlay_id,
        no_live_guest_references != 0u));
}

extern "C" int oracis_host_runtime_register_compiled_banks(void)
{
    auto &machine = host_machine();
    if (!machine.initialized()) return ORACIS_HOST_RUNTIME_NOT_INITIALIZED;
#ifdef ORACIS_WITH_PLATINUM_STATIC_BANKS
    auto &registry = oracis::recomp::global_static_dispatch_registry();
    const bool arm9_registered = registry.contains_bank(
        NDS_ARM9,
        g_dispatch_pokemon_platinum_usa_rev1_arm9,
        g_dispatch_pokemon_platinum_usa_rev1_arm9_len);
    const bool arm7_registered = registry.contains_bank(
        NDS_ARM7,
        g_dispatch_pokemon_platinum_usa_rev1_arm7,
        g_dispatch_pokemon_platinum_usa_rev1_arm7_len);

    if (arm9_registered && arm7_registered) {
#ifdef ORACIS_WITH_PLATINUM_OVERLAY_BANKS
        const int overlay_status = oracis_register_compiled_overlay_banks();
        if (overlay_status != ORACIS_DISPATCH_REGISTER_OK) {
            return ORACIS_HOST_RUNTIME_BANK_REGISTRATION_FAILED;
        }
#endif
        return ORACIS_HOST_RUNTIME_BANKS_ALREADY_REGISTERED;
    }

    /*
     * The registry is public through the C ABI, so callers may clear or
     * unregister banks independently of this host bridge. Derive current
     * ownership from the registry instead of keeping process-local flags
     * that can become stale after those public mutations.
     */
    bool added_arm9 = false;
#ifdef ORACIS_WITH_PLATINUM_OVERLAY_BANKS
    bool added_arm7 = false;
#endif
    if (!arm9_registered) {
        const int arm9_status = oracis_static_dispatch_register_bank(
            NDS_ARM9,
            g_dispatch_pokemon_platinum_usa_rev1_arm9,
            g_dispatch_pokemon_platinum_usa_rev1_arm9_len,
            "pokemon-platinum-usa-rev1-arm9");
        if (arm9_status != ORACIS_DISPATCH_REGISTER_OK) {
            return ORACIS_HOST_RUNTIME_BANK_REGISTRATION_FAILED;
        }
        added_arm9 = true;
    }

    if (!arm7_registered) {
        const int arm7_status = oracis_static_dispatch_register_bank(
            NDS_ARM7,
            g_dispatch_pokemon_platinum_usa_rev1_arm7,
            g_dispatch_pokemon_platinum_usa_rev1_arm7_len,
            "pokemon-platinum-usa-rev1-arm7");
        if (arm7_status != ORACIS_DISPATCH_REGISTER_OK) {
            if (added_arm9) {
                (void)oracis_static_dispatch_unregister_bank(
                    NDS_ARM9,
                    g_dispatch_pokemon_platinum_usa_rev1_arm9,
                    g_dispatch_pokemon_platinum_usa_rev1_arm9_len);
            }
            return ORACIS_HOST_RUNTIME_BANK_REGISTRATION_FAILED;
        }
#ifdef ORACIS_WITH_PLATINUM_OVERLAY_BANKS
        added_arm7 = true;
#endif
    }
#ifdef ORACIS_WITH_PLATINUM_OVERLAY_BANKS
    const int overlay_status = oracis_register_compiled_overlay_banks();
    if (overlay_status != ORACIS_DISPATCH_REGISTER_OK) {
        if (added_arm7) {
            (void)oracis_static_dispatch_unregister_bank(
                NDS_ARM7,
                g_dispatch_pokemon_platinum_usa_rev1_arm7,
                g_dispatch_pokemon_platinum_usa_rev1_arm7_len);
        }
        if (added_arm9) {
            (void)oracis_static_dispatch_unregister_bank(
                NDS_ARM9,
                g_dispatch_pokemon_platinum_usa_rev1_arm9,
                g_dispatch_pokemon_platinum_usa_rev1_arm9_len);
        }
        return ORACIS_HOST_RUNTIME_BANK_REGISTRATION_FAILED;
    }
#endif
    return ORACIS_HOST_RUNTIME_OK;
#else
    return ORACIS_HOST_RUNTIME_NO_COMPILED_BANKS;
#endif
}

extern "C" int oracis_host_runtime_run_round(
    uint64_t bus_cycle_budget,
    OracisHostIrqContextProvider irq_context,
    void *context)
{
    auto &machine = host_machine();
    if (!machine.initialized()) return ORACIS_HOST_RUNTIME_NOT_INITIALIZED;
    if (!has_both_program_banks()) return ORACIS_HOST_RUNTIME_NO_PROGRAM_BANKS;
    if (!machine.program_loaded()) return ORACIS_HOST_RUNTIME_NO_PROGRAM_LOADED;

    /*
     * A caller must not be able to enter generated code with a stale image,
     * an unregistered start address or the wrong ARM/Thumb mode. Dispatch
     * itself remains guarded on every transfer, but that is too late for the
     * initial PC because a miss there terminates through the fail-closed
     * dispatcher. Preflight both CPUs before consuming any round budget.
     */
    OracisHostRuntimeEntryPointReport arm9_entry{};
    OracisHostRuntimeEntryPointReport arm7_entry{};
    const int entrypoint_status = oracis_host_runtime_validate_entrypoints(
        &arm9_entry, &arm7_entry);
    if (entrypoint_status != ORACIS_HOST_RUNTIME_OK) {
        return entrypoint_status;
    }
    if (arm9_entry.resolved == 0u || arm7_entry.resolved == 0u) {
        return ORACIS_HOST_RUNTIME_ENTRYPOINT_UNRESOLVED;
    }

    oracis::machine::NdsMachineStatus status;
    if (irq_context == nullptr) {
        status = machine.run_dual_core_round(
            bus_cycle_budget,
            &oracis::machine::NdsMachine::execute_registered_bank_slice,
            nullptr,
            nullptr);
    } else {
        IrqProviderContext provider_context{irq_context, context};
        status = machine.run_dual_core_round(
            bus_cycle_budget,
            &oracis::machine::NdsMachine::execute_registered_bank_slice,
            &invoke_irq_provider,
            &provider_context);
    }

    g_last_machine_status = static_cast<int>(status);
    return status == oracis::machine::NdsMachineStatus::Ok
        ? ORACIS_HOST_RUNTIME_OK
        : ORACIS_HOST_RUNTIME_MACHINE_ERROR;
}

extern "C" int oracis_host_runtime_is_initialized(void)
{
    return host_machine().initialized() ? 1 : 0;
}

extern "C" int oracis_host_runtime_has_program_banks(void)
{
    return has_both_program_banks() ? 1 : 0;
}

extern "C" uint64_t oracis_host_runtime_global_bus_cycles(void)
{
    return host_machine().global_bus_cycles();
}

extern "C" int oracis_host_runtime_last_machine_status(void)
{
    return g_last_machine_status;
}

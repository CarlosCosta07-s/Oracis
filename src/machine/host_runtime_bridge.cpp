#include "oracis/host/runtime_bridge.h"

#include "oracis/machine/nds_machine.hpp"
#include "oracis/recomp/static_dispatch.hpp"
#include "oracis/recomp/static_dispatch_abi.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <new>
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

namespace {

oracis::machine::NdsMachine &host_machine() noexcept
{
    static oracis::machine::NdsMachine machine(
        oracis::memory::SharedWramMode::Arm9All32K);
    return machine;
}

int g_last_machine_status =
    static_cast<int>(oracis::machine::NdsMachineStatus::Ok);

#ifdef ORACIS_WITH_PLATINUM_STATIC_BANKS
bool g_arm9_compiled_bank_registered = false;
bool g_arm7_compiled_bank_registered = false;

bool unregister_compiled_banks() noexcept
{
    bool success = true;
    if (g_arm7_compiled_bank_registered) {
        if (oracis_static_dispatch_unregister_bank(
                NDS_ARM7,
                g_dispatch_pokemon_platinum_usa_rev1_arm7,
                g_dispatch_pokemon_platinum_usa_rev1_arm7_len) != 0) {
            g_arm7_compiled_bank_registered = false;
        } else {
            success = false;
        }
    }
    if (g_arm9_compiled_bank_registered) {
        if (oracis_static_dispatch_unregister_bank(
                NDS_ARM9,
                g_dispatch_pokemon_platinum_usa_rev1_arm9,
                g_dispatch_pokemon_platinum_usa_rev1_arm9_len) != 0) {
            g_arm9_compiled_bank_registered = false;
        } else {
            success = false;
        }
    }
    return success;
}
#endif

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

uint32_t rotate_left(uint32_t value, unsigned amount) noexcept
{
    return (value << amount) | (value >> (32u - amount));
}

std::array<uint8_t, 20> sha1_digest(
    const std::vector<uint8_t> &input)
{
    const uint64_t bit_length = static_cast<uint64_t>(input.size()) * 8u;
    const std::size_t padded_size =
        ((input.size() + 9u + 63u) / 64u) * 64u;
    std::vector<uint8_t> padded(padded_size, 0u);
    std::copy(input.begin(), input.end(), padded.begin());
    padded[input.size()] = 0x80u;
    for (unsigned index = 0u; index < 8u; ++index) {
        padded[padded_size - 1u - index] =
            static_cast<uint8_t>(bit_length >> (index * 8u));
    }

    uint32_t h0 = 0x67452301u;
    uint32_t h1 = 0xEFCDAB89u;
    uint32_t h2 = 0x98BADCFEu;
    uint32_t h3 = 0x10325476u;
    uint32_t h4 = 0xC3D2E1F0u;

    for (std::size_t offset = 0u; offset < padded.size(); offset += 64u) {
        uint32_t words[80]{};
        for (unsigned index = 0u; index < 16u; ++index) {
            const std::size_t byte = offset + index * 4u;
            words[index] =
                (static_cast<uint32_t>(padded[byte]) << 24u) |
                (static_cast<uint32_t>(padded[byte + 1u]) << 16u) |
                (static_cast<uint32_t>(padded[byte + 2u]) << 8u) |
                static_cast<uint32_t>(padded[byte + 3u]);
        }
        for (unsigned index = 16u; index < 80u; ++index) {
            words[index] = rotate_left(
                words[index - 3u] ^ words[index - 8u] ^
                words[index - 14u] ^ words[index - 16u], 1u);
        }

        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t cword = h2;
        uint32_t d = h3;
        uint32_t e = h4;
        for (unsigned index = 0u; index < 80u; ++index) {
            uint32_t function = 0u;
            uint32_t constant = 0u;
            if (index < 20u) {
                function = (b & cword) | ((~b) & d);
                constant = 0x5A827999u;
            } else if (index < 40u) {
                function = b ^ cword ^ d;
                constant = 0x6ED9EBA1u;
            } else if (index < 60u) {
                function = (b & cword) | (b & d) | (cword & d);
                constant = 0x8F1BBCDCu;
            } else {
                function = b ^ cword ^ d;
                constant = 0xCA62C1D6u;
            }
            const uint32_t temporary =
                rotate_left(a, 5u) + function + e + constant + words[index];
            e = d;
            d = cword;
            cword = rotate_left(b, 30u);
            b = a;
            a = temporary;
        }

        h0 += a;
        h1 += b;
        h2 += cword;
        h3 += d;
        h4 += e;
    }

    const uint32_t state[5]{h0, h1, h2, h3, h4};
    std::array<uint8_t, 20> digest{};
    for (unsigned word = 0u; word < 5u; ++word) {
        for (unsigned byte = 0u; byte < 4u; ++byte) {
            digest[word * 4u + byte] =
                static_cast<uint8_t>(state[word] >> (24u - byte * 8u));
        }
    }
    return digest;
}

std::string sha1_hex(const std::vector<uint8_t> &bytes)
{
    static constexpr char kHex[] = "0123456789abcdef";
    const auto digest = sha1_digest(bytes);
    std::string result;
    result.reserve(40u);
    for (const uint8_t byte : digest) {
        result.push_back(kHex[byte >> 4u]);
        result.push_back(kHex[byte & 0x0fu]);
    }
    return result;
}

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

    if (sha1_hex(contents) != expected.sha1) {
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
    const auto status = machine.shutdown();
    g_last_machine_status = static_cast<int>(status);
    if (status != oracis::machine::NdsMachineStatus::Ok) {
        return ORACIS_HOST_RUNTIME_MACHINE_ERROR;
    }
#ifdef ORACIS_WITH_PLATINUM_STATIC_BANKS
    if (!unregister_compiled_banks()) {
        return ORACIS_HOST_RUNTIME_BANK_REGISTRATION_FAILED;
    }
#endif
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
        int status = read_verified_segment(
            root / "arm9.bin", kExpectedArm9, arm9_bytes);
        if (status != ORACIS_HOST_RUNTIME_OK) return status;
        status = read_verified_segment(
            root / "arm7.bin", kExpectedArm7, arm7_bytes);
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
        return oracis_host_runtime_load_program_segments(&arm9, &arm7);
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

extern "C" int oracis_host_runtime_register_compiled_banks(void)
{
    auto &machine = host_machine();
    if (!machine.initialized()) return ORACIS_HOST_RUNTIME_NOT_INITIALIZED;
#ifdef ORACIS_WITH_PLATINUM_STATIC_BANKS
    if (g_arm9_compiled_bank_registered ||
        g_arm7_compiled_bank_registered) {
        return ORACIS_HOST_RUNTIME_BANKS_ALREADY_REGISTERED;
    }

    const int arm9_status = oracis_static_dispatch_register_bank(
        NDS_ARM9,
        g_dispatch_pokemon_platinum_usa_rev1_arm9,
        g_dispatch_pokemon_platinum_usa_rev1_arm9_len,
        "pokemon-platinum-usa-rev1-arm9");
    if (arm9_status != ORACIS_DISPATCH_REGISTER_OK) {
        return ORACIS_HOST_RUNTIME_BANK_REGISTRATION_FAILED;
    }
    g_arm9_compiled_bank_registered = true;

    const int arm7_status = oracis_static_dispatch_register_bank(
        NDS_ARM7,
        g_dispatch_pokemon_platinum_usa_rev1_arm7,
        g_dispatch_pokemon_platinum_usa_rev1_arm7_len,
        "pokemon-platinum-usa-rev1-arm7");
    if (arm7_status != ORACIS_DISPATCH_REGISTER_OK) {
        (void)unregister_compiled_banks();
        return ORACIS_HOST_RUNTIME_BANK_REGISTRATION_FAILED;
    }
    g_arm7_compiled_bank_registered = true;
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

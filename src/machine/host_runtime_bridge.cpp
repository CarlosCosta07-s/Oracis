#include "oracis/host/runtime_bridge.h"
#include "oracis/host/detail/sha1.hpp"

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

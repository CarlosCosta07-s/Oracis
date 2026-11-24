#include "oracis/host/runtime_bridge.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "oracis_bios_bank_link_smoke: %s\n", message);
    return 1;
}

bool read_exact_file(
    const char *path, std::size_t expected_size, std::vector<uint8_t> &bytes)
{
    if (path == nullptr || *path == '\0') return false;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;

    bytes.resize(expected_size);
    stream.read(
        reinterpret_cast<char *>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (stream.gcount() != static_cast<std::streamsize>(bytes.size()) ||
        stream.bad()) {
        return false;
    }

    char extra = 0;
    stream.read(&extra, 1);
    return stream.gcount() == 0 && !stream.bad();
}

int shutdown_runtime()
{
    if (oracis_host_runtime_shutdown() != ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_has_program_banks() != 0) {
        return fail("shutdown did not clear the generated BIOS bank registrations");
    }
    return 0;
}

int link_only_smoke()
{
    if (oracis_host_runtime_initialize() != ORACIS_HOST_RUNTIME_OK) {
        return fail("machine initialization failed");
    }
    const int registration = oracis_host_runtime_register_user_bios_banks();
    if ((registration != ORACIS_HOST_RUNTIME_OK &&
         registration != ORACIS_HOST_RUNTIME_BANKS_ALREADY_REGISTERED) ||
        oracis_host_runtime_has_program_banks() != 1) {
        (void)oracis_host_runtime_shutdown();
        return fail("BIOS bank registration failed");
    }
    if (shutdown_runtime() != 0) return 1;
    std::puts("user BIOS C-bank link/register/unregister smoke passed");
    return 0;
}

int preflight_case(
    const std::vector<uint8_t> &arm9_bios,
    const std::vector<uint8_t> &arm7_bios,
    bool expect_resolved,
    const char *case_name)
{
    if (oracis_host_runtime_initialize() != ORACIS_HOST_RUNTIME_OK) {
        return fail("machine initialization failed");
    }
    if (oracis_host_runtime_load_arm9_bios(
            arm9_bios.data(), static_cast<uint32_t>(arm9_bios.size())) !=
            ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_load_game_card_arm7_bios(
            arm7_bios.data(), static_cast<uint32_t>(arm7_bios.size())) !=
            ORACIS_HOST_RUNTIME_OK) {
        (void)oracis_host_runtime_shutdown();
        return fail("synthetic BIOS images could not be loaded");
    }

    const int registration = oracis_host_runtime_register_user_bios_banks();
    if ((registration != ORACIS_HOST_RUNTIME_OK &&
         registration != ORACIS_HOST_RUNTIME_BANKS_ALREADY_REGISTERED) ||
        oracis_host_runtime_has_program_banks() != 1) {
        (void)oracis_host_runtime_shutdown();
        return fail("NDSRecomp-generated ARM9/ARM7 banks could not be registered");
    }

    OracisHostRuntimeEntryPointReport arm9_report = {};
    OracisHostRuntimeEntryPointReport arm7_report = {};
    const int boot_status = oracis_host_runtime_start_retail_bios_boot(
        &arm9_report, &arm7_report);

    if (expect_resolved) {
        if (boot_status != ORACIS_HOST_RUNTIME_OK ||
            arm9_report.resolved != 1u ||
            arm9_report.pc != UINT32_C(0xFFFF0000) ||
            arm7_report.resolved != 1u ||
            arm7_report.pc != UINT32_C(0x00000000)) {
            std::fprintf(
                stderr,
                "oracis_bios_bank_link_smoke: %s BIOS preflight failed "
                "(status=%d, arm9=%u/%u candidates=%u, arm7=%u/%u candidates=%u)\n",
                case_name, boot_status,
                arm9_report.resolved, arm9_report.has_inactive_candidate,
                arm9_report.candidate_count,
                arm7_report.resolved, arm7_report.has_inactive_candidate,
                arm7_report.candidate_count);
            (void)oracis_host_runtime_shutdown();
            return 1;
        }

        if (oracis_host_runtime_run_retail_bios_round(
                64u, nullptr, nullptr) != ORACIS_HOST_RUNTIME_OK) {
            (void)oracis_host_runtime_shutdown();
            return fail("ARM9 synthetic native reset stub did not execute");
        }

        ArmCpuState arm9_after_first_round = {};
        ArmCpuState arm7_while_held = {};
        if (oracis_host_runtime_get_cpu_state(
                NDS_ARM9, &arm9_after_first_round) != ORACIS_HOST_RUNTIME_OK ||
            oracis_host_runtime_get_cpu_state(
                NDS_ARM7, &arm7_while_held) != ORACIS_HOST_RUNTIME_OK ||
            arm9_after_first_round.R[0] != UINT32_C(0x2B) ||
            arm7_while_held.R[0] != 0u ||
            oracis_host_runtime_global_bus_cycles() < UINT64_C(64)) {
            (void)oracis_host_runtime_shutdown();
            return fail("ARM9 execution result or ARM7 reset isolation was incorrect");
        }

        if (oracis_host_runtime_release_arm7_from_reset() !=
                ORACIS_HOST_RUNTIME_OK ||
            oracis_host_runtime_release_arm7_from_reset() !=
                ORACIS_HOST_RUNTIME_CPU_NOT_IN_RESET) {
            (void)oracis_host_runtime_shutdown();
            return fail("synthetic ARM7 reset hand-off was not one-shot");
        }

        if (oracis_host_runtime_run_retail_bios_round(
                64u, nullptr, nullptr) != ORACIS_HOST_RUNTIME_OK) {
            (void)oracis_host_runtime_shutdown();
            return fail("released ARM7 synthetic native reset stub did not execute");
        }

        ArmCpuState arm9_after_second_round = {};
        ArmCpuState arm7_after_second_round = {};
        const int arm9_snapshot_status = oracis_host_runtime_get_cpu_state(
            NDS_ARM9, &arm9_after_second_round);
        const int arm7_snapshot_status = oracis_host_runtime_get_cpu_state(
            NDS_ARM7, &arm7_after_second_round);
        const uint64_t second_round_bus_cycles =
            oracis_host_runtime_global_bus_cycles();
        if (arm9_snapshot_status != ORACIS_HOST_RUNTIME_OK ||
            arm7_snapshot_status != ORACIS_HOST_RUNTIME_OK ||
            arm9_after_second_round.R[0] != UINT32_C(0x2B) ||
            arm7_after_second_round.R[0] != UINT32_C(0x2B) ||
            arm9_after_second_round.R[15] < UINT32_C(0xFFFF0000) ||
            arm9_after_second_round.R[15] >= UINT32_C(0xFFFF1000) ||
            arm7_after_second_round.R[15] >= UINT32_C(0x00004000) ||
            second_round_bus_cycles < UINT64_C(128)) {
            std::fprintf(
                stderr,
                "second-round snapshot: arm9_status=%d R0=%08X PC=%08X CPSR=%08X; "
                "arm7_status=%d R0=%08X PC=%08X CPSR=%08X; bus_cycles=%llu\n",
                arm9_snapshot_status,
                arm9_after_second_round.R[0],
                arm9_after_second_round.R[15],
                arm9_after_second_round.cpsr,
                arm7_snapshot_status,
                arm7_after_second_round.R[0],
                arm7_after_second_round.R[15],
                arm7_after_second_round.cpsr,
                static_cast<unsigned long long>(second_round_bus_cycles));
            (void)oracis_host_runtime_shutdown();
            return fail("ARM9/ARM7 native reset execution results were incorrect");
        }
    } else {
        if (boot_status != ORACIS_HOST_RUNTIME_ENTRYPOINT_UNRESOLVED ||
            arm9_report.resolved != 0u ||
            arm9_report.candidate_count == 0u ||
            arm9_report.has_inactive_candidate != 1u ||
            arm7_report.resolved != 1u ||
            oracis_host_runtime_run_retail_bios_round(8u, nullptr, nullptr) !=
                ORACIS_HOST_RUNTIME_BIOS_BOOT_NOT_ACTIVE ||
            oracis_host_runtime_global_bus_cycles() != 0u) {
            std::fprintf(
                stderr,
                "oracis_bios_bank_link_smoke: %s changed BIOS bytes were not "
                "rejected fail-closed (status=%d, arm9=%u/%u candidates=%u, "
                "arm7=%u/%u candidates=%u)\n",
                case_name, boot_status,
                arm9_report.resolved, arm9_report.has_inactive_candidate,
                arm9_report.candidate_count,
                arm7_report.resolved, arm7_report.has_inactive_candidate,
                arm7_report.candidate_count);
            (void)oracis_host_runtime_shutdown();
            return 1;
        }
    }

    return shutdown_runtime();
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 1) return link_only_smoke();
    if (argc != 3) {
        return fail("usage: oracis_bios_bank_link_smoke [ARM9_BIOS ARM7_BIOS]");
    }

    std::vector<uint8_t> arm9_bios;
    std::vector<uint8_t> arm7_bios;
    if (!read_exact_file(argv[1], 0x1000u, arm9_bios) ||
        !read_exact_file(argv[2], 0x4000u, arm7_bios)) {
        return fail("synthetic BIOS input sizes or file reads are invalid");
    }

    std::vector<uint8_t> changed_arm9_bios = arm9_bios;
    changed_arm9_bios[0] ^= 0x01u;
    if (preflight_case(
            changed_arm9_bios, arm7_bios, false, "mismatched") != 0) {
        return 1;
    }
    if (preflight_case(arm9_bios, arm7_bios, true, "matching") != 0) {
        return 1;
    }

    std::puts(
        "pinned NDSRecomp BIOS banks linked into Oracis; live-byte mismatch "
        "was rejected and both matching reset vectors preflighted");
    return 0;
}

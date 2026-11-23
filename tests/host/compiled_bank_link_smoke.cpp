#include "oracis/host/runtime_bridge.h"
#include "oracis/memory/guest_memory_abi.h"
#include "oracis/recomp/static_dispatch_abi.h"

#include <cstdio>
#include <cstdint>

namespace {

constexpr uint32_t kArm9Entry = UINT32_C(0x02001000);
constexpr uint32_t kArm7Entry = UINT32_C(0x02381000);
constexpr uint32_t kArm9MarkerAddress = UINT32_C(0x02002000);
constexpr uint32_t kArm7MarkerAddress = UINT32_C(0x02002004);
constexpr uint32_t kArm9LinkedCallCountAddress = UINT32_C(0x02002008);
constexpr uint32_t kArm9LinkStateAddress = UINT32_C(0x02002010);
constexpr uint32_t kArm9Marker = UINT32_C(0xA9A90001);
constexpr uint32_t kArm7Marker = UINT32_C(0xA7A70001);
constexpr uint32_t kArm9ExpectedLinkedCalls = 3u;
constexpr uint32_t kArm9LinkStateMarker = UINT32_C(0xA9A90003);

void registration_collision_entry(void)
{
}

int fail(const char *message)
{
    std::fprintf(stderr, "compiled_bank_link_smoke: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    if (oracis_host_runtime_initialize() != ORACIS_HOST_RUNTIME_OK) {
        return fail("host runtime initialization failed");
    }

    /*
     * Force ARM7 registration to fail after ARM9 succeeds. The host must roll
     * back the newly registered ARM9 table and leave the existing ARM7 bank
     * untouched; a retry must then register both compiled banks normally.
     */
    const NdsDispatchEntry arm7_name_collision[] = {
        {UINT32_C(0x02380000), 0u, &registration_collision_entry, nullptr}
    };
    if (oracis_static_dispatch_register_bank(
            NDS_ARM7, arm7_name_collision, 1u,
            "pokemon-platinum-usa-rev1-arm7") !=
            ORACIS_DISPATCH_REGISTER_OK) {
        (void)oracis_host_runtime_shutdown();
        return fail("could not install ARM7 bank-name collision fixture");
    }

    if (oracis_host_runtime_register_compiled_banks() !=
            ORACIS_HOST_RUNTIME_BANK_REGISTRATION_FAILED ||
        oracis_host_runtime_has_program_banks() != 0) {
        (void)oracis_host_runtime_shutdown();
        return fail("failed ARM7 registration did not roll back the ARM9 bank");
    }

    if (oracis_static_dispatch_unregister_bank(
            NDS_ARM7, arm7_name_collision, 1u) == 0 ||
        oracis_host_runtime_register_compiled_banks() !=
            ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_has_program_banks() != 1) {
        (void)oracis_host_runtime_shutdown();
        return fail("compiled ARM9/ARM7 registration did not recover after rollback");
    }

    if (oracis_host_runtime_register_compiled_banks() !=
            ORACIS_HOST_RUNTIME_BANKS_ALREADY_REGISTERED) {
        (void)oracis_host_runtime_shutdown();
        return fail("repeated compiled-bank registration was not identified");
    }

    /*
     * The public dispatch ABI may clear every bank after host registration.
     * The host must rediscover that state and register the compiled pair
     * again instead of trusting stale process-local registration flags.
     */
    oracis_static_dispatch_clear();
    if (oracis_host_runtime_has_program_banks() != 0 ||
        oracis_host_runtime_register_compiled_banks() !=
            ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_has_program_banks() != 1) {
        (void)oracis_host_runtime_shutdown();
        return fail("compiled-bank registration did not recover after external registry clear");
    }

    if (oracis_host_runtime_program_loaded() != 0 ||
        oracis_host_runtime_run_round(1u, nullptr, nullptr) !=
            ORACIS_HOST_RUNTIME_NO_PROGRAM_LOADED ||
        oracis_host_runtime_global_bus_cycles() != 0u) {
        (void)oracis_host_runtime_shutdown();
        return fail("host executed compiled banks before loading program images");
    }

    /* Load tiny synthetic images at the exact PCs exported by the linked banks. */
    const uint8_t arm9_bytes[] = {
        UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0xA0), UINT8_C(0xE1),
        UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0xA0), UINT8_C(0xE1)
    };
    const uint8_t arm7_bytes[] = {
        UINT8_C(0xC0), UINT8_C(0x46)
    };
    const OracisHostProgramSegment arm9_image = {
        kArm9Entry,
        static_cast<uint32_t>(sizeof(arm9_bytes)),
        kArm9Entry,
        0u,
        arm9_bytes
    };
    const OracisHostProgramSegment arm7_image = {
        kArm7Entry,
        static_cast<uint32_t>(sizeof(arm7_bytes)),
        kArm7Entry,
        1u,
        arm7_bytes
    };

    if (oracis_host_runtime_load_program_segments(
            &arm9_image, &arm7_image) != ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_program_loaded() != 1) {
        (void)oracis_host_runtime_shutdown();
        return fail("synthetic ARM9/ARM7 program images did not load");
    }

    OracisHostRuntimeEntryPointReport arm9_report{};
    OracisHostRuntimeEntryPointReport arm7_report{};
    if (oracis_host_runtime_validate_entrypoints(
            &arm9_report, &arm7_report) != ORACIS_HOST_RUNTIME_OK ||
        arm9_report.pc != kArm9Entry ||
        arm9_report.resolved != 1u ||
        arm9_report.lookup_status != ORACIS_DISPATCH_LOOKUP_FOUND ||
        arm9_report.candidate_count != 1u ||
        arm7_report.pc != kArm7Entry ||
        arm7_report.resolved != 1u ||
        arm7_report.lookup_status != ORACIS_DISPATCH_LOOKUP_FOUND ||
        arm7_report.candidate_count != 1u ||
        (arm7_report.cpsr & CPSR_T_BIT) == 0u) {
        (void)oracis_host_runtime_shutdown();
        return fail("compiled ARM9/ARM7 entry points did not resolve in their execution modes");
    }

    if (oracis_host_runtime_run_round(1u, nullptr, nullptr) !=
            ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_global_bus_cycles() != 1u) {
        (void)oracis_host_runtime_shutdown();
        return fail("registered static banks did not complete a synchronized dual-core round");
    }

    if (bus_read_u32(kArm9MarkerAddress) != kArm9Marker ||
        bus_read_u32(kArm7MarkerAddress) != kArm7Marker ||
        bus_read_u32(kArm9LinkedCallCountAddress) != kArm9ExpectedLinkedCalls ||
        bus_read_u32(kArm9LinkStateAddress) != kArm9LinkStateMarker) {
        (void)oracis_host_runtime_shutdown();
        return fail("validated ARM9 link slot did not hit, invalidate, and re-resolve");
    }

    if (oracis_host_runtime_shutdown() != ORACIS_HOST_RUNTIME_OK ||
        oracis_host_runtime_has_program_banks() != 0) {
        return fail("compiled bank tables were not unregistered at shutdown");
    }

    std::puts("optional compiled bank rollback/link/load/preflight/dual-core execution smoke test passed");
    return 0;
}

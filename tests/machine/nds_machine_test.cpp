#include "oracis/machine/nds_machine.hpp"

#include "oracis/memory/guest_memory_abi.h"
#include "oracis/memory/nds_display_control.hpp"
#include "oracis/recomp/runtime_arm.h"
#include "oracis/recomp/runtime_scheduler.h"
#include "oracis/recomp/static_dispatch_abi.h"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_machine_test: %s\n", message);

    return 1;
}

uint32_t g_static_dispatch_calls[2]{};

void static_arm9_test_bank()
{
    ++g_static_dispatch_calls[0];
    runtime_tick(1u);
}

void static_arm7_test_bank()
{
    ++g_static_dispatch_calls[1];
    runtime_tick(1u);
}

struct SchedulerFixture {
    uint32_t calls[2]{};
    uint64_t budgets[2]{};
    bool yield_early_arm9 = false;
    uint32_t irq_context_calls[2]{};
};

bool execute_test_slice(
    void *opaque, NdsCpu cpu, ArmCpuState *state, uint64_t cycle_budget)
{
    auto *fixture = static_cast<SchedulerFixture *>(opaque);
    if (fixture == nullptr || state == nullptr ||
        (cpu != NDS_ARM9 && cpu != NDS_ARM7) ||
        cycle_budget > UINT32_MAX) {
        return false;
    }

    const unsigned index = static_cast<unsigned>(cpu);
    ++fixture->calls[index];
    fixture->budgets[index] = cycle_budget;

    if (cpu == NDS_ARM9 && fixture->yield_early_arm9 &&
        cycle_budget > 1u) {
        runtime_tick(static_cast<uint32_t>(cycle_budget / 2u));
        return oracis_scheduler_request_yield(cpu) != 0 &&
               runtime_should_yield();
    }

    runtime_tick(static_cast<uint32_t>(cycle_budget));
    return runtime_should_yield();
}

bool provide_test_irq_context(
    void *opaque, NdsCpu cpu, const ArmCpuState *state,
    uint32_t *return_address, uint32_t *vector_base)
{
    auto *fixture = static_cast<SchedulerFixture *>(opaque);
    if (fixture == nullptr || state == nullptr ||
        return_address == nullptr || vector_base == nullptr ||
        (cpu != NDS_ARM9 && cpu != NDS_ARM7)) {
        return false;
    }

    ++fixture->irq_context_calls[static_cast<unsigned>(cpu)];
    *return_address = cpu == NDS_ARM7
        ? UINT32_C(0x02005002)
        : UINT32_C(0x02004006);
    *vector_base = 0u;
    return true;
}

} // namespace

int main()
{
    using oracis::machine::NdsMachine;
    using oracis::machine::NdsMachineStatus;
    using oracis::memory::NdsInterruptControl;
    using oracis::memory::NdsDisplayControl;
    using oracis::memory::NdsKeypadControl;
    using oracis::memory::NdsTimerControl;
    using oracis::memory::SharedWramMode;

    NdsMachine machine(SharedWramMode::Arm9All32K);
    if (machine.initialize() != NdsMachineStatus::Ok ||
        !machine.initialized()) {
        return fail("machine initialization and device binding failed");
    }

    if (oracis_arm_runtime_current_state() != machine.cpu_state(NDS_ARM9) ||
        g_nds_active != NDS_ARM9 ||
        machine.cpu_state(NDS_ARM9)->cpsr != ORACIS_ARM_MODE_SYSTEM ||
        machine.cpu_state(NDS_ARM7)->cpsr != ORACIS_ARM_MODE_SYSTEM ||
        machine.exception_vector_base(NDS_ARM9) != UINT32_C(0xFFFF0000) ||
        runtime_coproc_read(15u, 0u, 1u, 0u, 0u) !=
            oracis::memory::NdsArm9Tcm::ResetControl ||
        runtime_coproc_read(15u, 0u, 0u, 0u, 0u) != UINT32_C(0x41059461) ||
        runtime_coproc_read(15u, 0u, 0u, 0u, 2u) != UINT32_C(0x00140180) ||
        runtime_coproc_read(15u, 0u, 9u, 1u, 0u) !=
            oracis::memory::NdsArm9Tcm::DtcmResetRegionControl ||
        runtime_coproc_read(15u, 0u, 9u, 1u, 1u) !=
            oracis::memory::NdsArm9Tcm::ItcmResetRegionControl ||
        machine.exception_vector_base(NDS_ARM7) != 0u) {
        return fail("machine did not bind CPU states or apply NDS reset vector bases");
    }

    if (machine.guest_memory().region_count() == 0u ||
        machine.guest_memory().write(
            NDS_ARM9, UINT32_C(0x02000000), 4u,
            UINT32_C(0x12345678)) != oracis::memory::Status::Ok ||
        machine.guest_memory().read(
            NDS_ARM7, UINT32_C(0x02000000), 4u).value !=
            UINT32_C(0x12345678)) {
        return fail("retail main RAM was not installed as shared memory");
    }

    /*
     * BIOS CpuSet copies halfwords or words; CpuFastSet accepts any NDS
     * word-count without rounding to the GBA's eight-word boundary.
     */
    constexpr uint32_t cpu_set_source = UINT32_C(0x02001000);
    constexpr uint32_t cpu_set_destination = UINT32_C(0x02001100);
    const uint32_t cpu_set_words[3] = {
        UINT32_C(0x11223344),
        UINT32_C(0xAABBCCDD),
        UINT32_C(0x0BADF00D)
    };
    for (uint32_t index = 0u; index < 3u; ++index) {
        if (machine.guest_memory().write(
                NDS_ARM9, cpu_set_source + index * 4u, 4u,
                cpu_set_words[index]) != oracis::memory::Status::Ok) {
            return fail("could not seed CpuSet word-copy source");
        }
    }
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 selection failed for CpuSet test");
    }
    ArmCpuState *arm9_state = machine.cpu_state(NDS_ARM9);
    ArmCpuState *arm7_state = machine.cpu_state(NDS_ARM7);
    arm9_state->R[0] = cpu_set_source;
    arm9_state->R[1] = cpu_set_destination;
    arm9_state->R[2] = (UINT32_C(1) << 26u) | 3u;
    runtime_swi(0x0Bu);
    for (uint32_t index = 0u; index < 3u; ++index) {
        if (machine.guest_memory().read(
                NDS_ARM9, cpu_set_destination + index * 4u, 4u).value !=
            cpu_set_words[index]) {
            return fail("CpuSet 32-bit word copy produced incorrect data");
        }
    }

    constexpr uint32_t cpu_set_fill_source = UINT32_C(0x02001020);
    constexpr uint32_t cpu_set_fill_destination = UINT32_C(0x02001120);
    if (machine.guest_memory().write(
            NDS_ARM9, cpu_set_fill_source, 2u, UINT16_C(0xA55A)) !=
        oracis::memory::Status::Ok) {
        return fail("could not seed CpuSet halfword-fill source");
    }
    arm9_state->R[0] = cpu_set_fill_source;
    arm9_state->R[1] = cpu_set_fill_destination;
    arm9_state->R[2] = (UINT32_C(1) << 24u) | 3u;
    runtime_swi(0x0Bu);
    for (uint32_t index = 0u; index < 3u; ++index) {
        if (machine.guest_memory().read(
                NDS_ARM9, cpu_set_fill_destination + index * 2u, 2u).value !=
            UINT16_C(0xA55A)) {
            return fail("CpuSet fixed-source halfword fill failed");
        }
    }

    constexpr uint32_t cpu_fast_source = UINT32_C(0x02001200);
    constexpr uint32_t cpu_fast_destination = UINT32_C(0x02001300);
    for (uint32_t index = 0u; index < 9u; ++index) {
        const uint32_t value = UINT32_C(0xC0010000) + index;
        if (machine.guest_memory().write(
                NDS_ARM9, cpu_fast_source + index * 4u, 4u, value) !=
            oracis::memory::Status::Ok) {
            return fail("could not seed CpuFastSet source");
        }
    }
    arm9_state->R[0] = cpu_fast_source;
    arm9_state->R[1] = cpu_fast_destination;
    arm9_state->R[2] = 9u;
    runtime_swi(0x0Cu);
    for (uint32_t index = 0u; index < 9u; ++index) {
        const uint32_t expected = UINT32_C(0xC0010000) + index;
        if (machine.guest_memory().read(
                NDS_ARM9, cpu_fast_destination + index * 4u, 4u).value !=
            expected) {
            return fail("CpuFastSet did not copy a non-eight-word NDS length");
        }
    }

    constexpr uint32_t cpu_fast_fill_destination =
        UINT32_C(0x02001400);
    arm9_state->R[0] = cpu_set_source;
    arm9_state->R[1] = cpu_fast_fill_destination;
    arm9_state->R[2] = (UINT32_C(1) << 24u) | 2u;
    runtime_swi(0x0Cu);
    if (machine.guest_memory().read(
            NDS_ARM9, cpu_fast_fill_destination, 4u).value !=
            cpu_set_words[0] ||
        machine.guest_memory().read(
            NDS_ARM9, cpu_fast_fill_destination + 4u, 4u).value !=
            cpu_set_words[0]) {
        return fail("CpuFastSet fixed-source word fill failed");
    }

    /*
     * ARM7 silently ignores a BIOS-sourced CpuSet. Invalid spans are rejected
     * by preflight before the destination can be modified.
     */
    constexpr uint32_t arm7_bios_guard_destination =
        UINT32_C(0x02001500);
    if (machine.guest_memory().write(
            NDS_ARM7, arm7_bios_guard_destination, 2u,
            UINT16_C(0x5AA5)) != oracis::memory::Status::Ok ||
        !oracis_arm_runtime_select_cpu(NDS_ARM7)) {
        return fail("could not prepare ARM7 BIOS-source guard test");
    }
    arm7_state->R[0] = 0u;
    arm7_state->R[1] = arm7_bios_guard_destination;
    arm7_state->R[2] = 1u;
    runtime_swi(0x0Bu);
    if (machine.guest_memory().read(
            NDS_ARM7, arm7_bios_guard_destination, 2u).value !=
        UINT16_C(0x5AA5)) {
        return fail("ARM7 CpuSet did not ignore BIOS-protected source");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9) ||
        oracis_arm_runtime_bios_memory_transfer(
            cpu_set_source, UINT32_C(0xFFFFFFFC), 2u, 4u, 0u) != 0 ||
        oracis_arm_runtime_bios_memory_transfer(
            cpu_set_source + 1u, cpu_set_destination, 1u, 4u, 0u) != 0) {
        return fail("BIOS memory transfer accepted overflow or misalignment");
    }
    if (machine.guest_memory().read(
            NDS_ARM9, cpu_set_destination, 4u).value !=
        cpu_set_words[0]) {
        return fail("rejected BIOS memory transfer modified its destination");
    }

    /*
     * GetCRC16 uses R0=initial CRC, R1=even-aligned source,
     * R2=even byte length, returns CRC in R0 and final halfword in R3.
     */
    constexpr uint32_t crc_source = UINT32_C(0x02001600);
    const uint8_t crc_data[] = {1u, 2u, 3u, 4u, 5u, 6u};
    if (machine.guest_memory().write_bytes(
            NDS_ARM9, crc_source, crc_data,
            static_cast<uint32_t>(sizeof(crc_data))) !=
        oracis::memory::Status::Ok) {
        return fail("could not seed GetCRC16 source data");
    }

    const NdsCpu crc_cpus[2] = {NDS_ARM9, NDS_ARM7};
    for (unsigned cpu_index = 0u; cpu_index < 2u; ++cpu_index) {
        const NdsCpu cpu = crc_cpus[cpu_index];
        if (!oracis_arm_runtime_select_cpu(cpu)) {
            return fail("CPU selection failed for GetCRC16");
        }
        ArmCpuState *state = machine.cpu_state(cpu);
        state->cpsr = ORACIS_ARM_MODE_SYSTEM | CPSR_N_BIT | CPSR_C_BIT;
        const uint32_t saved_cpsr = state->cpsr;
        state->R[0] = UINT32_C(0xFFFF);
        state->R[1] = crc_source;
        state->R[2] = static_cast<uint32_t>(sizeof(crc_data));
        state->R[3] = UINT32_C(0xDEADBEEF);
        runtime_swi(0x0Eu);
        if (state->R[0] != UINT32_C(0xDDBA) ||
            state->R[1] != crc_source ||
            state->R[2] != sizeof(crc_data) ||
            state->R[3] != UINT32_C(0x0605) ||
            state->cpsr != saved_cpsr) {
            return fail("GetCRC16 CRC result or final-halfword register was incorrect");
        }

        /* A caller-supplied seed must be honored, not replaced with FFFFh. */
        state->R[0] = UINT32_C(0x1234);
        state->R[1] = crc_source;
        state->R[2] = 4u;
        state->R[3] = UINT32_C(0xA5A5A5A5);
        runtime_swi(0x0Eu);
        if (state->R[0] != UINT32_C(0x3A0F) ||
            state->R[3] != UINT32_C(0x0403)) {
            return fail("GetCRC16 did not honor the initial CRC seed");
        }

        /* Zero bytes return the seed and leave R3 untouched. */
        state->R[0] = UINT32_C(0xABCD1234);
        state->R[1] = crc_source;
        state->R[2] = 0u;
        state->R[3] = UINT32_C(0x76543210);
        runtime_swi(0x0Eu);
        if (state->R[0] != UINT32_C(0x1234) ||
            state->R[3] != UINT32_C(0x76543210)) {
            return fail("zero-length GetCRC16 changed the wrong registers");
        }
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 selection failed for GetCRC16 span validation");
    }
    uint32_t crc_result = 0u;
    uint32_t crc_last_halfword = 0u;
    if (oracis_arm_runtime_bios_crc16(
            UINT32_C(0xFFFF), crc_source + 1u, 2u,
            &crc_result, &crc_last_halfword) != 0 ||
        oracis_arm_runtime_bios_crc16(
            UINT32_C(0xFFFF), crc_source, 3u,
            &crc_result, &crc_last_halfword) != 0 ||
        oracis_arm_runtime_bios_crc16(
            UINT32_C(0xFFFF), UINT32_C(0xFFFFFFFE), 4u,
            &crc_result, &crc_last_halfword) != 0 ||
        oracis_arm_runtime_bios_crc16(
            UINT32_C(0xFFFF), 0u, 2u,
            &crc_result, &crc_last_halfword) != 0) {
        return fail("GetCRC16 accepted misaligned, odd, overflowing or unmapped reads");
    }

    /*
     * IsDebugger (SWI 0x0F) reports the modeled main-RAM profile:
     * 0 means retail DS (4MB); 1 would mean debugger hardware (8MB).
     * The debugger memory profile is not currently supported by Oracis.
     */
    const NdsCpu debugger_query_cpus[2] = {NDS_ARM9, NDS_ARM7};
    for (unsigned index = 0u; index < 2u; ++index) {
        const NdsCpu cpu = debugger_query_cpus[index];
        if (!oracis_arm_runtime_select_cpu(cpu)) {
            return fail("CPU selection failed for IsDebugger");
        }
        ArmCpuState *state = machine.cpu_state(cpu);
        state->R[0] = UINT32_C(0xFFFFFFFF);
        state->R[1] = UINT32_C(0x11223344);
        state->R[2] = UINT32_C(0x55667788);
        state->R[3] = UINT32_C(0x99AABBCC);
        const uint32_t saved_cpsr = state->cpsr;
        runtime_swi(0x0Fu);
        if (state->R[0] != 0u ||
            state->R[1] != UINT32_C(0x11223344) ||
            state->R[2] != UINT32_C(0x55667788) ||
            state->R[3] != UINT32_C(0x99AABBCC) ||
            state->cpsr != saved_cpsr) {
            return fail("IsDebugger did not report the retail 4MB profile or corrupted registers");
        }
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 selection failed after IsDebugger tests");
    }

    /*
     * NDS BIOS BitUnPack expands packed source units into 32-bit output
     * words. It applies dataOffset only to nonzero values unless bit 31
     * requests that zero values are offset too.
     */
    constexpr uint32_t bit_unpack_source = UINT32_C(0x02001720);
    constexpr uint32_t bit_unpack_destination = UINT32_C(0x02001740);
    constexpr uint32_t bit_unpack_parameters = UINT32_C(0x02001700);
    const uint8_t bit_unpack_source_data[] = {UINT8_C(0xB2)};
    const uint8_t bit_unpack_info[] = {
        1u, 0u, 1u, 4u, 3u, 0u, 0u, 0u
    }; /* sourceSize=1, sourceWidth=1, destWidth=4, offset=3 */
    if (machine.guest_memory().write_bytes(
            NDS_ARM9, bit_unpack_source, bit_unpack_source_data, 1u) !=
            oracis::memory::Status::Ok ||
        machine.guest_memory().write_bytes(
            NDS_ARM9, bit_unpack_parameters, bit_unpack_info, 8u) !=
            oracis::memory::Status::Ok) {
        return fail("could not seed BitUnPack source and parameters");
    }

    const NdsCpu bit_unpack_cpus[2] = {NDS_ARM9, NDS_ARM7};
    for (unsigned index = 0u; index < 2u; ++index) {
        const NdsCpu cpu = bit_unpack_cpus[index];
        if (!oracis_arm_runtime_select_cpu(cpu)) {
            return fail("CPU selection failed for BitUnPack");
        }
        ArmCpuState *state = machine.cpu_state(cpu);
        state->R[0] = bit_unpack_source;
        state->R[1] = bit_unpack_destination;
        state->R[2] = bit_unpack_parameters;
        runtime_swi(0x10u);
        if (machine.guest_memory().read(
                cpu, bit_unpack_destination, 4u).value !=
            UINT32_C(0x40440040)) {
            return fail("BitUnPack 1-to-4 expansion or nonzero offset failed");
        }
    }

    /* The high dataOffset bit requests offsetting zero-valued source units. */
    constexpr uint32_t bit_unpack_zero_source = UINT32_C(0x02001780);
    constexpr uint32_t bit_unpack_zero_destination = UINT32_C(0x020017A0);
    constexpr uint32_t bit_unpack_zero_parameters = UINT32_C(0x02001760);
    const uint8_t bit_unpack_zeros[4] = {0u, 0u, 0u, 0u};
    const uint8_t bit_unpack_zero_info[8] = {
        4u, 0u, 2u, 8u, 0x20u, 0u, 0u, 0x80u
    }; /* 2-to-8 expansion, offset=0x20, including zeros */
    if (machine.guest_memory().write_bytes(
            NDS_ARM9, bit_unpack_zero_source,
            bit_unpack_zeros, 4u) != oracis::memory::Status::Ok ||
        machine.guest_memory().write_bytes(
            NDS_ARM9, bit_unpack_zero_parameters,
            bit_unpack_zero_info, 8u) != oracis::memory::Status::Ok) {
        return fail("could not seed zero-offset BitUnPack case");
    }
    for (unsigned index = 0u; index < 2u; ++index) {
        const NdsCpu cpu = bit_unpack_cpus[index];
        if (!oracis_arm_runtime_select_cpu(cpu)) {
            return fail("CPU selection failed for zero-offset BitUnPack");
        }
        ArmCpuState *state = machine.cpu_state(cpu);
        state->R[0] = bit_unpack_zero_source;
        state->R[1] = bit_unpack_zero_destination;
        state->R[2] = bit_unpack_zero_parameters;
        runtime_swi(0x10u);
        for (uint32_t word = 0u; word < 4u; ++word) {
            if (machine.guest_memory().read(
                    cpu, bit_unpack_zero_destination + word * 4u, 4u).value !=
                UINT32_C(0x20202020)) {
                return fail("BitUnPack zero-value offset flag was ignored");
            }
        }
    }

    /*
     * Source and destination may overlap: the BIOS expansion must consume
     * the complete packed input before writing its larger output range.
     */
    constexpr uint32_t bit_unpack_overlap = UINT32_C(0x02001900);
    constexpr uint32_t bit_unpack_overlap_info = UINT32_C(0x020018E0);
    const uint8_t bit_unpack_overlap_data[] = {
        UINT8_C(0xE4), UINT8_C(0x1B), UINT8_C(0x55), UINT8_C(0xAA)
    };
    const uint8_t bit_unpack_overlap_params[] = {
        4u, 0u, 2u, 8u, 0u, 0u, 0u, 0u
    };
    const uint8_t bit_unpack_overlap_expected[16] = {
        0u, 1u, 2u, 3u, 3u, 2u, 1u, 0u,
        1u, 1u, 1u, 1u, 2u, 2u, 2u, 2u
    };
    if (machine.guest_memory().write_bytes(
            NDS_ARM9, bit_unpack_overlap,
            bit_unpack_overlap_data, 4u) != oracis::memory::Status::Ok ||
        machine.guest_memory().write_bytes(
            NDS_ARM9, bit_unpack_overlap_info,
            bit_unpack_overlap_params, 8u) != oracis::memory::Status::Ok ||
        !oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("could not prepare overlapping BitUnPack test");
    }
    arm9_state->R[0] = bit_unpack_overlap;
    arm9_state->R[1] = bit_unpack_overlap;
    arm9_state->R[2] = bit_unpack_overlap_info;
    runtime_swi(0x10u);
    for (uint32_t byte = 0u; byte < 16u; ++byte) {
        if (machine.guest_memory().read(
                NDS_ARM9, bit_unpack_overlap + byte, 1u).value !=
            bit_unpack_overlap_expected[byte]) {
            return fail("BitUnPack corrupted overlapping source data");
        }
    }

    /* Malformed widths, output-size and value overflow fail before writes. */
    constexpr uint32_t bit_unpack_guard_source = UINT32_C(0x02001A00);
    constexpr uint32_t bit_unpack_guard_destination = UINT32_C(0x02001A20);
    constexpr uint32_t bit_unpack_guard_parameters = UINT32_C(0x02001A40);
    const uint8_t bit_unpack_guard_byte[] = {1u};
    const uint8_t bit_unpack_bad_size[] = {
        1u, 0u, 8u, 8u, 0u, 0u, 0u, 0u
    }; /* Output would be one byte, not a whole word. */
    const uint8_t bit_unpack_bad_value[] = {
        1u, 0u, 1u, 4u, 15u, 0u, 0u, 0u
    }; /* A set source bit plus offset 15 exceeds four bits. */
    if (machine.guest_memory().write_bytes(
            NDS_ARM9, bit_unpack_guard_source, bit_unpack_guard_byte, 1u) !=
            oracis::memory::Status::Ok ||
        machine.guest_memory().write_bytes(
            NDS_ARM9, bit_unpack_guard_parameters, bit_unpack_bad_size, 8u) !=
            oracis::memory::Status::Ok ||
        machine.guest_memory().write(
            NDS_ARM9, bit_unpack_guard_destination, 4u,
            UINT32_C(0xA5A55A5A)) != oracis::memory::Status::Ok ||
        !oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("could not prepare BitUnPack malformed-input tests");
    }
    if (oracis_arm_runtime_bios_bit_unpack(
            bit_unpack_guard_source, bit_unpack_guard_destination,
            bit_unpack_guard_parameters) != 0 ||
        machine.guest_memory().read(
            NDS_ARM9, bit_unpack_guard_destination, 4u).value !=
            UINT32_C(0xA5A55A5A)) {
        return fail("BitUnPack accepted output that was not word-sized");
    }
    if (machine.guest_memory().write_bytes(
            NDS_ARM9, bit_unpack_guard_parameters, bit_unpack_bad_value, 8u) !=
        oracis::memory::Status::Ok ||
        oracis_arm_runtime_bios_bit_unpack(
            bit_unpack_guard_source, bit_unpack_guard_destination,
            bit_unpack_guard_parameters) != 0 ||
        machine.guest_memory().read(
            NDS_ARM9, bit_unpack_guard_destination, 4u).value !=
            UINT32_C(0xA5A55A5A) ||
        oracis_arm_runtime_bios_bit_unpack(
            bit_unpack_guard_source, bit_unpack_guard_destination + 1u,
            bit_unpack_guard_parameters) != 0 ||
        oracis_arm_runtime_bios_bit_unpack(
            UINT32_C(0xFFFFFFFE), bit_unpack_guard_destination,
            bit_unpack_guard_parameters) != 0 ||
        oracis_arm_runtime_bios_bit_unpack(
            bit_unpack_guard_source, bit_unpack_guard_destination,
            UINT32_C(0xFFFFFFFC)) != 0) {
        return fail("BitUnPack accepted overflow, misalignment or invalid values");
    }

    /*
     * NDS BIOS LZ77UnCompWram (0x11) expands LZ10 byte streams. The stream
     * writes "ABC" literally, then repeats the prior three-byte window.
     */
    constexpr uint32_t lz77_wram_source = UINT32_C(0x02001B00);
    constexpr uint32_t lz77_wram_destination = UINT32_C(0x02001B21);
    const uint8_t lz77_wram_stream[] = {
        0x10u, 9u, 0u, 0u, 0x10u, 'A', 'B', 'C', 0x30u, 0x02u, 0u, 0u
    };
    const uint8_t lz77_wram_expected[] = {
        'A', 'B', 'C', 'A', 'B', 'C', 'A', 'B', 'C'
    };
    if (machine.guest_memory().write_bytes(
            NDS_ARM9, lz77_wram_source, lz77_wram_stream,
            static_cast<uint32_t>(sizeof(lz77_wram_stream))) !=
        oracis::memory::Status::Ok) {
        return fail("could not seed LZ77 WRAM stream");
    }

    const NdsCpu lz77_cpus[2] = {NDS_ARM9, NDS_ARM7};
    for (unsigned index = 0u; index < 2u; ++index) {
        const NdsCpu cpu = lz77_cpus[index];
        if (!oracis_arm_runtime_select_cpu(cpu)) {
            return fail("CPU selection failed for LZ77 WRAM");
        }
        ArmCpuState *state = machine.cpu_state(cpu);
        state->R[0] = lz77_wram_source;
        state->R[1] = lz77_wram_destination + index * 0x20u;
        const uint32_t saved_r2 = state->R[2];
        runtime_swi(0x11u);
        if (state->R[0] != lz77_wram_source ||
            state->R[1] != lz77_wram_destination + index * 0x20u ||
            state->R[2] != saved_r2) {
            return fail("LZ77 WRAM changed input registers");
        }
        for (uint32_t byte = 0u; byte < sizeof(lz77_wram_expected); ++byte) {
            if (machine.guest_memory().read(
                    cpu, lz77_wram_destination + index * 0x20u + byte,
                    1u).value != lz77_wram_expected[byte]) {
                return fail("LZ77 WRAM literal/back-reference expansion failed");
            }
        }
    }

    /*
     * LZ77UnCompVram (0x12) emits halfwords. This even-sized output uses
     * five literals followed by a five-byte overlapping back-reference.
     */
    constexpr uint32_t lz77_vram_source = UINT32_C(0x02001B40);
    constexpr uint32_t lz77_vram_destination = UINT32_C(0x02001B62);
    const uint8_t lz77_vram_stream[] = {
        0x10u, 10u, 0u, 0u, 0x04u, 'H', 'E', 'L', 'L', 'O', 0x20u, 0x04u
    };
    const uint8_t lz77_vram_expected[] = {
        'H', 'E', 'L', 'L', 'O', 'H', 'E', 'L', 'L', 'O'
    };
    if (machine.guest_memory().write_bytes(
            NDS_ARM9, lz77_vram_source, lz77_vram_stream,
            static_cast<uint32_t>(sizeof(lz77_vram_stream))) !=
        oracis::memory::Status::Ok) {
        return fail("could not seed LZ77 VRAM stream");
    }
    for (unsigned index = 0u; index < 2u; ++index) {
        const NdsCpu cpu = lz77_cpus[index];
        if (!oracis_arm_runtime_select_cpu(cpu)) {
            return fail("CPU selection failed for LZ77 VRAM");
        }
        ArmCpuState *state = machine.cpu_state(cpu);
        state->R[0] = lz77_vram_source;
        state->R[1] = lz77_vram_destination + index * 0x20u;
        runtime_swi(0x12u);
        for (uint32_t byte = 0u; byte < sizeof(lz77_vram_expected); ++byte) {
            if (machine.guest_memory().read(
                    cpu, lz77_vram_destination + index * 0x20u + byte,
                    1u).value != lz77_vram_expected[byte]) {
                return fail("LZ77 VRAM halfword output differed from expected bytes");
            }
        }
    }

    /* In-place decompression is safe because the complete output is staged. */
    constexpr uint32_t lz77_overlap = UINT32_C(0x02001B80);
    if (machine.guest_memory().write_bytes(
            NDS_ARM9, lz77_overlap, lz77_wram_stream,
            static_cast<uint32_t>(sizeof(lz77_wram_stream))) !=
            oracis::memory::Status::Ok ||
        !oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("could not prepare overlapping LZ77 input");
    }
    arm9_state->R[0] = lz77_overlap;
    arm9_state->R[1] = lz77_overlap;
    runtime_swi(0x11u);
    for (uint32_t byte = 0u; byte < sizeof(lz77_wram_expected); ++byte) {
        if (machine.guest_memory().read(
                NDS_ARM9, lz77_overlap + byte, 1u).value !=
            lz77_wram_expected[byte]) {
            return fail("overlapping LZ77 source was corrupted during decoding");
        }
    }

    /* Invalid type, bad history references, alignment and odd VRAM sizes fail. */
    constexpr uint32_t lz77_guard_source = UINT32_C(0x02001BC0);
    constexpr uint32_t lz77_guard_destination = UINT32_C(0x02001BE0);
    const uint8_t lz77_bad_type[] = {
        0x11u, 4u, 0u, 0u, 0u, 0u, 0u, 0u
    };
    const uint8_t lz77_bad_history[] = {
        0x10u, 3u, 0u, 0u, 0x80u, 0x00u, 0x00u
    };
    const uint8_t lz77_odd_size[] = {
        0x10u, 3u, 0u, 0u, 0x00u, 'X', 'Y', 'Z'
    };
    const uint32_t lz77_guard_value = UINT32_C(0xA55A3CC3);
    if (machine.guest_memory().write(
            NDS_ARM9, lz77_guard_destination, 4u,
            lz77_guard_value) != oracis::memory::Status::Ok ||
        machine.guest_memory().write_bytes(
            NDS_ARM9, lz77_guard_source, lz77_bad_type,
            static_cast<uint32_t>(sizeof(lz77_bad_type))) !=
            oracis::memory::Status::Ok ||
        !oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("could not prepare invalid LZ77 stream tests");
    }
    if (oracis_arm_runtime_bios_lz77(
            lz77_guard_source, lz77_guard_destination, 0u) != 0 ||
        machine.guest_memory().read(
            NDS_ARM9, lz77_guard_destination, 4u).value != lz77_guard_value ||
        machine.guest_memory().write_bytes(
            NDS_ARM9, lz77_guard_source, lz77_bad_history,
            static_cast<uint32_t>(sizeof(lz77_bad_history))) !=
            oracis::memory::Status::Ok ||
        oracis_arm_runtime_bios_lz77(
            lz77_guard_source, lz77_guard_destination, 0u) != 0 ||
        machine.guest_memory().read(
            NDS_ARM9, lz77_guard_destination, 4u).value != lz77_guard_value ||
        machine.guest_memory().write_bytes(
            NDS_ARM9, lz77_guard_source, lz77_odd_size,
            static_cast<uint32_t>(sizeof(lz77_odd_size))) !=
            oracis::memory::Status::Ok ||
        oracis_arm_runtime_bios_lz77(
            lz77_guard_source, lz77_guard_destination, 1u) != 0 ||
        oracis_arm_runtime_bios_lz77(
            lz77_guard_source + 1u, lz77_guard_destination, 0u) != 0 ||
        oracis_arm_runtime_bios_lz77(
            lz77_guard_source, lz77_guard_destination + 1u, 1u) != 0) {
        return fail("LZ77 accepted invalid type/history/alignment/VRAM size");
    }
    if (machine.guest_memory().read(
            NDS_ARM9, lz77_guard_destination, 4u).value !=
        lz77_guard_value) {
        return fail("rejected LZ77 stream modified the destination");
    }

    /*
     * NDS BIOS RLUnCompWram/Vram (0x14/0x15) supports literal and repeated
     * runs. This fixture expands 3 repeated bytes, 4 literals, then 3 more.
     */
    constexpr uint32_t rle_source = UINT32_C(0x02001C00);
    constexpr uint32_t rle_wram_destination = UINT32_C(0x02001C21);
    constexpr uint32_t rle_vram_destination = UINT32_C(0x02001C42);
    const uint8_t rle_stream[] = {
        0x30u, 10u, 0u, 0u,
        0x80u, 'A',
        0x03u, 'B', 'C', 'D', 'E',
        0x80u, 'Z',
        0u, 0u, 0u
    };
    const uint8_t rle_expected[] = {
        'A', 'A', 'A', 'B', 'C', 'D', 'E', 'Z', 'Z', 'Z'
    };
    if (machine.guest_memory().write_bytes(
            NDS_ARM9, rle_source, rle_stream,
            static_cast<uint32_t>(sizeof(rle_stream))) !=
        oracis::memory::Status::Ok) {
        return fail("could not seed BIOS RLE stream");
    }

    for (unsigned index = 0u; index < 2u; ++index) {
        const NdsCpu cpu = lz77_cpus[index];
        if (!oracis_arm_runtime_select_cpu(cpu)) {
            return fail("CPU selection failed for RLUnCompWram");
        }
        ArmCpuState *state = machine.cpu_state(cpu);
        state->R[0] = rle_source;
        state->R[1] = rle_wram_destination + index * 0x20u;
        const uint32_t saved_r2 = state->R[2];
        runtime_swi(0x14u);
        if (state->R[0] != rle_source ||
            state->R[1] != rle_wram_destination + index * 0x20u ||
            state->R[2] != saved_r2) {
            return fail("RLUnCompWram changed input registers");
        }
        for (uint32_t byte = 0u; byte < sizeof(rle_expected); ++byte) {
            if (machine.guest_memory().read(
                    cpu, rle_wram_destination + index * 0x20u + byte,
                    1u).value != rle_expected[byte]) {
                return fail("RLUnCompWram literal/repeat decoding failed");
            }
        }

        if (!oracis_arm_runtime_select_cpu(cpu)) {
            return fail("CPU selection failed for RLUnCompVram");
        }
        state->R[0] = rle_source;
        state->R[1] = rle_vram_destination + index * 0x20u;
        runtime_swi(0x15u);
        for (uint32_t byte = 0u; byte < sizeof(rle_expected); ++byte) {
            if (machine.guest_memory().read(
                    cpu, rle_vram_destination + index * 0x20u + byte,
                    1u).value != rle_expected[byte]) {
                return fail("RLUnCompVram halfword output differed from expected bytes");
            }
        }
    }

    /* Reject wrong type, odd VRAM output length and alignment before writes. */
    constexpr uint32_t rle_guard_source = UINT32_C(0x02001CC0);
    constexpr uint32_t rle_guard_destination = UINT32_C(0x02001CE0);
    const uint8_t rle_bad_type[] = {0x31u, 4u, 0u, 0u, 0u, 0u, 0u, 0u};
    const uint8_t rle_odd_size[] = {
        0x30u, 3u, 0u, 0u, 0x00u, 'X', 'Y', 'Z'
    };
    const uint32_t rle_guard_value = UINT32_C(0x6C3CA55A);
    if (machine.guest_memory().write(
            NDS_ARM9, rle_guard_destination, 4u,
            rle_guard_value) != oracis::memory::Status::Ok ||
        machine.guest_memory().write_bytes(
            NDS_ARM9, rle_guard_source, rle_bad_type,
            static_cast<uint32_t>(sizeof(rle_bad_type))) !=
            oracis::memory::Status::Ok ||
        !oracis_arm_runtime_select_cpu(NDS_ARM9) ||
        oracis_arm_runtime_bios_rle(
            rle_guard_source, rle_guard_destination, 0u) != 0 ||
        machine.guest_memory().write_bytes(
            NDS_ARM9, rle_guard_source, rle_odd_size,
            static_cast<uint32_t>(sizeof(rle_odd_size))) !=
            oracis::memory::Status::Ok ||
        oracis_arm_runtime_bios_rle(
            rle_guard_source, rle_guard_destination, 1u) != 0 ||
        oracis_arm_runtime_bios_rle(
            rle_guard_source + 1u, rle_guard_destination, 0u) != 0 ||
        oracis_arm_runtime_bios_rle(
            rle_guard_source, rle_guard_destination + 1u, 1u) != 0 ||
        machine.guest_memory().read(
            NDS_ARM9, rle_guard_destination, 4u).value != rle_guard_value) {
        return fail("RLE accepted invalid type/alignment/VRAM size or modified destination");
    }

    /*
     * NDS BIOS HuffUnComp (0x13) reads MSB-first bits from little-endian
     * 32-bit words and packs output symbols from the least-significant end.
     * This 8-bit tree maps 0->'A' and 1->'B', yielding "AABB".
     */
    constexpr uint32_t huffman8_source = UINT32_C(0x02001D00);
    constexpr uint32_t huffman8_destination = UINT32_C(0x02001D40);
    const uint8_t huffman8_stream[] = {
        0x28u, 4u, 0u, 0u,  /* Type 2, 8-bit symbols, four output bytes. */
        1u,                 /* Tree size: (1 * 2) + 1 = three bytes. */
        0xC0u, 'A', 'B',    /* Root: both children are leaf symbols. */
        0u, 0u, 0u, 0x30u   /* Bits 00 11 -> A A B B. */
    };
    if (machine.guest_memory().write_bytes(
            NDS_ARM9, huffman8_source, huffman8_stream,
            static_cast<uint32_t>(sizeof(huffman8_stream))) !=
        oracis::memory::Status::Ok) {
        return fail("could not seed 8-bit Huffman stream");
    }

    const NdsCpu huffman_cpus[2] = {NDS_ARM9, NDS_ARM7};
    for (unsigned index = 0u; index < 2u; ++index) {
        const NdsCpu cpu = huffman_cpus[index];
        if (!oracis_arm_runtime_select_cpu(cpu)) {
            return fail("CPU selection failed for 8-bit Huffman");
        }
        ArmCpuState *state = machine.cpu_state(cpu);
        state->R[0] = huffman8_source;
        state->R[1] = huffman8_destination + index * 0x20u;
        state->R[2] = UINT32_C(0xDEADBEEF); /* Direct mode ignores R2. */
        state->R[3] = 0u;                   /* No callback structure. */
        runtime_swi(0x13u);
        if (machine.guest_memory().read(
                cpu, huffman8_destination + index * 0x20u, 4u).value !=
            UINT32_C(0x42424141)) {
            return fail("8-bit Huffman symbols/word packing were incorrect");
        }
    }

    /*
     * The 4-bit form expands eight symbols into a single 32-bit word.
     * Alternating bits select symbols 3 and A; the low nibble is first.
     */
    constexpr uint32_t huffman4_source = UINT32_C(0x02001D80);
    constexpr uint32_t huffman4_destination = UINT32_C(0x02001DC0);
    const uint8_t huffman4_stream[] = {
        0x24u, 4u, 0u, 0u,  /* Type 2, 4-bit symbols, four output bytes. */
        1u, 0xC0u, 0x03u, 0x0Au,
        0x55u, 0x55u, 0x55u, 0x55u
    };
    if (machine.guest_memory().write_bytes(
            NDS_ARM9, huffman4_source, huffman4_stream,
            static_cast<uint32_t>(sizeof(huffman4_stream))) !=
        oracis::memory::Status::Ok) {
        return fail("could not seed 4-bit Huffman stream");
    }
    for (unsigned index = 0u; index < 2u; ++index) {
        const NdsCpu cpu = huffman_cpus[index];
        if (!oracis_arm_runtime_select_cpu(cpu)) {
            return fail("CPU selection failed for 4-bit Huffman");
        }
        ArmCpuState *state = machine.cpu_state(cpu);
        state->R[0] = huffman4_source;
        state->R[1] = huffman4_destination + index * 0x20u;
        state->R[2] = 0u;
        state->R[3] = 0u;
        runtime_swi(0x13u);
        if (machine.guest_memory().read(
                cpu, huffman4_destination + index * 0x20u, 4u).value !=
            UINT32_C(0xA3A3A3A3)) {
            return fail("4-bit Huffman nibble order/packing was incorrect");
        }
    }

    /* Invalid width/tree/output alignment/callback mode must not write. */
    constexpr uint32_t huffman_guard_source = UINT32_C(0x02001E00);
    constexpr uint32_t huffman_guard_destination = UINT32_C(0x02001E40);
    const uint8_t huffman_bad_width[] = {
        0x21u, 4u, 0u, 0u, 1u, 0xC0u, 'A', 'B', 0u, 0u, 0u, 0u
    };
    const uint8_t huffman_bad_tree[] = {
        0x28u, 4u, 0u, 0u, 1u, 0xFFu, 'A', 'B', 0u, 0u, 0u, 0u
    };
    const uint8_t huffman_partial_output[] = {
        0x28u, 5u, 0u, 0u, 1u, 0xC0u, 'A', 'B', 0u, 0u, 0u, 0u
    };
    const uint32_t huffman_guard_value = UINT32_C(0x5AA5C33C);
    if (machine.guest_memory().write(
            NDS_ARM9, huffman_guard_destination, 4u,
            huffman_guard_value) != oracis::memory::Status::Ok ||
        machine.guest_memory().write_bytes(
            NDS_ARM9, huffman_guard_source, huffman_bad_width,
            static_cast<uint32_t>(sizeof(huffman_bad_width))) !=
            oracis::memory::Status::Ok ||
        !oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("could not prepare invalid Huffman tests");
    }
    if (oracis_arm_runtime_bios_huffman(
            huffman_guard_source, huffman_guard_destination, 0u) != 0 ||
        machine.guest_memory().write_bytes(
            NDS_ARM9, huffman_guard_source, huffman_bad_tree,
            static_cast<uint32_t>(sizeof(huffman_bad_tree))) !=
            oracis::memory::Status::Ok ||
        oracis_arm_runtime_bios_huffman(
            huffman_guard_source, huffman_guard_destination, 0u) != 0 ||
        machine.guest_memory().write_bytes(
            NDS_ARM9, huffman_guard_source, huffman_partial_output,
            static_cast<uint32_t>(sizeof(huffman_partial_output))) !=
            oracis::memory::Status::Ok ||
        oracis_arm_runtime_bios_huffman(
            huffman_guard_source, huffman_guard_destination, 0u) != 0 ||
        oracis_arm_runtime_bios_huffman(
            huffman8_source, huffman_guard_destination, 1u) != 0 ||
        oracis_arm_runtime_bios_huffman(
            huffman8_source + 1u, huffman_guard_destination, 0u) != 0 ||
        oracis_arm_runtime_bios_huffman(
            huffman8_source, huffman_guard_destination + 1u, 0u) != 0 ||
        machine.guest_memory().read(
            NDS_ARM9, huffman_guard_destination, 4u).value !=
            huffman_guard_value) {
        return fail("Huffman accepted invalid stream/alignment/callback or modified destination");
    }

    /*
     * NDS9 Diff8bitUnFilterWram (0x16) restores byte samples by summing
     * subsequent modulo-256 deltas onto the preceding decoded sample.
     */
    constexpr uint32_t diff8_source = UINT32_C(0x02001F00);
    constexpr uint32_t diff8_destination = UINT32_C(0x02001F21);
    const uint8_t diff8_stream[] = {
        0x81u, 5u, 0u, 0u, 10u, 1u, 1u, 1u, 1u
    };
    const uint8_t diff8_expected[] = {10u, 11u, 12u, 13u, 14u};
    if (machine.guest_memory().write_bytes(
            NDS_ARM9, diff8_source, diff8_stream,
            static_cast<uint32_t>(sizeof(diff8_stream))) !=
            oracis::memory::Status::Ok ||
        !oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("could not prepare Diff8 unfilter test");
    }
    arm9_state->R[0] = diff8_source;
    arm9_state->R[1] = diff8_destination;
    runtime_swi(0x16u);
    for (uint32_t index = 0u; index < sizeof(diff8_expected); ++index) {
        if (machine.guest_memory().read(
                NDS_ARM9, diff8_destination + index, 1u).value !=
            diff8_expected[index]) {
            return fail("Diff8 unfilter did not accumulate modulo-256 deltas");
        }
    }

    /*
     * NDS9 Diff16bitUnFilter (0x18) accumulates little-endian halfword deltas
     * modulo 65536 and writes halfword units.
     */
    constexpr uint32_t diff16_source = UINT32_C(0x02001F40);
    constexpr uint32_t diff16_destination = UINT32_C(0x02001F62);
    const uint8_t diff16_stream[] = {
        0x82u, 6u, 0u, 0u,
        0xF0u, 0xFFu, 0x30u, 0x00u, 0xF0u, 0xFFu
    };
    const uint16_t diff16_expected[] = {
        UINT16_C(0xFFF0), UINT16_C(0x0020), UINT16_C(0x0010)
    };
    if (machine.guest_memory().write_bytes(
            NDS_ARM9, diff16_source, diff16_stream,
            static_cast<uint32_t>(sizeof(diff16_stream))) !=
        oracis::memory::Status::Ok) {
        return fail("could not seed Diff16 unfilter stream");
    }
    arm9_state->R[0] = diff16_source;
    arm9_state->R[1] = diff16_destination;
    runtime_swi(0x18u);
    for (uint32_t index = 0u; index < 3u; ++index) {
        if (machine.guest_memory().read(
                NDS_ARM9, diff16_destination + index * 2u, 2u).value !=
            diff16_expected[index]) {
            return fail("Diff16 unfilter did not accumulate modulo-65536 deltas");
        }
    }

    /* These differential BIOS services are NDS9-only and fail closed otherwise. */
    constexpr uint32_t diff_guard_destination = UINT32_C(0x02001FA0);
    constexpr uint32_t diff_guard_value = UINT32_C(0xA55AC33C);
    if (machine.guest_memory().write(
            NDS_ARM7, diff_guard_destination, 4u,
            diff_guard_value) != oracis::memory::Status::Ok ||
        !oracis_arm_runtime_select_cpu(NDS_ARM7) ||
        oracis_arm_runtime_bios_diff(diff8_source, diff_guard_destination, 1u) != 0 ||
        machine.guest_memory().read(
            NDS_ARM7, diff_guard_destination, 4u).value != diff_guard_value) {
        return fail("Diff8/16 services were exposed to ARM7");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9) ||
        oracis_arm_runtime_bios_diff(diff16_source, diff16_destination + 1u, 2u) != 0 ||
        oracis_arm_runtime_bios_diff(diff8_source + 1u, diff8_destination, 1u) != 0) {
        return fail("differential BIOS service accepted invalid alignment");
    }

    /*
     * The native bus bridge must enforce NDS palette/OAM store widths;
     * direct GuestMemory writes do not model STRB suppression.
     */
    constexpr uint32_t engine_a_bg_palette = UINT32_C(0x05000000);
    constexpr uint32_t engine_a_obj_palette = UINT32_C(0x05000200);
    constexpr uint32_t engine_b_bg_palette = UINT32_C(0x05000400);
    constexpr uint32_t engine_b_obj_palette = UINT32_C(0x05000600);
    constexpr uint32_t engine_a_oam = UINT32_C(0x07000000);
    constexpr uint32_t engine_b_oam = UINT32_C(0x07000400);
    constexpr uint32_t palette_marker = UINT32_C(0xA1B2C3D4);
    constexpr uint32_t oam_marker = UINT32_C(0x10293847);
    if (!machine.video_memory().installed()) {
        return fail("fixed video memory device was not installed");
    }

    if (!machine.display_control().installed()) {
        return fail("2D display register device was not installed");
    }
    constexpr uint32_t engine_a_dispcnt = UINT32_C(0x04000000);
    constexpr uint32_t engine_b_dispcnt = UINT32_C(0x04001000);
    constexpr uint32_t engine_a_bg0cnt = UINT32_C(0x04000008);
    constexpr uint32_t engine_b_bg0cnt = UINT32_C(0x04001008);
    bus_write_u32(engine_a_dispcnt, UINT32_C(0x00010013));
    bus_write_u32(engine_b_dispcnt, UINT32_C(0x80000021));
    bus_write_u32(engine_a_bg0cnt, UINT32_C(0xBEEF1234));
    bus_write_u16(engine_b_bg0cnt + 4u, UINT16_C(0xCDEF));

    bus_write_u16(UINT32_C(0x04000010), UINT16_C(0x0234));
    bus_write_u16(UINT32_C(0x04000012), UINT16_C(0x0102));
    bus_write_u16(UINT32_C(0x0400101E), UINT16_C(0xFFFF));

    bus_write_u16(UINT32_C(0x04000020), UINT16_C(0xFF00));
    bus_write_u32(UINT32_C(0x04000028), UINT32_C(0xF8765432));
    bus_write_u32(UINT32_C(0x0400103C), UINT32_C(0xF0000001));

    bus_write_u16(UINT32_C(0x04000040), UINT16_C(0x1234));
    bus_write_u16(UINT32_C(0x04000048), UINT16_C(0xFFFF));
    bus_write_u16(UINT32_C(0x04000050), UINT16_C(0xFFFF));
    bus_write_u16(UINT32_C(0x04000052), UINT16_C(0xFFFF));
    bus_write_u16(UINT32_C(0x04000054), UINT16_C(0xFFFF));
    if (bus_read_u32(engine_a_dispcnt) != UINT32_C(0x00010013) ||
        bus_read_u32(engine_b_dispcnt) != UINT32_C(0x80000021) ||
        bus_read_u16(engine_a_bg0cnt) != UINT16_C(0x1234) ||
        bus_read_u16(engine_a_bg0cnt + 2u) != UINT16_C(0xBEEF) ||
        bus_read_u16(engine_b_bg0cnt + 4u) != UINT16_C(0xCDEF) ||
        machine.display_control().bg_scroll(0u, 0u).horizontal !=
            UINT16_C(0x0034) ||
        machine.display_control().bg_scroll(0u, 0u).vertical !=
            UINT16_C(0x0102) ||
        machine.display_control().bg_scroll(1u, 3u).vertical !=
            UINT16_C(0x01FF) ||
        machine.display_control().bg_affine(0u, 2u).pa != -256 ||
        machine.display_control().bg_affine(0u, 2u).reference_x != -126462926 ||
        machine.display_control().bg_affine(1u, 3u).reference_y != 1 ||
        machine.display_control().window_blend(0u).bounds[0] != UINT16_C(0x1234) ||
        machine.display_control().window_blend(0u).winin !=
            NdsDisplayControl::WindowMask ||
        machine.display_control().window_blend(0u).bldcnt !=
            NdsDisplayControl::BlendControlMask ||
        machine.display_control().window_blend(0u).bldalpha !=
            NdsDisplayControl::BlendAlphaMask ||
        machine.display_control().window_blend(0u).bldy !=
            NdsDisplayControl::BlendYMask) {
        return fail("2D Engine A/B display register bus bridge failed");
    }

    if (bus_read_u16(NdsDisplayControl::DispstatAddress) != UINT16_C(0x0004) ||
        bus_read_u16(NdsDisplayControl::VcountAddress) != 0u) {
        return fail("DISPSTAT/VCOUNT reset state or machine bus bridge was incorrect");
    }
    bus_write_u16(
        NdsDisplayControl::DispstatAddress, UINT16_C(0x0138));
    if (bus_read_u16(NdsDisplayControl::DispstatAddress) != UINT16_C(0x0138)) {
        return fail("DISPSTAT writable IRQ-enable and VCOUNT compare bits were not retained");
    }
    bus_write_u32(engine_a_bg_palette, palette_marker);
    bus_write_u16(engine_a_obj_palette, UINT32_C(0x5566));
    bus_write_u32(engine_b_bg_palette, UINT32_C(0xB1B2B3B4));
    bus_write_u16(engine_b_obj_palette, UINT32_C(0x7788));
    bus_write_u32(engine_a_oam, oam_marker);
    bus_write_u32(engine_b_oam, UINT32_C(0x89ABCDEF));
    bus_write_u8(engine_a_bg_palette, UINT8_C(0x00));
    bus_write_u8(engine_a_oam, UINT8_C(0x00));
    if (bus_read_u32(engine_a_bg_palette) != palette_marker ||
        bus_read_u16(engine_a_obj_palette) != UINT16_C(0x5566) ||
        bus_read_u32(engine_b_bg_palette) != UINT32_C(0xB1B2B3B4) ||
        bus_read_u16(engine_b_obj_palette) != UINT16_C(0x7788) ||
        bus_read_u32(engine_a_oam) != oam_marker ||
        bus_read_u32(engine_b_oam) != UINT32_C(0x89ABCDEF) ||
        machine.guest_memory().read(
            NDS_ARM7, engine_a_bg_palette, 2u).status !=
            oracis::memory::Status::CpuAccessDenied ||
        machine.guest_memory().read(
            NDS_ARM7, engine_a_oam, 2u).status !=
            oracis::memory::Status::CpuAccessDenied) {
        return fail("palette/OAM mappings, CPU permissions or ignored STRB failed");
    }

    /*
     * Configure ARM9 TCM through CP15. The 32KB ITCM is mirrored through a
     * 32MB low-address aperture; the 16KB DTCM overlays main-RAM mirror 1.
     */
    constexpr uint32_t tcm_control = UINT32_C(0x00052078);
    constexpr uint32_t dtcm_disabled_control = UINT32_C(0x00042078);
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 could not be selected for TCM configuration");
    }
    uint32_t invalid_dtcm_region = UINT32_C(0x0000000A);
    if (oracis_arm_runtime_coprocessor_transfer(
            1u, 15u, 0u, 9u, 1u, 0u, &invalid_dtcm_region) != 0) {
        return fail("DTCM accepted the architecturally invalid zero base");
    }
    runtime_coproc_write(15u, 0u, 9u, 1u, 1u, UINT32_C(0x00000020));
    runtime_coproc_write(15u, 0u, 9u, 1u, 0u, UINT32_C(0x027C000A));
    runtime_coproc_write(15u, 0u, 1u, 0u, 0u, tcm_control);
    if (runtime_coproc_read(15u, 0u, 9u, 1u, 1u) != UINT32_C(0x20) ||
        runtime_coproc_read(15u, 0u, 9u, 1u, 0u) != UINT32_C(0x027C000A) ||
        runtime_coproc_read(15u, 0u, 1u, 0u, 0u) != tcm_control ||
        runtime_code_cycles(UINT32_C(0x01FF8000)) != 1u ||
        runtime_mem_cycles(UINT32_C(0x01FF8000), 4u, 0u) != 1u ||
        runtime_mem_cycles(UINT32_C(0x027C0000), 4u, 0u) != 1u) {
        return fail("CP15 did not preserve the supported ARM9 TCM registers");
    }

    if (machine.guest_memory().write(
            NDS_ARM9, UINT32_C(0x023C1000), 4u,
            UINT32_C(0x0BADF00D)) != oracis::memory::Status::Ok ||
        machine.guest_memory().write(
            NDS_ARM9, UINT32_C(0x023C0000), 4u,
            UINT32_C(0x11223344)) != oracis::memory::Status::Ok ||
        machine.guest_memory().write(
            NDS_ARM9, UINT32_C(0x027C0000), 4u,
            UINT32_C(0xAABBCCDD)) != oracis::memory::Status::Ok ||
        machine.guest_memory().read(
            NDS_ARM9, UINT32_C(0x027C0000), 4u).value !=
            UINT32_C(0xAABBCCDD) ||
        machine.guest_memory().read(
            NDS_ARM9, UINT32_C(0x023C0000), 4u).value !=
            UINT32_C(0x11223344) ||
        machine.guest_memory().validate_execute(
            NDS_ARM9, UINT32_C(0x027C0000), 4u) !=
            oracis::memory::Status::Ok) {
        return fail("DTCM overlay did not preserve the underlying main-RAM mirror");
    }

    if (machine.guest_memory().write(
            NDS_ARM9, UINT32_C(0x00000020), 4u,
            UINT32_C(0x55667788)) != oracis::memory::Status::Ok ||
        machine.guest_memory().read(
            NDS_ARM9, UINT32_C(0x00008020), 4u).value !=
            UINT32_C(0x55667788) ||
        machine.guest_memory().read(
            NDS_ARM7, UINT32_C(0x00000020), 4u).status !=
            oracis::memory::Status::CpuAccessDenied) {
        return fail("ITCM physical storage did not mirror privately to ARM9");
    }

    /*
     * The ARM946E-S region register can shrink the visible area below the
     * physical TCM size. The 4KB aperture reveals the RAM underlay immediately
     * after its end; restoring 16KB hides it again.
     */
    runtime_coproc_write(
        15u, 0u, 9u, 1u, 0u, UINT32_C(0x027C0006));
    if (machine.guest_memory().read(
            NDS_ARM9, UINT32_C(0x027C1000), 4u).value !=
            UINT32_C(0x0BADF00D)) {
        return fail("smaller DTCM region did not expose the RAM underlay");
    }
    runtime_coproc_write(
        15u, 0u, 9u, 1u, 0u, UINT32_C(0x027C000A));
    if (machine.guest_memory().read(
            NDS_ARM9, UINT32_C(0x027C1000), 4u).value != 0u) {
        return fail("restored DTCM region did not cover its full physical size");
    }

    runtime_coproc_write(15u, 0u, 1u, 0u, 0u, dtcm_disabled_control);
    if (machine.guest_memory().read(
            NDS_ARM9, UINT32_C(0x027C0000), 4u).value !=
            UINT32_C(0x11223344) ||
        runtime_mem_cycles(UINT32_C(0x027C0000), 4u, 0u) != 20u) {
        return fail("disabling DTCM did not reveal the underlying RAM mirror");
    }
    runtime_coproc_write(15u, 0u, 1u, 0u, 0u, tcm_control);
    if (machine.guest_memory().read(
            NDS_ARM9, UINT32_C(0x027C0000), 4u).value !=
            UINT32_C(0xAABBCCDD)) {
        return fail("re-enabling DTCM did not restore its physical backing");
    }

    /*
     * In DTCM load mode, data reads and instruction fetches bypass DTCM to
     * external RAM, while writes still populate the private DTCM store.
     */
    constexpr uint32_t dtcm_load_control = UINT32_C(0x00072078);
    runtime_coproc_write(15u, 0u, 1u, 0u, 0u, dtcm_load_control);
    if (machine.guest_memory().read(
            NDS_ARM9, UINT32_C(0x027C0000), 4u).value !=
            UINT32_C(0x11223344) ||
        machine.guest_memory().validate_execute(
            NDS_ARM9, UINT32_C(0x027C0000), 4u) !=
            oracis::memory::Status::Ok ||
        machine.guest_memory().write(
            NDS_ARM9, UINT32_C(0x027C0000), 4u,
            UINT32_C(0x88776655)) != oracis::memory::Status::Ok ||
        machine.guest_memory().read(
            NDS_ARM9, UINT32_C(0x027C0000), 4u).value !=
            UINT32_C(0x11223344)) {
        return fail("DTCM load mode did not bypass reads while retaining writes");
    }
    runtime_coproc_write(15u, 0u, 1u, 0u, 0u, tcm_control);
    if (machine.guest_memory().read(
            NDS_ARM9, UINT32_C(0x027C0000), 4u).value !=
            UINT32_C(0x88776655)) {
        return fail("DTCM load-mode writes were not retained in its backing");
    }

    /*
     * ITCM load mode redirects data reads but keeps instruction fetches in
     * ITCM; writes still load the instruction TCM through the data port.
     */
    constexpr uint32_t itcm_load_control = UINT32_C(0x000D2078);
    runtime_coproc_write(15u, 0u, 1u, 0u, 0u, itcm_load_control);
    if (machine.guest_memory().write(
            NDS_ARM9, UINT32_C(0x00000020), 4u,
            UINT32_C(0xA1B2C3D4)) != oracis::memory::Status::Ok ||
        machine.guest_memory().read(
            NDS_ARM9, UINT32_C(0x00000020), 4u).status !=
            oracis::memory::Status::Unmapped ||
        machine.guest_memory().validate_execute(
            NDS_ARM9, UINT32_C(0x00000020), 4u) !=
            oracis::memory::Status::Ok) {
        return fail("ITCM load mode did not distinguish data reads from fetches");
    }
    runtime_coproc_write(15u, 0u, 1u, 0u, 0u, tcm_control);
    if (machine.guest_memory().read(
            NDS_ARM9, UINT32_C(0x00000020), 4u).value !=
            UINT32_C(0xA1B2C3D4)) {
        return fail("ITCM load-mode writes were not retained in its backing");
    }

    /* Load pre-extracted segments only after both CPU destinations pass preflight. */
    const uint8_t arm9_program[] = {0x00u, 0x00u, 0xA0u, 0xE1u};
    const uint8_t arm7_program[] = {0x70u, 0x47u};
    const oracis::machine::NdsProgramSegment arm9_image = {
        NDS_ARM9, UINT32_C(0x02006000),
        static_cast<uint32_t>(sizeof(arm9_program)),
        UINT32_C(0x02006000), false, arm9_program
    };
    const oracis::machine::NdsProgramSegment invalid_arm7_image = {
        NDS_ARM7, UINT32_C(0x05000000),
        static_cast<uint32_t>(sizeof(arm7_program)),
        UINT32_C(0x05000000), true, arm7_program
    };
    if (machine.load_program_segments(
            arm9_image, invalid_arm7_image) !=
            NdsMachineStatus::ProgramMemoryUnavailable ||
        machine.program_loaded() ||
        machine.guest_memory().validate_identity(
            NDS_ARM9, arm9_image.load_address, arm9_image.size,
            arm9_program) != oracis::memory::Status::MissingWriteProvenance) {
        return fail("invalid ARM7 destination partially installed ARM9 program bytes");
    }

    const oracis::machine::NdsProgramSegment arm7_image = {
        NDS_ARM7, UINT32_C(0x02381000),
        static_cast<uint32_t>(sizeof(arm7_program)),
        UINT32_C(0x02381000), true, arm7_program
    };
    if (machine.load_program_segments(arm9_image, arm7_image) !=
            NdsMachineStatus::Ok ||
        !machine.program_loaded() ||
        machine.cpu_state(NDS_ARM9)->R[15] != arm9_image.entry_pc ||
        machine.cpu_state(NDS_ARM7)->R[15] != arm7_image.entry_pc ||
        (machine.cpu_state(NDS_ARM9)->cpsr & CPSR_T_BIT) != 0u ||
        (machine.cpu_state(NDS_ARM7)->cpsr & CPSR_T_BIT) == 0u ||
        machine.guest_memory().validate_identity(
            NDS_ARM9, arm9_image.load_address, arm9_image.size,
            arm9_program) != oracis::memory::Status::Ok ||
        machine.guest_memory().validate_identity(
            NDS_ARM7, arm7_image.load_address, arm7_image.size,
            arm7_program) != oracis::memory::Status::Ok) {
        return fail("valid ARM9/ARM7 segments were not loaded with correct entry modes");
    }
    if (machine.load_program_segments(arm9_image, arm7_image) !=
        NdsMachineStatus::ProgramAlreadyLoaded) {
        return fail("machine accepted a second program load without reset");
    }

    constexpr uint32_t timer_irq = UINT32_C(1) << 3u;
    constexpr uint32_t timer_start_irq = UINT32_C(0x00C00000);
    constexpr uint32_t timer_reload = UINT32_C(0x0000FFFE);
    const NdsCpu cpus[] = {NDS_ARM9, NDS_ARM7};
    for (const NdsCpu cpu : cpus) {
        if (machine.timer_control().write(
                cpu, NdsTimerControl::Timer0CounterAddress, 4u,
                timer_start_irq | timer_reload) !=
                oracis::memory::Status::Ok) {
            return fail("timer setup failed");
        }
    }

    /* A CPU slice now reports its cumulative count at the end boundary. */
    if (machine.begin_cpu_slice(NDS_ARM9, 2u) != NdsMachineStatus::Ok ||
        machine.begin_cpu_slice(NDS_ARM9, 1u) !=
            NdsMachineStatus::SliceAlreadyActive ||
        machine.begin_cpu_slice(NDS_ARM7, 1u) !=
            NdsMachineStatus::SliceAlreadyActive ||
        machine.report_idle_bus_cycles(NDS_ARM7, 1u) !=
            NdsMachineStatus::SliceAlreadyActive) {
        return fail("machine allowed overlapping slices or idle-time accounting");
    }
    runtime_tick(2u);
    if (machine.end_cpu_slice(NDS_ARM9) != NdsMachineStatus::Ok ||
        machine.global_bus_cycles() != 0u) {
        return fail("ARM9-only progress advanced shared time past ARM7");
    }

    if (machine.begin_cpu_slice(NDS_ARM7, 1u) != NdsMachineStatus::Ok) {
        return fail("ARM7 slice begin failed");
    }
    runtime_tick(1u);
    if (machine.end_cpu_slice(NDS_ARM7) != NdsMachineStatus::Ok ||
        machine.global_bus_cycles() != 1u ||
        machine.timer_control().read(
            NDS_ARM9, NdsTimerControl::Timer0CounterAddress, 2u).value !=
            0xFFFFu ||
        machine.timer_control().read(
            NDS_ARM7, NdsTimerControl::Timer0CounterAddress, 2u).value !=
            0xFFFFu) {
        return fail("slice-boundary cycle reports did not advance both timer banks");
    }

    /* Both domains explicitly report idle bus time before the next tick commits. */
    if (machine.report_idle_bus_cycles(NDS_ARM9, 1u) !=
            NdsMachineStatus::Ok ||
        machine.report_idle_bus_cycles(NDS_ARM7, 1u) !=
            NdsMachineStatus::Ok ||
        machine.global_bus_cycles() != 2u ||
        machine.timer_control().read(
            NDS_ARM9, NdsTimerControl::Timer0CounterAddress, 2u).value !=
            0xFFFEu ||
        machine.timer_control().read(
            NDS_ARM7, NdsTimerControl::Timer0CounterAddress, 2u).value !=
            0xFFFEu ||
        (machine.interrupt_control().read(
            NDS_ARM9, NdsInterruptControl::IfAddress, 4u).value &
            timer_irq) == 0u ||
        (machine.interrupt_control().read(
            NDS_ARM7, NdsInterruptControl::IfAddress, 4u).value &
            timer_irq) == 0u) {
        return fail("idle-time watermark did not advance timers and latch IRQs");
    }

    /* Host key state reaches the bound MMIO device and its IRQ controller. */
    if (machine.keypad_control().write(
            NDS_ARM9, NdsKeypadControl::KeyControlAddress, 2u,
            NdsKeypadControl::IrqEnableMask | 1u) !=
            oracis::memory::Status::Ok) {
        return fail("ARM9 KEYCNT setup failed");
    }
    oracis_host_set_keypad_state(1u);
    if (machine.keypad_control().read(
            NDS_ARM9, NdsKeypadControl::KeyInputAddress, 2u).value !=
            UINT32_C(0x03FE) ||
        (machine.interrupt_control().read(
            NDS_ARM9, NdsInterruptControl::IfAddress, 4u).value &
            NdsKeypadControl::KeypadIrqMask) == 0u) {
        return fail("host keypad state did not reach KEYINPUT and keypad IRQ");
    }

    /* NDS7 HALT wakes on IE&IF even with IME=0 and CPSR.I=1. */
    machine.cpu_state(NDS_ARM7)->cpsr |= CPSR_I_BIT;
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::IfAddress, 4u, UINT32_MAX);
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::IeAddress, 4u, 1u);
    if (machine.write_arm7_halt_control(0x80u) != NdsMachineStatus::Ok ||
        !machine.cpu_is_halted(NDS_ARM7) ||
        machine.cpu_can_execute(NDS_ARM7) ||
        machine.begin_cpu_slice(NDS_ARM7, 1u) != NdsMachineStatus::CpuHalted) {
        return fail("ARM7 HALT was not held while IE&IF was clear");
    }
    machine.interrupt_control().request_irq(NDS_ARM7, 1u);
    if (!machine.cpu_can_execute(NDS_ARM7) ||
        machine.cpu_is_halted(NDS_ARM7)) {
        return fail("ARM7 HALT did not wake on IE&IF with IME disabled");
    }

    /* An interrupt already pending at the ARM7 HALT boundary prevents sleep. */
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::IfAddress, 4u, UINT32_MAX);
    machine.interrupt_control().request_irq(NDS_ARM7, 1u);
    if (machine.write_arm7_halt_control(0x80u) != NdsMachineStatus::Ok ||
        machine.cpu_is_halted(NDS_ARM7)) {
        return fail("ARM7 HALT ignored an already pending enabled interrupt");
    }

    /* ARM9 Wait-For-Interrupt uses the IME-gated IRQ line, not CPSR.I. */
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::IfAddress, 4u, UINT32_MAX);
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::IeAddress, 4u, 2u);
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::ImeAddress, 4u, 0u);
    machine.interrupt_control().request_irq(NDS_ARM9, 2u);
    if (machine.enter_cpu_halt(NDS_ARM9) != NdsMachineStatus::Ok ||
        !machine.cpu_is_halted(NDS_ARM9) ||
        machine.cpu_can_execute(NDS_ARM9)) {
        return fail("ARM9 WFI woke while IME kept the IRQ line disabled");
    }
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::ImeAddress, 4u, 1u);
    machine.cpu_state(NDS_ARM9)->cpsr |= CPSR_I_BIT;
    if (!machine.cpu_can_execute(NDS_ARM9) ||
        machine.cpu_is_halted(NDS_ARM9) ||
        (machine.cpu_state(NDS_ARM9)->cpsr & CPSR_I_BIT) == 0u) {
        return fail("ARM9 WFI did not wake on IME/IE/IF independently of CPSR.I");
    }

    if (machine.write_arm7_halt_control(0x40u) !=
            NdsMachineStatus::UnsupportedPowerMode ||
        machine.write_arm7_halt_control(0xC0u) !=
            NdsMachineStatus::UnsupportedPowerMode ||
        machine.cpu_is_halted(NDS_ARM7)) {
        return fail("unsupported GBA-mode/SLEEP requests changed CPU run state");
    }

    /* NDS BIOS SWI 06 dispatches HALT through the currently bound machine. */
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::IfAddress, 4u, UINT32_MAX);
    if (machine.begin_cpu_slice(NDS_ARM7, 5u) != NdsMachineStatus::Ok) {
        return fail("ARM7 HALT SWI test slice did not begin");
    }
    runtime_swi(0x06u);
    const bool halt_swi_yielded = runtime_should_yield();
    const bool halt_swi_halted = machine.cpu_is_halted(NDS_ARM7);
    const uint32_t halt_swi_pending =
        machine.interrupt_control().pending_enabled(NDS_ARM7);
    const uint32_t halt_swi_if =
        machine.interrupt_control().read(
            NDS_ARM7, NdsInterruptControl::IfAddress, 4u).value;
    const auto halt_swi_end = machine.end_cpu_slice(NDS_ARM7);
    if (!halt_swi_yielded || !halt_swi_halted ||
        halt_swi_end != NdsMachineStatus::Ok) {
        std::fprintf(stderr,
            "HALT SWI diagnostic: yield=%d halted=%d IE&IF=0x%08X IF=0x%08X "
            "active_cpu=%u end_status=%u\n",
            halt_swi_yielded ? 1 : 0,
            halt_swi_halted ? 1 : 0,
            halt_swi_pending,
            halt_swi_if,
            static_cast<unsigned>(g_nds_active),
            static_cast<unsigned>(halt_swi_end));
        return fail("BIOS HALT SWI did not set ARM7 HALT and yield its slice");
    }
    machine.interrupt_control().request_irq(NDS_ARM7, 1u);
    if (!machine.cpu_can_execute(NDS_ARM7)) {
        return fail("ARM7 did not resume after the SWI HALT test interrupt");
    }

    /* ARM7 CustomHalt SWI writes its byte argument from R2 to HALTCNT. */
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::IfAddress, 4u, UINT32_MAX);
    if (machine.begin_cpu_slice(NDS_ARM7, 5u) != NdsMachineStatus::Ok) {
        return fail("ARM7 CustomHalt test slice did not begin");
    }
    g_cpu.R[0] = 0x40u; /* Must be ignored by CustomHalt. */
    g_cpu.R[2] = 0x80u;
    runtime_swi(0x1Fu);
    if (!runtime_should_yield() || !machine.cpu_is_halted(NDS_ARM7) ||
        machine.end_cpu_slice(NDS_ARM7) != NdsMachineStatus::Ok) {
        return fail("ARM7 CustomHalt SWI did not write HALTCNT and yield");
    }
    machine.interrupt_control().request_irq(NDS_ARM7, 1u);
    if (!machine.cpu_is_halted(NDS_ARM7)) {
        return fail("ARM7 CustomHalt left HALT before the IRQ boundary check");
    }

    /* ARM9 CP15 WFI is wired to the same machine state transition. */
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::IfAddress, 4u, UINT32_MAX);
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::ImeAddress, 4u, 1u);
    if (machine.begin_cpu_slice(NDS_ARM9, 5u) != NdsMachineStatus::Ok) {
        return fail("ARM9 CP15 WFI test slice did not begin");
    }
    runtime_coproc_write(15u, 0u, 7u, 0u, 4u, 0u);
    if (!runtime_should_yield() || !machine.cpu_is_halted(NDS_ARM9) ||
        machine.end_cpu_slice(NDS_ARM9) != NdsMachineStatus::Ok) {
        return fail("ARM9 CP15 WFI did not set HALT and yield its slice");
    }
    machine.interrupt_control().request_irq(NDS_ARM9, 2u);
    if (!machine.cpu_can_execute(NDS_ARM9)) {
        return fail("ARM9 CP15 WFI did not resume after its IRQ line asserted");
    }

    /*
     * IRQ delivery is deliberately performed only after both CPU slices have
     * ended. The helper must preserve pending IF and use the caller's exact
     * architectural LR/vector base rather than deriving either from R15.
     */
    constexpr uint32_t machine_irq = UINT32_C(1);
    auto *arm9 = machine.cpu_state(NDS_ARM9);
    auto *arm7 = machine.cpu_state(NDS_ARM7);
    if (arm9 == nullptr || arm7 == nullptr) {
        return fail("machine did not expose both CPU states for IRQ delivery");
    }

    /* ARM9: IME and CPSR.I independently gate exception entry. */
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::IfAddress, 4u, UINT32_MAX);
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::IeAddress, 4u, machine_irq);
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::ImeAddress, 4u, 0u);
    machine.interrupt_control().request_irq(NDS_ARM9, machine_irq);
    arm9->cpsr = ORACIS_ARM_MODE_SYSTEM | CPSR_T_BIT | CPSR_Z_BIT;
    arm9->R[15] = UINT32_C(0x02004000);
    const uint32_t arm9_original_cpsr = arm9->cpsr;
    if (machine.try_deliver_pending_irq(
            NDS_ARM9, UINT32_C(0x02004006), UINT32_C(0xFFFF0000)) !=
            NdsMachineStatus::IrqNotEligible ||
        arm9->cpsr != arm9_original_cpsr ||
        arm9->R[15] != UINT32_C(0x02004000)) {
        return fail("ARM9 entered IRQ while IME was disabled or mutated state");
    }

    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::ImeAddress, 4u, 1u);
    arm9->cpsr |= CPSR_I_BIT;
    const uint32_t arm9_masked_cpsr = arm9->cpsr;
    if (machine.try_deliver_pending_irq(
            NDS_ARM9, UINT32_C(0x02004006), UINT32_C(0xFFFF0000)) !=
            NdsMachineStatus::IrqNotEligible ||
        arm9->cpsr != arm9_masked_cpsr) {
        return fail("ARM9 entered IRQ while CPSR.I was set");
    }

    arm9->cpsr &= ~CPSR_I_BIT;
    if (machine.begin_cpu_slice(NDS_ARM9, 1u) != NdsMachineStatus::Ok ||
        machine.try_deliver_pending_irq(
            NDS_ARM9, UINT32_C(0x02004006), UINT32_C(0xFFFF0000)) !=
            NdsMachineStatus::SliceAlreadyActive ||
        machine.end_cpu_slice(NDS_ARM9) != NdsMachineStatus::Ok) {
        return fail("IRQ delivery was not blocked during an active CPU slice");
    }

    const uint32_t arm9_irq_cpsr = arm9->cpsr;
    if (machine.try_deliver_pending_irq(
            NDS_ARM9, UINT32_C(0x02004006), UINT32_C(0xFFFF0000)) !=
            NdsMachineStatus::IrqEntered ||
        (arm9->cpsr & ORACIS_ARM_MODE_MASK) != ORACIS_ARM_MODE_IRQ ||
        (arm9->cpsr & CPSR_T_BIT) != 0u ||
        (arm9->cpsr & CPSR_I_BIT) == 0u ||
        arm9->banked_spsr[ARM_BANK_IRQ] != arm9_irq_cpsr ||
        arm9->R[14] != UINT32_C(0x02004006) ||
        arm9->R[15] != UINT32_C(0xFFFF0018) ||
        (machine.interrupt_control().read(
            NDS_ARM9, NdsInterruptControl::IfAddress, 4u).value &
            machine_irq) == 0u ||
        g_nds_active != NDS_ARM9) {
        return fail("ARM9 IRQ entry did not save context/use supplied LR and vector");
    }

    /* ARM7 wakes from HALT on IE&IF, but CPSR.I still blocks entry. */
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::IfAddress, 4u, UINT32_MAX);
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::IeAddress, 4u, machine_irq);
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::ImeAddress, 4u, 1u);
    machine.interrupt_control().request_irq(NDS_ARM7, machine_irq);
    arm7->cpsr = ORACIS_ARM_MODE_SYSTEM | CPSR_T_BIT | CPSR_Z_BIT | CPSR_I_BIT;
    arm7->R[15] = UINT32_C(0x02005000);
    const uint32_t arm7_masked_cpsr = arm7->cpsr;
    if (machine.try_deliver_pending_irq(
            NDS_ARM7, UINT32_C(0x02005002), 0u) !=
            NdsMachineStatus::IrqNotEligible ||
        machine.cpu_is_halted(NDS_ARM7) ||
        arm7->cpsr != arm7_masked_cpsr) {
        return fail("ARM7 did not wake on IE&IF while keeping CPSR.I entry mask");
    }

    arm7->cpsr &= ~CPSR_I_BIT;
    const uint32_t arm7_irq_cpsr = arm7->cpsr;
    if (machine.try_deliver_pending_irq(
            NDS_ARM7, UINT32_C(0x02005002), 0u) !=
            NdsMachineStatus::IrqEntered ||
        (arm7->cpsr & ORACIS_ARM_MODE_MASK) != ORACIS_ARM_MODE_IRQ ||
        (arm7->cpsr & CPSR_T_BIT) != 0u ||
        (arm7->cpsr & CPSR_I_BIT) == 0u ||
        arm7->banked_spsr[ARM_BANK_IRQ] != arm7_irq_cpsr ||
        arm7->R[14] != UINT32_C(0x02005002) ||
        arm7->R[15] != UINT32_C(0x00000018) ||
        (machine.interrupt_control().read(
            NDS_ARM7, NdsInterruptControl::IfAddress, 4u).value &
            machine_irq) == 0u ||
        g_nds_active != NDS_ARM7) {
        return fail("ARM7 IRQ entry did not wake/enter with supplied LR and vector");
    }

    /*
     * The dual-core round advances both CPUs toward one shared bus-time
     * watermark, preserving the ARM9:ARM7 2:1 cycle ratio.
     */
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::IfAddress, 4u, UINT32_MAX);
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::IeAddress, 4u, 0u);
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::ImeAddress, 4u, 0u);
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::IfAddress, 4u, UINT32_MAX);
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::IeAddress, 4u, 0u);
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::ImeAddress, 4u, 0u);
    arm9->cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm7->cpsr = ORACIS_ARM_MODE_SYSTEM;
    SchedulerFixture scheduler_fixture{};
    scheduler_fixture.yield_early_arm9 = true;

    if (machine.run_dual_core_round(
            0u, &execute_test_slice, &provide_test_irq_context,
            &scheduler_fixture) != NdsMachineStatus::InvalidCycleBudget ||
        machine.run_dual_core_round(
            1u, nullptr, &provide_test_irq_context,
            &scheduler_fixture) != NdsMachineStatus::SliceExecutionFailed) {
        return fail("dual-core scheduler accepted an invalid round configuration");
    }

    if (machine.run_dual_core_round(
            1u, &execute_test_slice, &provide_test_irq_context,
            &scheduler_fixture) != NdsMachineStatus::Ok ||
        scheduler_fixture.calls[0] != 1u ||
        scheduler_fixture.calls[1] != 1u ||
        scheduler_fixture.budgets[0] != 2u ||
        scheduler_fixture.budgets[1] != 1u ||
        machine.global_bus_cycles() != 3u ||
        machine.system_clock().cpu_cycle_count(NDS_ARM9) != 6u ||
        machine.system_clock().cpu_cycle_count(NDS_ARM7) != 3u ||
        oracis_scheduler_cycle_count(NDS_ARM9) != 3u) {
        return fail("dual-core scheduler failed 2:1 timing or early-yield accounting");
    }

    /* A halted ARM7 advances only its elapsed clock time; it is not dispatched. */
    if (machine.write_arm7_halt_control(0x80u) != NdsMachineStatus::Ok ||
        !machine.cpu_is_halted(NDS_ARM7)) {
        return fail("ARM7 did not enter HALT before the scheduler idle test");
    }
    const uint32_t arm7_calls_before_halt = scheduler_fixture.calls[1];
    if (machine.run_dual_core_round(
            1u, &execute_test_slice, &provide_test_irq_context,
            &scheduler_fixture) != NdsMachineStatus::Ok ||
        scheduler_fixture.calls[1] != arm7_calls_before_halt ||
        !machine.cpu_is_halted(NDS_ARM7) ||
        machine.global_bus_cycles() != 4u ||
        machine.system_clock().cpu_cycle_count(NDS_ARM7) != 4u) {
        return fail("dual-core scheduler executed HALTed ARM7 or failed to advance its clock");
    }

    /* A pending enabled ARM7 interrupt wakes HALT and is delivered before code runs. */
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::IeAddress, 4u, machine_irq);
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::ImeAddress, 4u, 1u);
    machine.interrupt_control().request_irq(NDS_ARM7, machine_irq);
    arm7->cpsr = ORACIS_ARM_MODE_SYSTEM | CPSR_Z_BIT;
    const uint32_t arm7_round_irq_cpsr = arm7->cpsr;
    if (machine.run_dual_core_round(
            1u, &execute_test_slice, &provide_test_irq_context,
            &scheduler_fixture) != NdsMachineStatus::Ok ||
        machine.cpu_is_halted(NDS_ARM7) ||
        scheduler_fixture.irq_context_calls[1] != 1u ||
        (arm7->cpsr & ORACIS_ARM_MODE_MASK) != ORACIS_ARM_MODE_IRQ ||
        arm7->banked_spsr[ARM_BANK_IRQ] != arm7_round_irq_cpsr ||
        arm7->R[14] != UINT32_C(0x02005002) ||
        arm7->R[15] != UINT32_C(0x00000018) ||
        scheduler_fixture.calls[1] != arm7_calls_before_halt + 1u ||
        machine.global_bus_cycles() != 5u) {
        return fail("dual-core scheduler did not wake and service ARM7 IRQ at its boundary");
    }

    /*
     * Exercise the production adapter that enters generated banks through
     * runtime_dispatch, rather than the test-only slice callback above.
     */
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::IfAddress, 4u, UINT32_MAX);
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::IeAddress, 4u, 0u);
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::ImeAddress, 4u, 0u);
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::IfAddress, 4u, UINT32_MAX);
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::IeAddress, 4u, 0u);
    machine.interrupt_control().write(
        NDS_ARM7, NdsInterruptControl::ImeAddress, 4u, 0u);
    arm9->cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm7->cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm9->R[15] = UINT32_C(0x02006000);
    arm7->R[15] = UINT32_C(0x02381000);
    g_static_dispatch_calls[0] = 0u;
    g_static_dispatch_calls[1] = 0u;

    const NdsDispatchEntry arm9_test_bank[] = {
        {UINT32_C(0x02006000), 0u, &static_arm9_test_bank, nullptr}
    };
    const NdsDispatchEntry arm7_test_bank[] = {
        {UINT32_C(0x02381000), 0u, &static_arm7_test_bank, nullptr}
    };
    if (oracis_static_dispatch_register_bank(
            NDS_ARM9, arm9_test_bank, 1u, "machine-test-arm9") != 0 ||
        oracis_static_dispatch_register_bank(
            NDS_ARM7, arm7_test_bank, 1u, "machine-test-arm7") != 0) {
        oracis_static_dispatch_clear();
        return fail("machine test static banks could not be registered");
    }

    const NdsMachineStatus static_round = machine.run_dual_core_round(
        1u, &NdsMachine::execute_registered_bank_slice,
        &provide_test_irq_context, &scheduler_fixture);
    const bool banks_dispatched =
        static_round == NdsMachineStatus::Ok &&
        g_static_dispatch_calls[0] == 2u &&
        g_static_dispatch_calls[1] == 1u &&
        machine.global_bus_cycles() == 6u;

    const bool arm9_unregistered = oracis_static_dispatch_unregister_bank(
        NDS_ARM9, arm9_test_bank, 1u) != 0;
    const bool arm7_unregistered = oracis_static_dispatch_unregister_bank(
        NDS_ARM7, arm7_test_bank, 1u) != 0;
    if (!banks_dispatched || !arm9_unregistered || !arm7_unregistered) {
        oracis_static_dispatch_clear();
        return fail("dual-core rounds did not invoke both registered static banks");
    }

    /*
     * Generated-bank validation must consult the machine's live GuestMemory.
     * Test both instruction bytes and the dependent immutable data closure.
     */
    constexpr uint32_t validated_arm9_pc = UINT32_C(0x02007000);
    constexpr uint32_t validated_arm9_data = UINT32_C(0x02007100);
    constexpr uint32_t validated_arm7_pc = UINT32_C(0x02382000);
    const uint8_t validated_arm9_code[] = {0x00u, 0x00u, 0xA0u, 0xE1u};
    const uint8_t validated_arm9_data_bytes[] = {
        0x11u, 0x22u, 0x33u, 0x44u
    };
    const uint8_t validated_arm7_code[] = {0x70u, 0x47u};
    if (machine.guest_memory().write_bytes(
            NDS_ARM9, validated_arm9_pc, validated_arm9_code,
            static_cast<uint32_t>(sizeof(validated_arm9_code))) !=
            oracis::memory::Status::Ok ||
        machine.guest_memory().write_bytes(
            NDS_ARM9, validated_arm9_data, validated_arm9_data_bytes,
            static_cast<uint32_t>(sizeof(validated_arm9_data_bytes))) !=
            oracis::memory::Status::Ok ||
        machine.guest_memory().write_bytes(
            NDS_ARM7, validated_arm7_pc, validated_arm7_code,
            static_cast<uint32_t>(sizeof(validated_arm7_code))) !=
            oracis::memory::Status::Ok) {
        return fail("validated static-bank fixture bytes could not be materialized");
    }

    const NdsStaticValidationRange arm9_dependency[] = {{
        validated_arm9_data,
        static_cast<uint32_t>(sizeof(validated_arm9_data_bytes)),
        validated_arm9_data_bytes
    }};
    const NdsStaticValidation arm9_validation = {
        validated_arm9_pc,
        static_cast<uint32_t>(sizeof(validated_arm9_code)),
        validated_arm9_code,
        arm9_dependency,
        1u
    };
    const NdsStaticValidation arm7_validation = {
        validated_arm7_pc,
        static_cast<uint32_t>(sizeof(validated_arm7_code)),
        validated_arm7_code,
        nullptr,
        0u
    };
    const NdsDispatchEntry validated_arm9_bank[] = {{
        validated_arm9_pc, 0u, &static_arm9_test_bank, &arm9_validation
    }};
    const NdsDispatchEntry validated_arm7_bank[] = {{
        validated_arm7_pc, 1u, &static_arm7_test_bank, &arm7_validation
    }};
    if (oracis_static_dispatch_register_bank(
            NDS_ARM9, validated_arm9_bank, 1u,
            "machine-validated-arm9") != ORACIS_DISPATCH_REGISTER_OK ||
        oracis_static_dispatch_register_bank(
            NDS_ARM7, validated_arm7_bank, 1u,
            "machine-validated-arm7") != ORACIS_DISPATCH_REGISTER_OK) {
        oracis_static_dispatch_clear();
        return fail("validated static banks could not be registered");
    }

    uint32_t validation_status = UINT32_MAX;
    uint32_t validation_candidates = 0u;
    const NdsDispatchEntry *inactive_candidate = nullptr;
    arm9->cpsr = ORACIS_ARM_MODE_SYSTEM;
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9) ||
        oracis_static_dispatch_lookup(
            NDS_ARM9, validated_arm9_pc, arm9->cpsr,
            &validation_status, &validation_candidates,
            &inactive_candidate) != validated_arm9_bank ||
        validation_status != ORACIS_DISPATCH_LOOKUP_FOUND ||
        validation_candidates != 1u || inactive_candidate != nullptr) {
        return fail("live ARM9 code and dependency bytes did not validate");
    }

    /* The registry CPU must match the runtime-selected CPU before validation. */
    arm7->cpsr = ORACIS_ARM_MODE_SYSTEM | CPSR_T_BIT;
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7) ||
        oracis_static_dispatch_lookup(
            NDS_ARM9, validated_arm9_pc, arm9->cpsr,
            &validation_status, &validation_candidates,
            &inactive_candidate) != nullptr ||
        validation_status != ORACIS_DISPATCH_LOOKUP_MISS ||
        inactive_candidate != validated_arm9_bank) {
        return fail("ARM9 bank validation was allowed while ARM7 was selected");
    }
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 runtime state could not be reselected after CPU mismatch test");
    }

    const uint8_t altered_dependency[] = {
        0x11u, 0x22u, 0x99u, 0x44u
    };
    if (machine.guest_memory().write_bytes(
            NDS_ARM9, validated_arm9_data, altered_dependency,
            static_cast<uint32_t>(sizeof(altered_dependency))) !=
            oracis::memory::Status::Ok ||
        oracis_static_dispatch_lookup(
            NDS_ARM9, validated_arm9_pc, arm9->cpsr,
            &validation_status, &validation_candidates,
            &inactive_candidate) != nullptr ||
        validation_status != ORACIS_DISPATCH_LOOKUP_MISS ||
        inactive_candidate != validated_arm9_bank) {
        return fail("changed ARM9 dependency bytes were accepted by dispatch");
    }

    if (machine.guest_memory().write_bytes(
            NDS_ARM9, validated_arm9_data, validated_arm9_data_bytes,
            static_cast<uint32_t>(sizeof(validated_arm9_data_bytes))) !=
            oracis::memory::Status::Ok) {
        return fail("ARM9 dependency fixture could not be restored");
    }
    const uint8_t altered_arm9_code[] = {0x00u, 0x00u, 0xA1u, 0xE1u};
    if (machine.guest_memory().write_bytes(
            NDS_ARM9, validated_arm9_pc, altered_arm9_code,
            static_cast<uint32_t>(sizeof(altered_arm9_code))) !=
            oracis::memory::Status::Ok ||
        oracis_static_dispatch_lookup(
            NDS_ARM9, validated_arm9_pc, arm9->cpsr,
            &validation_status, &validation_candidates,
            &inactive_candidate) != nullptr ||
        validation_status != ORACIS_DISPATCH_LOOKUP_MISS ||
        inactive_candidate != validated_arm9_bank) {
        return fail("changed ARM9 instruction bytes were accepted by dispatch");
    }

    arm7->cpsr = ORACIS_ARM_MODE_SYSTEM | CPSR_T_BIT;
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9) ||
        oracis_static_dispatch_lookup(
            NDS_ARM7, validated_arm7_pc | 1u, arm7->cpsr,
            &validation_status, &validation_candidates,
            &inactive_candidate) != nullptr ||
        validation_status != ORACIS_DISPATCH_LOOKUP_MISS ||
        inactive_candidate != validated_arm7_bank) {
        return fail("ARM7 validation used the wrong active CPU memory context");
    }
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7) ||
        oracis_static_dispatch_lookup(
            NDS_ARM7, validated_arm7_pc | 1u, arm7->cpsr,
            &validation_status, &validation_candidates,
            &inactive_candidate) != validated_arm7_bank ||
        validation_status != ORACIS_DISPATCH_LOOKUP_FOUND ||
        inactive_candidate != nullptr) {
        return fail("live ARM7 Thumb code did not validate against ARM7 memory");
    }

    {
        NdsMachine competing_machine(SharedWramMode::Arm9All32K);
        if (competing_machine.initialize() !=
                NdsMachineStatus::RuntimeAlreadyBound) {
            return fail("a second machine was allowed to replace live CPU states");
        }
    }
    if (machine.global_bus_cycles() != 6u) {
        return fail("a rejected competing machine disturbed the active scheduler");
    }

    /*
     * ARM9 CP15 Control.V owns the high-vector selection. The legacy host
     * callback deliberately reports the low base; the machine must use CP15.
     */
    constexpr uint32_t cp15_vector_select =
        oracis::memory::NdsArm9Tcm::ResetControl;
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 could not be selected for unsupported CP15 write test");
    }
    uint32_t unsupported_cp15_control = 1u;
    if (oracis_arm_runtime_coprocessor_transfer(
            1u, 15u, 0u, 1u, 0u, 0u, &unsupported_cp15_control) != 0 ||
        machine.exception_vector_base(NDS_ARM9) != UINT32_C(0xFFFF0000)) {
        return fail("unsupported ARM9 CP15 MPU/cache control bits were silently accepted");
    }
    arm9->cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm9->R[15] = UINT32_C(0x02006000);
    arm7->cpsr = ORACIS_ARM_MODE_SYSTEM;
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 could not be selected for CP15 register access");
    }
    runtime_coproc_write(15u, 0u, 1u, 0u, 0u, cp15_vector_select);
    if (runtime_coproc_read(15u, 0u, 1u, 0u, 0u) != cp15_vector_select ||
        machine.exception_vector_base(NDS_ARM9) != UINT32_C(0xFFFF0000) ||
        machine.exception_vector_base(NDS_ARM7) != 0u) {
        return fail("CP15 Control.V did not select high ARM9 vectors independently");
    }

    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::IfAddress, 4u, UINT32_MAX);
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::IeAddress, 4u, machine_irq);
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::ImeAddress, 4u, 1u);
    machine.interrupt_control().request_irq(NDS_ARM9, machine_irq);
    SchedulerFixture cp15_irq_fixture{};
    if (machine.run_dual_core_round(
            1u, &execute_test_slice, &provide_test_irq_context,
            &cp15_irq_fixture) != NdsMachineStatus::Ok ||
        cp15_irq_fixture.irq_context_calls[0] != 1u ||
        (arm9->cpsr & ORACIS_ARM_MODE_MASK) != ORACIS_ARM_MODE_IRQ ||
        arm9->R[15] != UINT32_C(0xFFFF0018)) {
        return fail("dual-core IRQ scheduling ignored ARM9 CP15 high-vector selection");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 could not be reselected after high-vector IRQ test");
    }
    runtime_coproc_write(
        15u, 0u, 1u, 0u, 0u,
        oracis::memory::NdsArm9Tcm::RequiredControlOnesMask);
    arm9->cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm9->R[15] = UINT32_C(0x02006000);
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::ImeAddress, 4u, 0u);
    machine.interrupt_control().write(
        NDS_ARM9, NdsInterruptControl::IeAddress, 4u, 0u);
    if (machine.exception_vector_base(NDS_ARM9) != 0u ||
        machine.global_bus_cycles() != 7u) {
        return fail("CP15 Control.V reset-to-low selection or cycle accounting failed");
    }

    /* Preserve the ARM7 Thumb mode expected by its registered dispatch entry. */
    arm7->cpsr = ORACIS_ARM_MODE_SYSTEM | CPSR_T_BIT;

    if (machine.shutdown() != NdsMachineStatus::Ok ||
        machine.initialized() ||
        oracis_arm_runtime_current_state() != nullptr ||
        oracis_bus_global_bus_cycles() != 0u) {
        return fail("machine shutdown did not unbind/reset its runtime safely");
    }
    if (machine.end_cpu_slice(NDS_ARM9) != NdsMachineStatus::NotInitialized) {
        return fail("shutdown machine accepted a new CPU slice");
    }

    /* The global validator must be cleared before this machine's memory dies. */
    validation_status = UINT32_MAX;
    inactive_candidate = nullptr;
    const NdsDispatchEntry *post_shutdown_arm9 = oracis_static_dispatch_lookup(
        NDS_ARM9, validated_arm9_pc, arm9->cpsr,
        &validation_status, &validation_candidates, &inactive_candidate);
    const uint32_t post_shutdown_arm9_status = validation_status;
    const uint32_t post_shutdown_arm9_candidates = validation_candidates;
    const NdsDispatchEntry *post_shutdown_arm9_inactive = inactive_candidate;

    const NdsDispatchEntry *post_shutdown_arm7 = oracis_static_dispatch_lookup(
        NDS_ARM7, validated_arm7_pc | 1u, arm7->cpsr,
        &validation_status, &validation_candidates, &inactive_candidate);
    const uint32_t post_shutdown_arm7_status = validation_status;
    const uint32_t post_shutdown_arm7_candidates = validation_candidates;
    const NdsDispatchEntry *post_shutdown_arm7_inactive = inactive_candidate;

    if (post_shutdown_arm9 != nullptr ||
        post_shutdown_arm9_status != ORACIS_DISPATCH_LOOKUP_MISS ||
        post_shutdown_arm9_candidates != 1u ||
        post_shutdown_arm9_inactive != validated_arm9_bank ||
        post_shutdown_arm7 != nullptr ||
        post_shutdown_arm7_status != ORACIS_DISPATCH_LOOKUP_MISS ||
        post_shutdown_arm7_candidates != 1u ||
        post_shutdown_arm7_inactive != validated_arm7_bank) {
        std::fprintf(stderr,
            "post-shutdown dispatch diagnostics: "
            "ARM9 selected=%p status=%u candidates=%u inactive=%p expected=%p; "
            "ARM7 selected=%p status=%u candidates=%u inactive=%p expected=%p; "
            "active_state=%p active_cpu=%u\n",
            static_cast<const void *>(post_shutdown_arm9),
            post_shutdown_arm9_status, post_shutdown_arm9_candidates,
            static_cast<const void *>(post_shutdown_arm9_inactive),
            static_cast<const void *>(validated_arm9_bank),
            static_cast<const void *>(post_shutdown_arm7),
            post_shutdown_arm7_status, post_shutdown_arm7_candidates,
            static_cast<const void *>(post_shutdown_arm7_inactive),
            static_cast<const void *>(validated_arm7_bank),
            static_cast<void *>(oracis_arm_runtime_current_state()),
            static_cast<unsigned>(g_nds_active));
        return fail("shutdown left the process-global GuestMemory validator installed");
    }
    if (oracis_static_dispatch_unregister_bank(
            NDS_ARM9, validated_arm9_bank, 1u) == 0 ||
        oracis_static_dispatch_unregister_bank(
            NDS_ARM7, validated_arm7_bank, 1u) == 0) {
        oracis_static_dispatch_clear();
        return fail("validated static banks could not be unregistered after shutdown");
    }

    oracis_host_set_keypad_state(0u);
    NdsMachine successor(SharedWramMode::Arm9All32K);
    if (successor.initialize() != NdsMachineStatus::Ok ||
        successor.shutdown() != NdsMachineStatus::Ok) {
        return fail("machine ownership was not released after shutdown");
    }

    std::puts("NdsMachine ownership, dual-core scheduling, dispatch and IRQ tests passed");
    return 0;
}

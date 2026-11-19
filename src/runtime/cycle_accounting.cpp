/*
 * Fail-closed timing ABI for generated NDSRecomp banks.
 *
 * These symbols are part of the generated C contract, but Oracis has not yet
 * completed the retail DS wait-state/TCM/cache model needed to calculate them.
 * Keeping the symbols defined lets banks link; executing an unsupported timing
 * path stops with CPU/PC and operation diagnostics instead of inventing cycles.
 */
#include "oracis/recomp/runtime_arm.h"

#include <cstdio>
#include <cstdlib>

namespace {

[[noreturn]] void timing_unavailable(
    const char *operation,
    uint32_t address,
    uint32_t detail0,
    uint32_t detail1,
    uint32_t detail2)
{
    std::fprintf(stderr,
        "[Oracis timing] %s is not implemented: CPU=%u PC=0x%08X "
        "address=0x%08X detail=(%u,%u,%u)\n",
        operation,
        static_cast<unsigned>(g_nds_active),
        oracis_active_cpu_state
            ? oracis_active_cpu_state->R[15] : 0u,
        address,
        detail0, detail1, detail2);
    std::abort();
}

} // namespace

namespace {

uint32_t g_arm9_itcm_code_window_size = 0u;

uint32_t arm9_uncached_code_fetch_cycles(uint32_t pc)
{
    /*
     * These costs are in ARM9 core cycles (66 MHz). The documented DS bus
     * costs are in 33 MHz units: Main RAM N32=9 -> 18 core cycles, and
     * WRAM/BIOS/I/O/OAM N32=4 -> 8 core cycles. ITCM is 0.5 bus cycles -> 1.
     *
     * This first pass deliberately excludes cache, VRAM/Palette executable
     * mappings, gamecard windows and arbitrary unmapped addresses.
     */
    if (pc < g_arm9_itcm_code_window_size) return 1u;
    if ((pc - UINT32_C(0x02000000)) < UINT32_C(0x01000000)) {
        return 18u; /* uncached Main RAM code fetch */
    }
    if ((pc - UINT32_C(0x03000000)) < UINT32_C(0x01000000) ||
        (pc - UINT32_C(0x04000000)) < UINT32_C(0x01000000) ||
        (pc - UINT32_C(0x07000000)) < UINT32_C(0x00004000) ||
        (pc - UINT32_C(0xFFFF0000)) < UINT32_C(0x00008000)) {
        return 8u; /* WRAM, BIOS, I/O or OAM code fetch */
    }

    timing_unavailable("runtime_code_cycles unsupported ARM9 code region",
                       pc, g_arm9_itcm_code_window_size, 0u, 0u);
}

} // namespace

extern "C" void oracis_runtime_set_arm9_itcm_code_window(uint32_t size)
{
    if (size != 0u &&
        (size < UINT32_C(0x00001000) ||
         size > UINT32_C(0x02000000) ||
         (size & (size - 1u)) != 0u)) {
        timing_unavailable(
            "invalid ARM9 ITCM code window", 0u, size, 0u, 0u);
    }
    g_arm9_itcm_code_window_size = size;
}

extern "C" uint32_t runtime_code_cycles(uint32_t pc)
{
    if (oracis_active_cpu_state == nullptr ||
        (g_nds_active != NDS_ARM9 && g_nds_active != NDS_ARM7)) {
        timing_unavailable("runtime_code_cycles without active CPU",
                           pc, 0u, 0u, 0u);
    }

    /* The ARM7 generated instruction base already includes its fetch cost. */
    if (g_nds_active == NDS_ARM7) return 0u;

    const bool thumb = (g_cpu.cpsr & CPSR_T_BIT) != 0u;
    /*
     * The NDS9 fetches two Thumb halfwords per 32-bit bus fetch. The full
     * 32-bit fetch cost is charged at the first halfword; the second halfword
     * has no additional cost. Taken branches are charged separately by the
     * pipeline-refill provider (still fail-closed).
     */
    if (thumb && (pc & 2u) != 0u) return 0u;

    /* ITCM is one ARM9 core cycle per 32-bit fetch, even in Thumb state. */
    if (pc < g_arm9_itcm_code_window_size) return 1u;

    return arm9_uncached_code_fetch_cycles(pc);
}

extern "C" uint32_t runtime_mem_cycles(
    uint32_t addr, uint32_t width, uint32_t sequential)
{
    timing_unavailable(
        "runtime_mem_cycles", addr, width, sequential, 0u);
}

extern "C" uint32_t runtime_mul_cycles(
    uint32_t rs_value, uint32_t signed_variant, uint32_t extra)
{
    timing_unavailable(
        "runtime_mul_cycles", 0u, rs_value, signed_variant, extra);
}

extern "C" uint32_t arm7_refill_cycles(uint32_t target)
{
    timing_unavailable("arm7_refill_cycles", target, 0u, 0u, 0u);
}

extern "C" uint32_t arm9_refill_cycles(uint32_t target)
{
    timing_unavailable("arm9_refill_cycles", target, 0u, 0u, 0u);
}

extern "C" uint32_t arm7_cycle_combine(
    uint32_t flat_cycles, uint32_t num_data,
    uint32_t is_load, uint32_t has_internal)
{
    timing_unavailable(
        "arm7_cycle_combine", 0u,
        flat_cycles, num_data,
        (is_load & 0xFFFFu) | ((has_internal & 0xFFFFu) << 16u));
}

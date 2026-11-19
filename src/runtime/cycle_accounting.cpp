/*
 * Partial timing ABI for generated NDSRecomp banks.
 *
 * ARM9 uncached code fetch and ARM9/ARM7 pipeline refill are implemented for
 * documented memory classes. Data timing, multiply, ARM7 cycle combination,
 * cache and dynamic device waits remain fail-closed with CPU/PC diagnostics.
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
     * costs are in 33 MHz units: Main RAM N32=9 -> 18 core cycles,
     * WRAM/BIOS/I/O/OAM N32=4 -> 8, Palette/VRAM N32=5 -> 10, and ITCM
     * 0.5 -> 1 core cycle. Cache and Gamecard windows remain unsupported.
     */
    if (pc < g_arm9_itcm_code_window_size) return 1u;
    if ((pc - UINT32_C(0x02000000)) < UINT32_C(0x01000000)) {
        return 18u; /* uncached Main RAM code fetch */
    }
    if ((pc - UINT32_C(0x03000000)) < UINT32_C(0x01000000) ||
        (pc - UINT32_C(0x04000000)) < UINT32_C(0x00001000) ||
        (pc - UINT32_C(0x07000000)) < UINT32_C(0x00004000) ||
        (pc - UINT32_C(0xFFFF0000)) < UINT32_C(0x00008000)) {
        return 8u; /* WRAM, BIOS, I/O or OAM code fetch */
    }
    if ((pc - UINT32_C(0x05000000)) < UINT32_C(0x02000000)) {
        return 10u; /* Palette/VRAM code fetch */
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
     * ARM9/ARM7 pipeline-refill providers for supported target regions.
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

namespace {

uint32_t arm7_sequential_code_fetch_cycles(
    uint32_t address, bool thumb)
{
    /*
     * ARM7TDMI branch refill performs two sequential instruction fetches.
     * Costs are NDS7/CODE S32 or S16 in 33MHz ARM7 cycles.
     */
    if ((address - UINT32_C(0x02000000)) < UINT32_C(0x01000000)) {
        return thumb ? 1u : 2u; /* Main RAM: S16=1, S32=2 */
    }
    if (address < UINT32_C(0x00004000) ||
        (address - UINT32_C(0x03000000)) < UINT32_C(0x01000000) ||
        (address - UINT32_C(0x04000000)) < UINT32_C(0x00001000) ||
        (address - UINT32_C(0x07000000)) < UINT32_C(0x00004000)) {
        return 1u; /* BIOS, WRAM, I/O and OAM: S16=S32=1 */
    }
    if ((address - UINT32_C(0x05000000)) < UINT32_C(0x02000000)) {
        return thumb ? 1u : 2u; /* Palette/VRAM: S16=1, S32=2 */
    }

    timing_unavailable(
        "arm7_refill_cycles unsupported code region",
        address, thumb ? 2u : 4u, 0u, 0u);
}

} // namespace

extern "C" uint32_t arm7_refill_cycles(uint32_t target)
{
    if (oracis_active_cpu_state == nullptr || g_nds_active != NDS_ARM7) {
        timing_unavailable(
            "arm7_refill_cycles without active ARM7", target, 0u, 0u, 0u);
    }

    const bool thumb = (target & 1u) != 0u;
    const uint32_t instruction_width = thumb ? 2u : 4u;
    const uint32_t first_address = thumb
        ? (target & ~UINT32_C(1))
        : (target & ~UINT32_C(3));

    /*
     * ARM7TDMI's branch timing table marks both destination fetches
     * sequential. Count each fetch independently so region-boundary
     * crossings use the correct S16/S32 wait state on each side.
     */
    uint32_t cycles = 0u;
    for (uint32_t fetch = 0u; fetch < 2u; ++fetch) {
        const uint32_t address =
            first_address + fetch * instruction_width;
        cycles += arm7_sequential_code_fetch_cycles(address, thumb);
    }
    return cycles;
}

extern "C" uint32_t arm9_refill_cycles(uint32_t target)
{
    if (oracis_active_cpu_state == nullptr || g_nds_active != NDS_ARM9) {
        timing_unavailable(
            "arm9_refill_cycles without active ARM9", target, 0u, 0u, 0u);
    }

    const bool thumb = (target & 1u) != 0u;
    const uint32_t target_address = thumb
        ? (target & ~UINT32_C(1))
        : (target & ~UINT32_C(3));
    const uint32_t fetch_count =
        (!thumb || (target_address & 2u) != 0u) ? 2u : 1u;
    const uint32_t first_word = target_address & ~UINT32_C(3);

    /*
     * ARM9's branch/PC-write refill performs two instruction fetches. In
     * Thumb state, a pair of aligned halfwords shares one 32-bit N32 fetch;
     * a target in the second halfword needs two N32 fetches. NDS9/CODE marks
     * both refill fetches sequential, but the DS forces S32 to N32 for ARM9
     * code. Reclassify each word independently at region boundaries.
     */
    uint32_t cycles = 0u;
    for (uint32_t fetch = 0u; fetch < fetch_count; ++fetch) {
        const uint32_t address = first_word + fetch * 4u;
        cycles += arm9_uncached_code_fetch_cycles(address);
    }
    return cycles;
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

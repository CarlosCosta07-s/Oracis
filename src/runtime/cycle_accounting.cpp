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

extern "C" uint32_t runtime_code_cycles(uint32_t pc)
{
    timing_unavailable("runtime_code_cycles", pc, 0u, 0u, 0u);
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

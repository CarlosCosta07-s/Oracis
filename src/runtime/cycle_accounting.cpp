/*
 * Partial timing ABI for generated NDSRecomp banks.
 *
 * ARM9 uncached fetch, ARM9/ARM7 pipeline refill and ARM7TDMI multiply
 * early-termination timing are implemented for documented cases. ARM7 cycle
 * combination, cache and dynamic device waits remain fail-closed.
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
uint32_t g_arm9_dtcm_base = 0u;
uint32_t g_arm9_dtcm_window_size = 0u;
bool g_arm9_itcm_load_mode = false;
bool g_arm9_dtcm_load_mode = false;

/* Per-instruction context published by the generated ARM7 code prologue. */
constexpr uint32_t kNoArm7TimingAddress = UINT32_MAX;
uint32_t g_arm7_last_code_pc = kNoArm7TimingAddress;
uint32_t g_arm7_last_data_addr = kNoArm7TimingAddress;
bool g_arm7_last_code_thumb = false;

bool range_contains(
    uint32_t address, uint32_t width,
    uint32_t base, uint32_t size) noexcept
{
    if (size == 0u) return false;
    const uint64_t access_begin = address;
    const uint64_t access_end = access_begin + width;
    const uint64_t region_begin = base;
    const uint64_t region_end = region_begin + size;
    return access_begin >= region_begin && access_end <= region_end;
}

bool range_overlaps(
    uint32_t address, uint32_t width,
    uint32_t base, uint32_t size) noexcept
{
    if (size == 0u) return false;
    const uint64_t access_begin = address;
    const uint64_t access_end = access_begin + width;
    const uint64_t region_begin = base;
    const uint64_t region_end = region_begin + size;
    return access_begin < region_end && region_begin < access_end;
}

uint32_t select_bus_timing(
    uint32_t width,
    uint32_t sequential,
    uint32_t n32,
    uint32_t s32,
    uint32_t n16,
    uint32_t s16) noexcept
{
    if (width >= 4u) return sequential != 0u ? s32 : n32;
    return sequential != 0u ? s16 : n16;
}

struct Arm7TimingRegion {
    uint32_t n16;
    uint32_t s16;
    uint8_t bus_width;
};

/*
 * NDS7/CODE differs from NDS7/DATA. Main-RAM CODE is N32=9/N16=8,
 * while DATA is N32=10/N16=9. Keep code fetch classification separate.
 */
Arm7TimingRegion arm7_timing_region(uint32_t address)
{
    if ((address - UINT32_C(0x02000000)) < UINT32_C(0x01000000)) {
        return {8u, 1u, 16u}; /* Main RAM NDS7/CODE */
    }
    if (address < UINT32_C(0x00004000) ||
        (address - UINT32_C(0x03000000)) < UINT32_C(0x01000000) ||
        (address - UINT32_C(0x04000000)) < UINT32_C(0x00001000) ||
        (address - UINT32_C(0x07000000)) < UINT32_C(0x00004000)) {
        return {1u, 1u, 32u}; /* BIOS, WRAM, I/O and OAM */
    }
    if ((address - UINT32_C(0x06000000)) < UINT32_C(0x00040000)) {
        return {1u, 1u, 16u}; /* Potential ARM7 VRAM C/D Work RAM window */
    }
    timing_unavailable(
        "unsupported ARM7 code/data timing region",
        address, 0u, 0u, 0u);
}

uint32_t arm7_n32(const Arm7TimingRegion &region) noexcept
{
    return region.bus_width == 32u
        ? region.n16 : region.n16 + region.s16;
}

uint32_t arm7_s32(const Arm7TimingRegion &region) noexcept
{
    return region.bus_width == 32u
        ? region.s16 : 2u * region.s16;
}

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

uint32_t arm9_external_data_access_cycles(
    uint32_t addr, uint32_t width, uint32_t sequential)
{
    /* NDS9/DATA values are 33MHz bus cycles; ARM9 runs at 66MHz. */
    uint32_t bus_cycles = 0u;
    if (range_contains(
            addr, width, UINT32_C(0x02000000), UINT32_C(0x01000000))) {
        bus_cycles = select_bus_timing(
            width, sequential, 10u, 2u, 9u, 1u);
    } else if (
        range_contains(addr, width, UINT32_C(0x03000000),
                       UINT32_C(0x01000000)) ||
        range_contains(addr, width, UINT32_C(0x04000000),
                       UINT32_C(0x00001000)) ||
        range_contains(addr, width, UINT32_C(0x07000000),
                       UINT32_C(0x00004000)) ||
        range_contains(addr, width, UINT32_C(0xFFFF0000),
                       UINT32_C(0x00008000))) {
        bus_cycles = select_bus_timing(
            width, sequential, 4u, 1u, 4u, 1u);
    } else if (
        range_contains(addr, width, UINT32_C(0x05000000),
                       UINT32_C(0x02000000))) {
        bus_cycles = select_bus_timing(
            width, sequential, 5u, 2u, 4u, 1u);
    } else if (
        addr >= UINT32_C(0x08000000) &&
        addr < UINT32_C(0x0B000000)) {
        timing_unavailable(
            "ARM9 Gamecard/GBA-slot data wait states are dynamic",
            addr, width, sequential, 0u);
    } else {
        timing_unavailable(
            "runtime_mem_cycles unsupported ARM9 data region",
            addr, width, sequential, 0u);
    }
    return bus_cycles * 2u;
}

} // namespace

extern "C" void oracis_runtime_set_arm9_tcm_timing_state(
    uint32_t itcm_window_size,
    uint32_t dtcm_base,
    uint32_t dtcm_window_size,
    uint32_t itcm_load_mode,
    uint32_t dtcm_load_mode)
{
    const auto valid_window = [](uint32_t size) noexcept {
        return size == 0u ||
            (size >= UINT32_C(0x00001000) &&
             size <= UINT32_C(0x02000000) &&
             (size & (size - 1u)) == 0u);
    };
    const uint64_t dtcm_end =
        static_cast<uint64_t>(dtcm_base) + dtcm_window_size;
    if (!valid_window(itcm_window_size) ||
        !valid_window(dtcm_window_size) ||
        itcm_load_mode > 1u || dtcm_load_mode > 1u ||
        (dtcm_window_size == 0u &&
         (dtcm_base != 0u || dtcm_load_mode != 0u)) ||
        (dtcm_window_size != 0u &&
         (dtcm_base == 0u ||
          (static_cast<uint64_t>(dtcm_base) &
              (dtcm_window_size - 1u)) != 0u ||
          dtcm_end > UINT64_C(0x1'0000'0000)))) {
        timing_unavailable(
            "invalid ARM9 TCM timing state",
            dtcm_base, itcm_window_size, dtcm_window_size,
            (itcm_load_mode & 0xFFFFu) |
                ((dtcm_load_mode & 0xFFFFu) << 16u));
    }

    g_arm9_itcm_code_window_size = itcm_window_size;
    g_arm9_dtcm_base = dtcm_base;
    g_arm9_dtcm_window_size = dtcm_window_size;
    g_arm9_itcm_load_mode = itcm_load_mode != 0u;
    g_arm9_dtcm_load_mode = dtcm_load_mode != 0u;
}

extern "C" uint32_t runtime_code_cycles(uint32_t pc)
{
    if (oracis_active_cpu_state == nullptr ||
        (g_nds_active != NDS_ARM9 && g_nds_active != NDS_ARM7)) {
        timing_unavailable("runtime_code_cycles without active CPU",
                           pc, 0u, 0u, 0u);
    }

    if (g_nds_active == NDS_ARM7) {
        g_arm7_last_code_pc = pc;
        g_arm7_last_code_thumb =
            (oracis_active_cpu_state->cpsr & CPSR_T_BIT) != 0u;
        g_arm7_last_data_addr = kNoArm7TimingAddress;
        (void)arm7_timing_region(pc);
        /* The generated flat ARM7 base contains the common 1S fetch. */
        return 0u;
    }

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

static uint32_t runtime_mem_cycles_impl(
    uint32_t addr, uint32_t width, uint32_t sequential,
    bool direction_known, bool is_write)
{
    if (oracis_active_cpu_state == nullptr ||
        (g_nds_active != NDS_ARM9 && g_nds_active != NDS_ARM7)) {
        timing_unavailable(
            "runtime_mem_cycles without active CPU", addr, width,
            sequential, 0u);
    }
    if ((width != 1u && width != 2u && width != 4u) || sequential > 1u) {
        timing_unavailable(
            "runtime_mem_cycles invalid width/sequence", addr,
            width, sequential, 0u);
    }

    const uint64_t access_end = static_cast<uint64_t>(addr) + width;
    if (access_end > UINT64_C(0x1'0000'0000)) {
        timing_unavailable(
            "runtime_mem_cycles address overflow", addr,
            width, sequential, 0u);
    }

    if (g_nds_active == NDS_ARM7) {
        g_arm7_last_data_addr = addr;
    }

    if (g_nds_active == NDS_ARM9) {
        const bool in_itcm = range_contains(
            addr, width, 0u, g_arm9_itcm_code_window_size);
        const bool overlaps_itcm = range_overlaps(
            addr, width, 0u, g_arm9_itcm_code_window_size);
        if (overlaps_itcm) {
            if (g_arm9_itcm_load_mode) {
                if (!direction_known) {
                    timing_unavailable(
                        "ARM9 ITCM load-mode timing needs explicit load/store direction",
                        addr, width, sequential, 0u);
                }
                if (is_write) {
                    if (!in_itcm) {
                        timing_unavailable(
                            "ARM9 ITCM write crosses TCM timing boundary",
                            addr, width, sequential,
                            g_arm9_itcm_code_window_size);
                    }
                    /* Load mode bypasses ITCM only for reads; stores still hit TCM. */
                    return 1u;
                }
                /* A read bypasses ITCM and uses the underlying external bus. */
                return arm9_external_data_access_cycles(
                    addr, width, sequential);
            }
            if (!in_itcm) {
                timing_unavailable(
                    "ARM9 data access crosses ITCM timing boundary",
                    addr, width, sequential, g_arm9_itcm_code_window_size);
            }
            return 1u;
        }

        const bool in_dtcm = range_contains(
            addr, width, g_arm9_dtcm_base, g_arm9_dtcm_window_size);
        const bool overlaps_dtcm = range_overlaps(
            addr, width, g_arm9_dtcm_base, g_arm9_dtcm_window_size);
        if (overlaps_dtcm) {
            if (g_arm9_dtcm_load_mode) {
                if (!direction_known) {
                    timing_unavailable(
                        "ARM9 DTCM load-mode timing needs explicit load/store direction",
                        addr, width, sequential, 0u);
                }
                if (is_write) {
                    if (!in_dtcm) {
                        timing_unavailable(
                            "ARM9 DTCM write crosses TCM timing boundary",
                            addr, width, sequential, g_arm9_dtcm_window_size);
                    }
                    /* Load mode bypasses DTCM only for reads; stores still hit TCM. */
                    return 1u;
                }
                /* A read falls through to the memory region under the DTCM overlay. */
                return arm9_external_data_access_cycles(
                    addr, width, sequential);
            }
            if (!in_dtcm) {
                timing_unavailable(
                    "ARM9 data access crosses DTCM timing boundary",
                    addr, width, sequential, g_arm9_dtcm_window_size);
            }
            return 1u;
        }

        return arm9_external_data_access_cycles(addr, width, sequential);
    }

    /* NDS7/DATA uses the 33MHz ARM7 clock directly. */
    if (range_contains(
            addr, width, UINT32_C(0x02000000), UINT32_C(0x01000000))) {
        return select_bus_timing(
            width, sequential, 10u, 2u, 9u, 1u);
    }
    if (range_contains(addr, width, 0u, UINT32_C(0x00004000)) ||
        range_contains(addr, width, UINT32_C(0x03000000),
                       UINT32_C(0x01000000)) ||
        range_contains(addr, width, UINT32_C(0x04000000),
                       UINT32_C(0x00001000)) ||
        range_contains(addr, width, UINT32_C(0x07000000),
                       UINT32_C(0x00004000))) {
        return 1u;
    }
    if (range_contains(addr, width, UINT32_C(0x06000000),
                       UINT32_C(0x00040000))) {
        if (width >= 4u && sequential == 0u) {
            timing_unavailable(
                "ARM7 nonsequential 32-bit VRAM timing is not established",
                addr, width, sequential, 0u);
        }
        return select_bus_timing(
            width, sequential, 1u, 2u, 1u, 1u);
    }
    if ((addr >= UINT32_C(0x04800000) &&
         addr < UINT32_C(0x04810000)) ||
        (addr >= UINT32_C(0x08000000) &&
         addr < UINT32_C(0x0B000000))) {
        timing_unavailable(
            "ARM7 Wi-Fi/Gamecard data wait states are dynamic",
            addr, width, sequential, 0u);
    }

    timing_unavailable(
        "runtime_mem_cycles unsupported ARM7 data region",
        addr, width, sequential, 0u);
}

extern "C" uint32_t runtime_mem_cycles(
    uint32_t addr, uint32_t width, uint32_t sequential)
{
    /* Legacy callers have no direction; load-mode TCM accesses fail closed. */
    return runtime_mem_cycles_impl(
        addr, width, sequential, false, false);
}

extern "C" uint32_t runtime_mem_cycles_ex(
    uint32_t addr, uint32_t width, uint32_t sequential, uint32_t direction)
{
    if (direction != ORACIS_MEMORY_ACCESS_READ &&
        direction != ORACIS_MEMORY_ACCESS_WRITE) {
        timing_unavailable(
            "runtime_mem_cycles_ex invalid access direction",
            addr, width, sequential, direction);
    }
    return runtime_mem_cycles_impl(
        addr, width, sequential, true,
        direction == ORACIS_MEMORY_ACCESS_WRITE);
}

namespace {

uint32_t arm7_multiply_array_cycles(
    uint32_t multiplier, bool signed_variant) noexcept
{
    if (signed_variant) {
        /*
         * Signed early termination: the high bits may be all zero or all one
         * (a sign extension of the significant low byte/halfword/three bytes).
         */
        if ((multiplier & UINT32_C(0xFFFFFF00)) == 0u ||
            (multiplier & UINT32_C(0xFFFFFF00)) == UINT32_C(0xFFFFFF00)) {
            return 1u;
        }
        if ((multiplier & UINT32_C(0xFFFF0000)) == 0u ||
            (multiplier & UINT32_C(0xFFFF0000)) == UINT32_C(0xFFFF0000)) {
            return 2u;
        }
        if ((multiplier & UINT32_C(0xFF000000)) == 0u ||
            (multiplier & UINT32_C(0xFF000000)) == UINT32_C(0xFF000000)) {
            return 3u;
        }
        return 4u;
    }

    /* Unsigned early termination only removes leading zero bytes. */
    if ((multiplier & UINT32_C(0xFFFFFF00)) == 0u) return 1u;
    if ((multiplier & UINT32_C(0xFFFF0000)) == 0u) return 2u;
    if ((multiplier & UINT32_C(0xFF000000)) == 0u) return 3u;
    return 4u;
}

} // namespace

extern "C" uint32_t runtime_mul_cycles(
    uint32_t rs_value, uint32_t signed_variant, uint32_t extra)
{
    if (oracis_active_cpu_state == nullptr ||
        (g_nds_active != NDS_ARM9 && g_nds_active != NDS_ARM7)) {
        timing_unavailable(
            "runtime_mul_cycles without active CPU", 0u,
            rs_value, signed_variant, extra);
    }
    if (signed_variant > 1u || extra > 2u) {
        timing_unavailable(
            "runtime_mul_cycles invalid variant/extra", 0u,
            rs_value, signed_variant, extra);
    }

    /*
     * ARM9's multiply internal cost is a static numI term supplied to
     * arm9_cycle_combine. This dynamic ARM7TDMI helper is used only by ARM7.
     */
    if (g_nds_active == NDS_ARM9) return 0u;

    return arm7_multiply_array_cycles(
        rs_value, signed_variant != 0u) + extra;
}

namespace {

uint32_t arm7_sequential_code_fetch_cycles(
    uint32_t address, bool thumb)
{
    const Arm7TimingRegion region = arm7_timing_region(address);
    return thumb ? region.s16 : arm7_s32(region);
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
    if (oracis_active_cpu_state == nullptr || g_nds_active != NDS_ARM7) {
        timing_unavailable(
            "arm7_cycle_combine without active ARM7", 0u,
            flat_cycles, num_data, 0u);
    }
    if (is_load > 1u || has_internal > 1u) {
        timing_unavailable(
            "arm7_cycle_combine invalid flags", 0u,
            flat_cycles, num_data, is_load);
    }

    /* Zero-cost dispatcher resynchronisation, not a retired guest instruction. */
    if (flat_cycles == 0u && num_data == 0u) return 0u;

    if (g_arm7_last_code_pc == kNoArm7TimingAddress) {
        timing_unavailable(
            "arm7_cycle_combine without instruction PC", 0u,
            flat_cycles, num_data, 0u);
    }

    const Arm7TimingRegion code_region =
        arm7_timing_region(g_arm7_last_code_pc);

    if (num_data == 0u) {
        /*
         * The generated flat cost contains one 1S fetch. Replace it with S
         * for C-class instructions or N for CI-class instructions. Internal
         * cycles themselves remain inside flat_cycles.
         */
        const uint32_t sequential_code = g_arm7_last_code_thumb
            ? code_region.s16 : arm7_s32(code_region);
        const uint32_t nonsequential_code = g_arm7_last_code_thumb
            ? code_region.n16 : arm7_n32(code_region);
        const uint32_t fetch_cost =
            has_internal != 0u ? nonsequential_code : sequential_code;
        if (fetch_cost == 0u ||
            flat_cycles > UINT32_MAX - (fetch_cost - 1u)) {
            timing_unavailable(
                "arm7_cycle_combine no-data cycle overflow",
                g_arm7_last_code_pc, flat_cycles, fetch_cost, 0u);
        }
        return flat_cycles + fetch_cost - 1u;
    }

    if (g_arm7_last_data_addr == kNoArm7TimingAddress) {
        timing_unavailable(
            "arm7_cycle_combine missing data-access address",
            g_arm7_last_code_pc, flat_cycles, num_data, 0u);
    }
    (void)arm7_timing_region(g_arm7_last_data_addr);

    const uint32_t num_code = g_arm7_last_code_thumb
        ? code_region.n16 : arm7_n32(code_region);
    const bool code_main =
        g_arm7_last_code_pc >= UINT32_C(0x02000000) &&
        g_arm7_last_code_pc < UINT32_C(0x03000000);
    const bool data_main =
        g_arm7_last_data_addr >= UINT32_C(0x02000000) &&
        g_arm7_last_data_addr < UINT32_C(0x03000000);

    /*
     * Apply the ARM7 CD/CDI overlap once to all DATA accesses made by this
     * instruction. Code N/S costs come from NDS7/CODE; num_data already
     * contains the per-access NDS7/DATA costs from runtime_mem_cycles.
     */
    if (data_main) {
        if (code_main) {
            if (num_code > UINT32_MAX - num_data) {
                timing_unavailable(
                    "arm7_cycle_combine main/main overflow",
                    g_arm7_last_data_addr, num_code, num_data, 0u);
            }
            return num_code + num_data;
        }

        const uint32_t adjusted_code = num_code + is_load;
        if (adjusted_code < num_code ||
            adjusted_code > UINT32_MAX - num_data) {
            timing_unavailable(
                "arm7_cycle_combine data-main overflow",
                g_arm7_last_data_addr, num_code, num_data, is_load);
        }
        const uint32_t combined = adjusted_code + num_data;
        const uint32_t overlap = combined > 3u ? combined - 3u : 0u;
        const uint32_t floor_cost =
            adjusted_code > num_data ? adjusted_code : num_data;
        return overlap > floor_cost ? overlap : floor_cost;
    }

    if (code_main) {
        const uint32_t adjusted_data = num_data + is_load;
        if (adjusted_data < num_data ||
            num_code > UINT32_MAX - adjusted_data) {
            timing_unavailable(
                "arm7_cycle_combine code-main overflow",
                g_arm7_last_data_addr, num_code, num_data, is_load);
        }
        const uint32_t combined = num_code + adjusted_data;
        const uint32_t overlap = combined > 3u ? combined - 3u : 0u;
        const uint32_t floor_cost =
            num_code > adjusted_data ? num_code : adjusted_data;
        return overlap > floor_cost ? overlap : floor_cost;
    }

    if (num_code > UINT32_MAX - num_data ||
        num_code + num_data > UINT32_MAX - is_load) {
        timing_unavailable(
            "arm7_cycle_combine non-main overflow",
            g_arm7_last_data_addr, num_code, num_data, is_load);
    }
    return num_code + num_data + is_load;
}

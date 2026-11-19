/*
 * Retail Nintendo DS core RAM map.
 *
 * Only regions with documented retail NDS backing are installed here.
 * MMIO, BIOS, TCM, VRAM, palettes, OAM, Wi-Fi and cartridge windows are
 * separate hardware-backed regions and are intentionally not mapped as RAM.
 */
#ifndef ORACIS_MEMORY_NDS_RETAIL_MAP_HPP
#define ORACIS_MEMORY_NDS_RETAIL_MAP_HPP

#include <cstdint>

#include "oracis/memory/guest_memory.hpp"

namespace oracis::memory {

/* Values correspond to WRAMCNT bits 0-1 (register 0x04000247). */
enum class SharedWramMode : uint8_t {
    Arm9All32K = 0u,
    Arm9Bank1Arm7Bank0 = 1u,
    Arm9Bank0Arm7Bank1 = 2u,
    Arm7All32K = 3u
};

class NdsRetailRamMap final {
public:
    static constexpr uint32_t MainRamBase = UINT32_C(0x02000000);
    static constexpr uint32_t MainRamSize = UINT32_C(0x00400000);
    static constexpr uint32_t MainRamMirror1 = UINT32_C(0x02400000);
    static constexpr uint32_t MainRamMirror2 = UINT32_C(0x02800000);
    static constexpr uint32_t MainRamMirror3 = UINT32_C(0x02C00000);

    static constexpr uint32_t SharedWramBase = UINT32_C(0x03000000);
    static constexpr uint32_t SharedWramBankSize = UINT32_C(0x00004000);
    static constexpr uint32_t SharedWramSize = UINT32_C(0x00008000);
    static constexpr uint32_t Arm9SharedWramWindow = UINT32_C(0x01000000);
    static constexpr uint32_t Arm7SharedWramWindow = UINT32_C(0x00800000);

    static constexpr uint32_t Arm7WramBase = UINT32_C(0x03800000);
    static constexpr uint32_t Arm7WramSize = UINT32_C(0x00010000);

    /*
     * Install on an empty GuestMemory instance.
     *
     * main RAM is shared by ARM9/ARM7 with three 4MB mirror windows.
     * ARM7-private WRAM is visible only to ARM7.
     * All possible Shared WRAM views are installed once; only the view matching
     * the initial WRAMCNT mode is enabled for each CPU. The initial mode is
     * supplied explicitly; this builder does not guess the firmware's value.
     */
    static Status install(GuestMemory &memory, SharedWramMode shared_wram_mode);

    /*
     * Atomically switch CPU visibility among the preallocated Shared WRAM
     * views. Physical bank storage and the region set remain unchanged.
     */
    static Status set_shared_wram_mode(
        GuestMemory &memory,
        SharedWramMode shared_wram_mode);
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_RETAIL_MAP_HPP */

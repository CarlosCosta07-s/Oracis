/*
 * Verified loader for already-extracted Nintendo DS overlay images.
 *
 * The loader copies a decompressed module and zeroes its BSS in one
 * GuestMemory transaction. It never parses ROM/FAT data and never executes
 * static initializers or guest code.
 */
#ifndef ORACIS_MACHINE_NDS_OVERLAY_LOADER_HPP
#define ORACIS_MACHINE_NDS_OVERLAY_LOADER_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "oracis/memory/guest_memory.hpp"

namespace oracis::machine {

enum class NdsOverlayStatus : uint8_t {
    Ok = 0,
    AlreadyLoaded,
    NotLoaded,
    InvalidCpu,
    InvalidMetadata,
    ImageHashMismatch,
    MemoryUnavailable,
    Overlap,
    Busy,
    WriteFailed,
    IdentityConflict,
    NotInitialized,
    ProgramNotLoaded,
    InitializersAlreadyRun,
    InitializerDispatchMissing,
    InitializerBudgetExceeded,
    InitializerExecutionFailed,
    InvalidStack,
    InitializerPending
};

/*
 * Image bytes must already be decompressed to exactly ram_size bytes.
 * image_sha1 is the SHA-1 recorded for that decompressed image by the
 * local Oracis extractor. The input buffer is copied before returning.
 */
/*
 * One validated function pointer read from an overlay's static-init table.
 * address has bit 0 cleared; thumb preserves the execution state encoded by
 * that bit. The order is the order stored in the guest table.
 */
struct NdsOverlayFunctionPointer {
    uint32_t address = 0u;
    bool thumb = false;
};

struct NdsOverlayImage {
    NdsCpu cpu = NDS_ARM9;
    uint32_t overlay_id = 0u;
    uint32_t ram_address = 0u;
    uint32_t ram_size = 0u;
    uint32_t bss_size = 0u;
    uint32_t static_init_start = 0u;
    uint32_t static_init_end = 0u;
    const uint8_t *contents = nullptr;
    const char *image_sha1 = nullptr;
};

class NdsOverlayLoader final {
public:
    explicit NdsOverlayLoader(memory::GuestMemory &memory) noexcept
        : memory_(memory) {}

    NdsOverlayLoader(const NdsOverlayLoader &) = delete;
    NdsOverlayLoader &operator=(const NdsOverlayLoader &) = delete;

    NdsOverlayStatus load(const NdsOverlayImage &image) noexcept;

    /* Zero all loaded modules and discard their records at machine shutdown. */
    NdsOverlayStatus clear() noexcept;

    /*
     * no_live_guest_references is an explicit caller certificate: the loader
     * cannot inspect guest stack frames or saved return addresses. current_pc
     * is checked independently, and both CPU slices must already be stopped
     * by the owning NdsMachine.
     */
    NdsOverlayStatus unload(
        NdsCpu cpu,
        uint32_t overlay_id,
        bool no_live_guest_references,
        uint32_t current_pc) noexcept;

    bool is_loaded(NdsCpu cpu, uint32_t overlay_id) const noexcept;

    /*
     * Return the validated static-initializer sequence for a loaded overlay,
     * or nullptr when the overlay is not loaded. This describes callbacks;
     * the loader does not execute them.
     */
    const std::vector<NdsOverlayFunctionPointer> *static_initializers(
        NdsCpu cpu, uint32_t overlay_id) const noexcept;
    uint32_t static_initializer_progress(
        NdsCpu cpu, uint32_t overlay_id) const noexcept;
    NdsOverlayStatus begin_static_initializers(
        NdsCpu cpu, uint32_t overlay_id) noexcept;
    NdsOverlayStatus complete_static_initializer(
        NdsCpu cpu, uint32_t overlay_id) noexcept;
    NdsOverlayStatus fail_static_initializers(
        NdsCpu cpu, uint32_t overlay_id) noexcept;

    bool contains_address(
        NdsCpu cpu, uint32_t overlay_id, uint32_t address) const noexcept;
    std::size_t loaded_count() const noexcept { return loaded_.size(); }

private:
    struct LoadedOverlay {
        NdsCpu cpu = NDS_ARM9;
        uint32_t overlay_id = 0u;
        uint32_t ram_address = 0u;
        uint32_t ram_size = 0u;
        uint32_t bss_size = 0u;
        uint32_t static_init_start = 0u;
        uint32_t static_init_end = 0u;
        std::string image_sha1;
        std::vector<NdsOverlayFunctionPointer> static_initializers;
        uint32_t static_initializer_progress = 0u;
        bool static_initializers_running = false;
        bool static_initializers_complete = false;
        bool static_initializers_failed = false;
    };

    memory::GuestMemory &memory_;
    std::vector<LoadedOverlay> loaded_;
};

const char *overlay_status_name(NdsOverlayStatus status) noexcept;

} // namespace oracis::machine

#endif /* ORACIS_MACHINE_NDS_OVERLAY_LOADER_HPP */

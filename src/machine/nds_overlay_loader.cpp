#include "oracis/machine/nds_overlay_loader.hpp"

#include "oracis/host/detail/sha1.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace oracis::machine {
namespace {

bool valid_sha1(const char *value) noexcept
{
    if (value == nullptr || std::strlen(value) != 40u) return false;
    for (unsigned index = 0u; index < 40u; ++index) {
        const char byte = value[index];
        if (!((byte >= '0' && byte <= '9') ||
              (byte >= 'a' && byte <= 'f'))) {
            return false;
        }
    }
    return value[40] == '\0';
}

uint32_t read_u32_le(const uint8_t *bytes) noexcept
{
    return static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8u) |
        (static_cast<uint32_t>(bytes[2]) << 16u) |
        (static_cast<uint32_t>(bytes[3]) << 24u);
}

bool valid_cpu(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 || cpu == NDS_ARM7;
}

bool valid_region(NdsOverlayRegion region) noexcept
{
    return region == NdsOverlayRegion::Main ||
           region == NdsOverlayRegion::Itcm ||
           region == NdsOverlayRegion::Dtcm;
}

bool ranges_overlap(
    uint64_t left_begin, uint64_t left_end,
    uint64_t right_begin, uint64_t right_end) noexcept
{
    return left_begin < right_end && right_begin < left_end;
}

} // namespace

NdsOverlayStatus NdsOverlayLoader::load(
    const NdsOverlayImage &image) noexcept
{
    if (!valid_cpu(image.cpu)) return NdsOverlayStatus::InvalidCpu;
    if (!valid_region(image.region)) return NdsOverlayStatus::InvalidMetadata;
    if (image.contents == nullptr || !valid_sha1(image.image_sha1) ||
        image.ram_size == 0u) {
        return NdsOverlayStatus::InvalidMetadata;
    }

    const uint64_t begin = image.ram_address;
    const uint64_t code_end = begin + image.ram_size;
    const uint64_t total_size =
        static_cast<uint64_t>(image.ram_size) + image.bss_size;
    const uint64_t end = begin + total_size;
    if (total_size == 0u || total_size > UINT32_MAX ||
        code_end > UINT64_C(0x1'0000'0000) ||
        end > UINT64_C(0x1'0000'0000)) {
        return NdsOverlayStatus::InvalidMetadata;
    }

    const bool no_static_init =
        image.static_init_start == 0u && image.static_init_end == 0u;
    if (!no_static_init &&
        (image.static_init_start == 0u ||
         image.static_init_end == 0u ||
         image.static_init_start < image.ram_address ||
         image.static_init_end < image.static_init_start ||
         static_cast<uint64_t>(image.static_init_end) > code_end ||
         (image.static_init_start & 3u) != 0u ||
         (image.static_init_end & 3u) != 0u)) {
        return NdsOverlayStatus::InvalidMetadata;
    }

    uint32_t region_entry_count = 0u;
    for (const LoadedOverlay &loaded : loaded_) {
        if (loaded.cpu != image.cpu) continue;
        if (loaded.overlay_id == image.overlay_id) {
            const bool same_identity =
                loaded.region == image.region &&
                loaded.ram_address == image.ram_address &&
                loaded.ram_size == image.ram_size &&
                loaded.bss_size == image.bss_size &&
                loaded.static_init_start == image.static_init_start &&
                loaded.static_init_end == image.static_init_end &&
                loaded.image_sha1 == image.image_sha1;
            return same_identity
                ? NdsOverlayStatus::AlreadyLoaded
                : NdsOverlayStatus::IdentityConflict;
        }
        if (loaded.region != image.region) continue;
        ++region_entry_count;
        const uint64_t loaded_begin = loaded.ram_address;
        const uint64_t loaded_end = loaded_begin +
            static_cast<uint64_t>(loaded.ram_size) + loaded.bss_size;
        if (ranges_overlap(begin, end, loaded_begin, loaded_end)) {
            return NdsOverlayStatus::Overlap;
        }
    }
    if (region_entry_count >= MaxOverlaysPerRegion) {
        return NdsOverlayStatus::RegionCapacityExceeded;
    }

    const uint32_t writable_size = static_cast<uint32_t>(total_size);
    if (memory_.validate_write(
            image.cpu, image.ram_address, writable_size) !=
            memory::Status::Ok ||
        (image.region != NdsOverlayRegion::Dtcm &&
         memory_.validate_execute(
             image.cpu, image.ram_address, image.ram_size) !=
             memory::Status::Ok)) {
        return NdsOverlayStatus::MemoryUnavailable;
    }

    try {
        std::vector<uint8_t> source(
            image.contents, image.contents + image.ram_size);
        if (oracis::host::detail::sha1_hex(source) != image.image_sha1) {
            return NdsOverlayStatus::ImageHashMismatch;
        }

        /*
         * Decode the overlay table's static-initializer function pointers
         * only after the complete image identity has been verified. A zero
         * word terminates the pointer sequence. Preserve table order because
         * constructor order is guest metadata, not an address sort.
         */
        std::vector<NdsOverlayFunctionPointer> static_initializers;
        if (!no_static_init) {
            const std::size_t table_offset =
                static_cast<std::size_t>(image.static_init_start - image.ram_address);
            const std::size_t table_size =
                static_cast<std::size_t>(image.static_init_end - image.static_init_start);
            if ((table_size & 3u) != 0u ||
                table_offset > source.size() ||
                table_size > source.size() - table_offset) {
                return NdsOverlayStatus::InvalidMetadata;
            }

            for (std::size_t offset = 0u; offset < table_size; offset += 4u) {
                const uint32_t pointer = read_u32_le(
                    source.data() + table_offset + offset);
                if (pointer == 0u) break;

                const bool thumb = (pointer & 1u) != 0u;
                const uint32_t address = pointer & ~UINT32_C(1);
                const uint32_t alignment = thumb ? 2u : 4u;
                const uint64_t instruction_size = thumb ? 2u : 4u;
                const uint64_t instruction_end =
                    static_cast<uint64_t>(address) + instruction_size;
                const bool local_code_target =
                    image.region != NdsOverlayRegion::Dtcm;
                if ((local_code_target &&
                     (static_cast<uint64_t>(address) < begin ||
                      instruction_end > code_end)) ||
                    (address % alignment) != 0u ||
                    instruction_end > UINT64_C(0x1'0000'0000)) {
                    return NdsOverlayStatus::InvalidMetadata;
                }
                static_initializers.push_back({address, thumb});
            }
        }

        const bool has_static_initializers = !static_initializers.empty();

        /* Prepare the complete RAM+BSS payload before touching guest memory. */
        std::vector<uint8_t> staged(writable_size, 0u);
        std::copy(source.begin(), source.end(), staged.begin());

        /* Reserve before the write so publishing the record cannot allocate. */
        loaded_.reserve(loaded_.size() + 1u);
        LoadedOverlay record{
            image.cpu,
            image.region,
            image.overlay_id,
            image.ram_address,
            image.ram_size,
            image.bss_size,
            image.static_init_start,
            image.static_init_end,
            image.image_sha1,
            std::move(static_initializers),
            0u,
            false,
            !has_static_initializers,
            false
        };

        if (memory_.write_bytes(
                image.cpu, image.ram_address, staged.data(), writable_size) !=
            memory::Status::Ok) {
            return NdsOverlayStatus::WriteFailed;
        }
        if (memory_.validate_identity(
                image.cpu, image.ram_address, image.ram_size,
                source.data()) != memory::Status::Ok) {
            /* The validated one-range write is atomic; this indicates a
               concurrent memory mutation, so do not publish a false record. */
            return NdsOverlayStatus::WriteFailed;
        }

        loaded_.push_back(std::move(record));
        return NdsOverlayStatus::Ok;
    } catch (const std::bad_alloc &) {
        return NdsOverlayStatus::WriteFailed;
    } catch (...) {
        return NdsOverlayStatus::WriteFailed;
    }
}

NdsOverlayStatus NdsOverlayLoader::clear() noexcept
{
    if (loaded_.empty()) return NdsOverlayStatus::Ok;

    uint32_t maximum_size = 0u;
    for (const LoadedOverlay &item : loaded_) {
        const uint64_t total =
            static_cast<uint64_t>(item.ram_size) + item.bss_size;
        if (total == 0u || total > UINT32_MAX ||
            memory_.validate_write(
                item.cpu, item.ram_address,
                static_cast<uint32_t>(total)) != memory::Status::Ok) {
            return NdsOverlayStatus::MemoryUnavailable;
        }
        if (total > maximum_size) {
            maximum_size = static_cast<uint32_t>(total);
        }
    }

    try {
        const std::vector<uint8_t> zeros(maximum_size, 0u);
        while (!loaded_.empty()) {
            const LoadedOverlay &item = loaded_.back();
            const uint32_t total_size = item.ram_size + item.bss_size;
            if (memory_.write_bytes(
                    item.cpu, item.ram_address,
                    zeros.data(), total_size) != memory::Status::Ok) {
                return NdsOverlayStatus::WriteFailed;
            }
            loaded_.pop_back();
        }
        return NdsOverlayStatus::Ok;
    } catch (const std::bad_alloc &) {
        return NdsOverlayStatus::WriteFailed;
    } catch (...) {
        return NdsOverlayStatus::WriteFailed;
    }
}

NdsOverlayStatus NdsOverlayLoader::unload(
    NdsCpu cpu,
    uint32_t overlay_id,
    bool no_live_guest_references,
    uint32_t current_pc) noexcept
{
    if (!valid_cpu(cpu)) return NdsOverlayStatus::InvalidCpu;
    auto found = std::find_if(
        loaded_.begin(), loaded_.end(),
        [cpu, overlay_id](const LoadedOverlay &item) {
            return item.cpu == cpu && item.overlay_id == overlay_id;
        });
    if (found == loaded_.end()) return NdsOverlayStatus::NotLoaded;

    const uint64_t begin = found->ram_address;
    const uint64_t end = begin +
        static_cast<uint64_t>(found->ram_size) + found->bss_size;
    if (!no_live_guest_references ||
        (static_cast<uint64_t>(current_pc) >= begin &&
         static_cast<uint64_t>(current_pc) < end)) {
        return NdsOverlayStatus::Busy;
    }

    const uint32_t total_size = found->ram_size + found->bss_size;
    if (memory_.validate_write(cpu, found->ram_address, total_size) !=
        memory::Status::Ok) {
        return NdsOverlayStatus::MemoryUnavailable;
    }

    try {
        const std::vector<uint8_t> cleared(total_size, 0u);
        if (memory_.write_bytes(
                cpu, found->ram_address, cleared.data(), total_size) !=
            memory::Status::Ok) {
            return NdsOverlayStatus::WriteFailed;
        }
        loaded_.erase(found);
        return NdsOverlayStatus::Ok;
    } catch (const std::bad_alloc &) {
        return NdsOverlayStatus::WriteFailed;
    } catch (...) {
        return NdsOverlayStatus::WriteFailed;
    }
}

bool NdsOverlayLoader::is_loaded(
    NdsCpu cpu, uint32_t overlay_id) const noexcept
{
    return std::any_of(
        loaded_.begin(), loaded_.end(),
        [cpu, overlay_id](const LoadedOverlay &item) {
            return item.cpu == cpu && item.overlay_id == overlay_id;
        });
}

const std::vector<NdsOverlayFunctionPointer> *
NdsOverlayLoader::static_initializers(
    NdsCpu cpu, uint32_t overlay_id) const noexcept
{
    const auto found = std::find_if(
        loaded_.begin(), loaded_.end(),
        [cpu, overlay_id](const LoadedOverlay &item) {
            return item.cpu == cpu && item.overlay_id == overlay_id;
        });
    return found == loaded_.end() ? nullptr : &found->static_initializers;
}

bool NdsOverlayLoader::static_initializers_started(
    NdsCpu cpu, uint32_t overlay_id) const noexcept
{
    const auto found = std::find_if(
        loaded_.begin(), loaded_.end(),
        [cpu, overlay_id](const LoadedOverlay &item) {
            return item.cpu == cpu && item.overlay_id == overlay_id;
        });
    return found != loaded_.end() &&
        (found->static_initializers_running ||
         found->static_initializers_complete ||
         found->static_initializers_failed);
}

uint32_t NdsOverlayLoader::static_initializer_progress(
    NdsCpu cpu, uint32_t overlay_id) const noexcept
{
    const auto found = std::find_if(
        loaded_.begin(), loaded_.end(),
        [cpu, overlay_id](const LoadedOverlay &item) {
            return item.cpu == cpu && item.overlay_id == overlay_id;
        });
    return found == loaded_.end() ? 0u : found->static_initializer_progress;
}

NdsOverlayStatus NdsOverlayLoader::begin_static_initializers(
    NdsCpu cpu, uint32_t overlay_id) noexcept
{
    if (!valid_cpu(cpu)) return NdsOverlayStatus::InvalidCpu;
    const auto found = std::find_if(
        loaded_.begin(), loaded_.end(),
        [cpu, overlay_id](const LoadedOverlay &item) {
            return item.cpu == cpu && item.overlay_id == overlay_id;
        });
    if (found == loaded_.end()) return NdsOverlayStatus::NotLoaded;
    if (found->static_initializers_failed) {
        return NdsOverlayStatus::InitializerExecutionFailed;
    }
    if (found->static_initializers_complete) {
        return NdsOverlayStatus::InitializersAlreadyRun;
    }
    if (found->static_initializers_running ||
        found->static_initializer_progress != 0u) {
        return NdsOverlayStatus::Busy;
    }
    found->static_initializers_running = true;
    return NdsOverlayStatus::Ok;
}

NdsOverlayStatus NdsOverlayLoader::complete_static_initializer(
    NdsCpu cpu, uint32_t overlay_id) noexcept
{
    if (!valid_cpu(cpu)) return NdsOverlayStatus::InvalidCpu;
    const auto found = std::find_if(
        loaded_.begin(), loaded_.end(),
        [cpu, overlay_id](const LoadedOverlay &item) {
            return item.cpu == cpu && item.overlay_id == overlay_id;
        });
    if (found == loaded_.end()) return NdsOverlayStatus::NotLoaded;
    if (!found->static_initializers_running ||
        found->static_initializers_failed ||
        found->static_initializer_progress >= found->static_initializers.size()) {
        return NdsOverlayStatus::InvalidMetadata;
    }

    ++found->static_initializer_progress;
    if (found->static_initializer_progress ==
        found->static_initializers.size()) {
        found->static_initializers_running = false;
        found->static_initializers_complete = true;
    }
    return NdsOverlayStatus::Ok;
}

NdsOverlayStatus NdsOverlayLoader::fail_static_initializers(
    NdsCpu cpu, uint32_t overlay_id) noexcept
{
    if (!valid_cpu(cpu)) return NdsOverlayStatus::InvalidCpu;
    const auto found = std::find_if(
        loaded_.begin(), loaded_.end(),
        [cpu, overlay_id](const LoadedOverlay &item) {
            return item.cpu == cpu && item.overlay_id == overlay_id;
        });
    if (found == loaded_.end()) return NdsOverlayStatus::NotLoaded;
    found->static_initializers_running = false;
    found->static_initializers_failed = true;
    return NdsOverlayStatus::InitializerExecutionFailed;
}

bool NdsOverlayLoader::contains_address(
    NdsCpu cpu, uint32_t overlay_id, uint32_t address) const noexcept
{
    const auto found = std::find_if(
        loaded_.begin(), loaded_.end(),
        [cpu, overlay_id](const LoadedOverlay &item) {
            return item.cpu == cpu && item.overlay_id == overlay_id;
        });
    if (found == loaded_.end()) return false;
    const uint64_t begin = found->ram_address;
    const uint64_t end = begin +
        static_cast<uint64_t>(found->ram_size) + found->bss_size;
    return static_cast<uint64_t>(address) >= begin &&
           static_cast<uint64_t>(address) < end;
}

const char *overlay_status_name(NdsOverlayStatus status) noexcept
{
    switch (status) {
    case NdsOverlayStatus::Ok: return "ok";
    case NdsOverlayStatus::AlreadyLoaded: return "already-loaded";
    case NdsOverlayStatus::NotLoaded: return "not-loaded";
    case NdsOverlayStatus::InvalidCpu: return "invalid-cpu";
    case NdsOverlayStatus::InvalidMetadata: return "invalid-metadata";
    case NdsOverlayStatus::ImageHashMismatch: return "image-hash-mismatch";
    case NdsOverlayStatus::MemoryUnavailable: return "memory-unavailable";
    case NdsOverlayStatus::Overlap: return "overlap";
    case NdsOverlayStatus::Busy: return "busy";
    case NdsOverlayStatus::WriteFailed: return "write-failed";
    case NdsOverlayStatus::IdentityConflict: return "identity-conflict";
    case NdsOverlayStatus::NotInitialized: return "not-initialized";
    case NdsOverlayStatus::ProgramNotLoaded: return "program-not-loaded";
    case NdsOverlayStatus::InitializersAlreadyRun: return "initializers-already-run";
    case NdsOverlayStatus::InitializerDispatchMissing: return "initializer-dispatch-missing";
    case NdsOverlayStatus::InitializerBudgetExceeded: return "initializer-budget-exceeded";
    case NdsOverlayStatus::InitializerExecutionFailed: return "initializer-execution-failed";
    case NdsOverlayStatus::InvalidStack: return "invalid-stack";
    case NdsOverlayStatus::InitializerPending: return "initializer-pending";
    case NdsOverlayStatus::RegionCapacityExceeded: return "region-capacity-exceeded";
    case NdsOverlayStatus::FinalizersUnavailable: return "finalizers-unavailable";
    }
    return "unknown";
}

} // namespace oracis::machine

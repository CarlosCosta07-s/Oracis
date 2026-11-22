/*
 * Nintendo DS 3D-to-2D scanline FIFO.
 *
 * The 3D renderer produces BG0-ready RGB555/alpha5 lines. Engine A's 2D
 * compositor consumes them in display order from a 48-line ring buffer.
 */
#ifndef ORACIS_MEMORY_NDS_3D_SCANLINE_CACHE_HPP
#define ORACIS_MEMORY_NDS_3D_SCANLINE_CACHE_HPP

#include <array>
#include <cstddef>
#include <cstdint>

namespace oracis::memory {

struct Nds3dBg0Pixel {
    uint16_t rgb555 = 0u;
    uint8_t alpha5 = 0u;
};

using Nds3dBg0Scanline = std::array<Nds3dBg0Pixel, 256>;

enum class Nds3dScanlineCacheStatus : uint8_t {
    Ok = 0,
    Full,
    Empty,
    InvalidScanline,
    UnexpectedScanline,
    InvalidPixel,
    InvalidOutput
};

/* Single-owner queue; reset once at frame start, then push/pop in line order. */
class Nds3dScanlineCache final {
public:
    static constexpr uint16_t ScreenHeight = 192u;
    static constexpr std::size_t Capacity = 48u;

    Nds3dScanlineCache() noexcept = default;
    Nds3dScanlineCache(const Nds3dScanlineCache &) = delete;
    Nds3dScanlineCache &operator=(const Nds3dScanlineCache &) = delete;

    Nds3dScanlineCacheStatus reset(
        uint16_t first_scanline = 0u) noexcept;
    Nds3dScanlineCacheStatus can_enqueue(
        uint16_t scanline) const noexcept;
    Nds3dScanlineCacheStatus enqueue(
        uint16_t scanline, const Nds3dBg0Scanline &pixels) noexcept;
    Nds3dScanlineCacheStatus peek(
        uint16_t expected_scanline,
        const Nds3dBg0Scanline **pixels) const noexcept;
    Nds3dScanlineCacheStatus consume(
        uint16_t expected_scanline) noexcept;

    std::size_t queued_lines() const noexcept { return count_; }
    std::size_t free_lines() const noexcept { return Capacity - count_; }
    bool empty() const noexcept { return count_ == 0u; }
    bool full() const noexcept { return count_ == Capacity; }
    uint16_t next_producer_scanline() const noexcept {
        return next_producer_scanline_;
    }
    uint16_t next_consumer_scanline() const noexcept {
        return next_consumer_scanline_;
    }

private:
    struct Entry {
        uint16_t scanline = 0u;
        Nds3dBg0Scanline pixels{};
    };
    static bool valid_line(const Nds3dBg0Scanline &pixels) noexcept;

    std::array<Entry, Capacity> entries_{};
    std::size_t head_ = 0u;
    std::size_t count_ = 0u;
    uint16_t next_producer_scanline_ = 0u;
    uint16_t next_consumer_scanline_ = 0u;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_3D_SCANLINE_CACHE_HPP */

#include "oracis/memory/nds_3d_scanline_cache.hpp"

namespace oracis::memory {

Nds3dScanlineCacheStatus Nds3dScanlineCache::reset(
    uint16_t first_scanline) noexcept
{
    if (first_scanline >= ScreenHeight) {
        return Nds3dScanlineCacheStatus::InvalidScanline;
    }
    head_ = 0u;
    count_ = 0u;
    next_producer_scanline_ = first_scanline;
    next_consumer_scanline_ = first_scanline;
    minimum_queued_lines_ = Capacity;
    low_water_tracking_ = false;
    return Nds3dScanlineCacheStatus::Ok;
}

Nds3dScanlineCacheStatus Nds3dScanlineCache::can_enqueue(
    uint16_t scanline) const noexcept
{
    if (scanline >= ScreenHeight) return Nds3dScanlineCacheStatus::InvalidScanline;
    if (count_ == Capacity) return Nds3dScanlineCacheStatus::Full;
    if (scanline != next_producer_scanline_) {
        return Nds3dScanlineCacheStatus::UnexpectedScanline;
    }
    return Nds3dScanlineCacheStatus::Ok;
}

bool Nds3dScanlineCache::valid_line(
    const Nds3dBg0Scanline &pixels) noexcept
{
    for (const Nds3dBg0Pixel &pixel : pixels) {
        if (pixel.rgb555 > UINT16_C(0x7FFF) || pixel.alpha5 > 31u) {
            return false;
        }
    }
    return true;
}

Nds3dScanlineCacheStatus Nds3dScanlineCache::enqueue(
    uint16_t scanline,
    const Nds3dBg0Scanline &pixels) noexcept
{
    const Nds3dScanlineCacheStatus available = can_enqueue(scanline);
    if (available != Nds3dScanlineCacheStatus::Ok) return available;
    if (!valid_line(pixels)) return Nds3dScanlineCacheStatus::InvalidPixel;
    const std::size_t tail = (head_ + count_) % Capacity;
    entries_[tail].scanline = scanline;
    entries_[tail].pixels = pixels;
    ++count_;
    ++next_producer_scanline_;
    return Nds3dScanlineCacheStatus::Ok;
}

Nds3dScanlineCacheStatus Nds3dScanlineCache::peek(
    uint16_t expected_scanline,
    const Nds3dBg0Scanline **pixels) const noexcept
{
    if (pixels == nullptr) return Nds3dScanlineCacheStatus::InvalidOutput;
    *pixels = nullptr;
    if (expected_scanline >= ScreenHeight) {
        return Nds3dScanlineCacheStatus::InvalidScanline;
    }
    if (count_ == 0u) return Nds3dScanlineCacheStatus::Empty;
    if (expected_scanline != next_consumer_scanline_ ||
        entries_[head_].scanline != expected_scanline) {
        return Nds3dScanlineCacheStatus::UnexpectedScanline;
    }
    *pixels = &entries_[head_].pixels;
    return Nds3dScanlineCacheStatus::Ok;
}

Nds3dScanlineCacheStatus Nds3dScanlineCache::consume(
    uint16_t expected_scanline) noexcept
{
    const Nds3dBg0Scanline *unused = nullptr;
    const Nds3dScanlineCacheStatus available = peek(expected_scanline, &unused);
    if (available != Nds3dScanlineCacheStatus::Ok) return available;
    head_ = (head_ + 1u) % Capacity;
    --count_;
    ++next_consumer_scanline_;
    if (low_water_tracking_ && count_ < minimum_queued_lines_) {
        minimum_queued_lines_ = count_;
    }
    return Nds3dScanlineCacheStatus::Ok;
}

void Nds3dScanlineCache::begin_low_water_tracking() noexcept
{
    minimum_queued_lines_ = count_;
    low_water_tracking_ = true;
}

bool Nds3dScanlineCache::finish_low_water_tracking(
    std::size_t &minimum_queued_lines) noexcept
{
    if (!low_water_tracking_) return false;
    minimum_queued_lines = minimum_queued_lines_;
    low_water_tracking_ = false;
    return true;
}

} // namespace oracis::memory

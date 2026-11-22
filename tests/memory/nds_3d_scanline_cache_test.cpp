#include "oracis/memory/nds_3d_scanline_cache.hpp"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_3d_scanline_cache_test: %s\n", message);
    return 1;
}

oracis::memory::Nds3dBg0Scanline make_line(uint16_t marker)
{
    oracis::memory::Nds3dBg0Scanline line{};
    for (std::size_t x = 0u; x < line.size(); ++x) {
        line[x].rgb555 = static_cast<uint16_t>(
            (marker + static_cast<uint16_t>(x)) & UINT16_C(0x7FFF));
        line[x].alpha5 = static_cast<uint8_t>(marker & UINT16_C(0x1F));
    }
    return line;
}

} // namespace

int main()
{
    using namespace oracis::memory;
    Nds3dScanlineCache cache;
    if (cache.reset() != Nds3dScanlineCacheStatus::Ok ||
        cache.queued_lines() != 0u ||
        cache.free_lines() != Nds3dScanlineCache::Capacity) {
        return fail("48-line FIFO reset or capacity was incorrect");
    }

    Nds3dBg0Scanline invalid = make_line(1u);
    invalid[17].alpha5 = 32u;
    if (cache.enqueue(0u, invalid) != Nds3dScanlineCacheStatus::InvalidPixel ||
        !cache.empty() || cache.next_producer_scanline() != 0u) {
        return fail("invalid BG0 alpha partially entered the FIFO");
    }

    for (uint16_t scanline = 0u; scanline < Nds3dScanlineCache::Capacity; ++scanline) {
        if (cache.enqueue(scanline, make_line(scanline)) !=
            Nds3dScanlineCacheStatus::Ok) {
            return fail("sequential 48-line FIFO fill failed");
        }
    }
    if (!cache.full() || cache.queued_lines() != 48u ||
        cache.free_lines() != 0u ||
        cache.enqueue(48u, make_line(48u)) != Nds3dScanlineCacheStatus::Full) {
        return fail("FIFO did not enforce its 48-scanline capacity");
    }

    const Nds3dBg0Scanline *line = nullptr;
    if (cache.peek(1u, &line) != Nds3dScanlineCacheStatus::UnexpectedScanline ||
        line != nullptr ||
        cache.consume(1u) != Nds3dScanlineCacheStatus::UnexpectedScanline ||
        cache.queued_lines() != 48u) {
        return fail("out-of-order consumer request changed the FIFO");
    }
    if (cache.peek(0u, &line) != Nds3dScanlineCacheStatus::Ok ||
        line == nullptr || (*line)[127].rgb555 != UINT16_C(127) ||
        cache.consume(0u) != Nds3dScanlineCacheStatus::Ok ||
        cache.queued_lines() != 47u || cache.free_lines() != 1u) {
        return fail("FIFO head could not be inspected and consumed");
    }
    if (cache.enqueue(48u, make_line(48u)) != Nds3dScanlineCacheStatus::Ok) {
        return fail("FIFO did not accept a new line after one pop");
    }
    for (uint16_t scanline = 1u; scanline <= 48u; ++scanline) {
        if (cache.peek(scanline, &line) != Nds3dScanlineCacheStatus::Ok ||
            line == nullptr || (*line)[0].rgb555 != scanline ||
            cache.consume(scanline) != Nds3dScanlineCacheStatus::Ok) {
            return fail("FIFO ring wrap or strict scanline order failed");
        }
    }
    if (!cache.empty() ||
        cache.peek(49u, &line) != Nds3dScanlineCacheStatus::Empty) {
        return fail("FIFO did not report an empty queue");
    }
    if (cache.reset(191u) != Nds3dScanlineCacheStatus::Ok ||
        cache.enqueue(191u, make_line(31u)) != Nds3dScanlineCacheStatus::Ok ||
        cache.consume(191u) != Nds3dScanlineCacheStatus::Ok ||
        cache.enqueue(0u, make_line(0u)) != Nds3dScanlineCacheStatus::UnexpectedScanline ||
        cache.reset(192u) != Nds3dScanlineCacheStatus::InvalidScanline) {
        return fail("frame-boundary scanline policy was not enforced");
    }

    std::puts("NDS 3D 48-line scanline FIFO tests passed");
    return 0;
}

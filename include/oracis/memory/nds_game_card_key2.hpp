/*
 * Nintendo DS KEY2 byte-stream cipher.
 *
 * This primitive accepts already-normalized 39-bit seeds. Seed derivation
 * from RTC/header fields is intentionally outside this class.
 */
#ifndef ORACIS_MEMORY_NDS_GAME_CARD_KEY2_HPP
#define ORACIS_MEMORY_NDS_GAME_CARD_KEY2_HPP

#include <cstdint>

#include "oracis/memory/guest_memory.hpp"

namespace oracis::memory {

class NdsGameCardKey2 final {
public:
    static constexpr uint64_t SeedMask = UINT64_C(0x7FFFFFFFFF);
    static constexpr uint32_t SeedBits = 39u;

    NdsGameCardKey2() noexcept = default;

    /*
     * Install two normalized 39-bit LFSR states. Values above SeedMask are
     * rejected without changing the current stream state.
     */
    Status set_seeds(uint64_t seed1, uint64_t seed2) noexcept;

    /*
     * XOR the KEY2 keystream into bytes in-place. Consecutive calls continue
     * the same stream, including across 32-bit word boundaries.
     */
    Status apply_keystream(uint8_t *bytes, uint32_t size) noexcept;

    bool initialized() const noexcept { return initialized_; }
    uint64_t state1() const noexcept { return state1_; }
    uint64_t state2() const noexcept { return state2_; }
    uint64_t bytes_processed() const noexcept { return bytes_processed_; }

private:
    uint8_t next_keystream_byte() noexcept;

    uint64_t state1_ = 0u;
    uint64_t state2_ = 0u;
    uint64_t bytes_processed_ = 0u;
    bool initialized_ = false;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_GAME_CARD_KEY2_HPP */

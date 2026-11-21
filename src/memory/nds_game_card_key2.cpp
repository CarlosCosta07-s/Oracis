#include "oracis/memory/nds_game_card_key2.hpp"

namespace oracis::memory {

Status NdsGameCardKey2::set_seeds(
    uint64_t seed1, uint64_t seed2) noexcept
{
    if (seed1 > SeedMask || seed2 > SeedMask) {
        return Status::InvalidRegion;
    }

    state1_ = seed1;
    state2_ = seed2;
    bytes_processed_ = 0u;
    initialized_ = true;
    return Status::Ok;
}

uint8_t NdsGameCardKey2::next_keystream_byte() noexcept
{
    /*
     * Each 39-bit state shifts by one byte and inserts an 8-bit feedback
     * value formed from its documented taps. Truncation keeps the state
     * within the 39-bit register width.
     */
    const uint64_t feedback1 =
        ((state1_ >> 5u) ^ (state1_ >> 17u) ^
         (state1_ >> 18u) ^ (state1_ >> 31u)) & UINT64_C(0xFF);
    const uint64_t feedback2 =
        ((state2_ >> 5u) ^ (state2_ >> 23u) ^
         (state2_ >> 18u) ^ (state2_ >> 31u)) & UINT64_C(0xFF);

    state1_ = ((state1_ << 8u) + feedback1) & SeedMask;
    state2_ = ((state2_ << 8u) + feedback2) & SeedMask;

    return static_cast<uint8_t>((state1_ ^ state2_) & UINT64_C(0xFF));
}

Status NdsGameCardKey2::apply_keystream(
    uint8_t *bytes, uint32_t size) noexcept
{
    if (size == 0u) return Status::Ok;
    if (bytes == nullptr) return Status::InvalidRegion;
    if (!initialized_) return Status::PermissionDenied;

    for (uint32_t index = 0u; index < size; ++index) {
        bytes[index] ^= next_keystream_byte();
    }
    bytes_processed_ += size;
    return Status::Ok;
}

} // namespace oracis::memory

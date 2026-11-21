#include "oracis/memory/nds_game_card_key2.hpp"

namespace oracis::memory {
namespace {

constexpr uint8_t kHeaderSeedBytes[8] = {
    UINT8_C(0xE8), UINT8_C(0x4D), UINT8_C(0x5A), UINT8_C(0xB1),
    UINT8_C(0x17), UINT8_C(0x8F), UINT8_C(0x99), UINT8_C(0xD5)
};
constexpr uint64_t kSecondRawSeed = UINT64_C(0x5C879B9B05);

uint64_t reverse_seed_bits(uint64_t value) noexcept
{
    uint64_t reversed = 0u;
    for (uint32_t bit = 0u; bit < NdsGameCardKey2::SeedBits; ++bit) {
        reversed |= ((value >> bit) & UINT64_C(1)) <<
            (NdsGameCardKey2::SeedBits - 1u - bit);
    }
    return reversed;
}

} // namespace

Status NdsGameCardKey2::initialize_from_random_seed(
    uint32_t random_value24, uint8_t seed_index) noexcept
{
    if (random_value24 > UINT32_C(0x00FFFFFF) || seed_index >= 8u) {
        return Status::InvalidRegion;
    }

    const uint64_t raw_seed1 =
        ((static_cast<uint64_t>(random_value24) << 15u) +
         UINT64_C(0x6000) + kHeaderSeedBytes[seed_index]) & SeedMask;
    const uint64_t raw_seed2 = kSecondRawSeed & SeedMask;
    return set_seeds(
        reverse_seed_bits(raw_seed1), reverse_seed_bits(raw_seed2));
}

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

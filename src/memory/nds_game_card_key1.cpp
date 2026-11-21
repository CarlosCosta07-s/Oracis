#include "oracis/memory/nds_game_card_key1.hpp"

namespace oracis::memory {

uint32_t NdsGameCardKey1::byte_swap32(uint32_t value) noexcept
{
    return ((value & UINT32_C(0x000000FF)) << 24u) |
        ((value & UINT32_C(0x0000FF00)) << 8u) |
        ((value & UINT32_C(0x00FF0000)) >> 8u) |
        ((value & UINT32_C(0xFF000000)) >> 24u);
}

Status NdsGameCardKey1::load_key_table(
    const uint8_t *bytes, uint32_t size) noexcept
{
    if (bytes == nullptr || size != KeyTableBytes) {
        return Status::InvalidRegion;
    }

    std::array<uint32_t, KeyTableWords> candidate{};
    for (uint32_t index = 0u; index < KeyTableWords; ++index) {
        const uint32_t offset = index * 4u;
        candidate[index] =
            static_cast<uint32_t>(bytes[offset]) |
            (static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
            (static_cast<uint32_t>(bytes[offset + 2u]) << 16u) |
            (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
    }

    base_table_ = candidate;
    active_table_ = {};
    id_code_ = 0u;
    modulo_bytes_ = 0u;
    level_ = 0u;
    table_loaded_ = true;
    initialized_ = false;
    return Status::Ok;
}

uint32_t NdsGameCardKey1::round_function(uint32_t value) const noexcept
{
    const uint32_t s0 = active_table_[0x012u + ((value >> 24u) & 0xFFu)];
    const uint32_t s1 = active_table_[0x112u + ((value >> 16u) & 0xFFu)];
    const uint32_t s2 = active_table_[0x212u + ((value >> 8u) & 0xFFu)];
    const uint32_t s3 = active_table_[0x312u + (value & 0xFFu)];
    return ((s0 + s1) ^ s2) + s3;
}

void NdsGameCardKey1::encrypt_words(
    uint32_t &first, uint32_t &second) const noexcept
{
    uint32_t y = first;
    uint32_t x = second;

    for (uint32_t round = 0u; round < 16u; ++round) {
        const uint32_t z = active_table_[round] ^ x;
        x = y ^ round_function(z);
        y = z;
    }

    first = x ^ active_table_[16u];
    second = y ^ active_table_[17u];
}

void NdsGameCardKey1::decrypt_words(
    uint32_t &first, uint32_t &second) const noexcept
{
    uint32_t y = first;
    uint32_t x = second;

    for (uint32_t round = 18u; round-- > 2u;) {
        const uint32_t z = active_table_[round] ^ x;
        x = y ^ round_function(z);
        y = z;
    }

    first = x ^ active_table_[1u];
    second = y ^ active_table_[0u];
}

void NdsGameCardKey1::apply_key_code(
    std::array<uint32_t, 3> &key_code) noexcept
{
    /* Apply the current key to keycode[1..2], then keycode[0..1]. */
    encrypt_words(key_code[1], key_code[2]);
    encrypt_words(key_code[0], key_code[1]);

    /* The NDS key schedule XORs the P-array with byte-swapped keycode words. */
    for (uint32_t byte_offset = 0u; byte_offset < 0x48u; byte_offset += 4u) {
        const uint32_t key_code_index =
            (byte_offset % modulo_bytes_) / 4u;
        active_table_[byte_offset / 4u] ^=
            byte_swap32(key_code[key_code_index]);
    }

    /* Expand all P/S words by repeatedly encrypting the zero 64-bit state. */
    uint32_t scratch_first = 0u;
    uint32_t scratch_second = 0u;
    for (uint32_t word = 0u; word < KeyTableWords; word += 2u) {
        encrypt_words(scratch_first, scratch_second);
        active_table_[word] = scratch_second;
        active_table_[word + 1u] = scratch_first;
    }
}

Status NdsGameCardKey1::initialize(
    uint32_t id_code, uint32_t level,
    uint32_t modulo_bytes) noexcept
{
    if (!table_loaded_) return Status::Unmapped;
    if (level < 1u || level > 3u ||
        (modulo_bytes != 8u && modulo_bytes != 12u)) {
        return Status::InvalidRegion;
    }

    active_table_ = base_table_;
    std::array<uint32_t, 3> key_code{
        id_code,
        id_code >> 1u,
        id_code << 1u
    };

    modulo_bytes_ = modulo_bytes;
    apply_key_code(key_code);
    if (level >= 2u) apply_key_code(key_code);

    key_code[1] <<= 1u;
    key_code[2] >>= 1u;
    if (level >= 3u) apply_key_code(key_code);

    id_code_ = id_code;
    level_ = level;
    initialized_ = true;
    return Status::Ok;
}

Status NdsGameCardKey1::encrypt_block(
    uint8_t *bytes, uint32_t size) const noexcept
{
    if (bytes == nullptr) return Status::InvalidRegion;
    if (size != BlockBytes) return Status::InvalidWidth;
    if (!initialized_) return Status::PermissionDenied;

    uint32_t first =
        static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8u) |
        (static_cast<uint32_t>(bytes[2]) << 16u) |
        (static_cast<uint32_t>(bytes[3]) << 24u);
    uint32_t second =
        static_cast<uint32_t>(bytes[4]) |
        (static_cast<uint32_t>(bytes[5]) << 8u) |
        (static_cast<uint32_t>(bytes[6]) << 16u) |
        (static_cast<uint32_t>(bytes[7]) << 24u);

    encrypt_words(first, second);

    for (uint32_t index = 0u; index < 4u; ++index) {
        bytes[index] = static_cast<uint8_t>(first >> (index * 8u));
        bytes[index + 4u] =
            static_cast<uint8_t>(second >> (index * 8u));
    }
    return Status::Ok;
}

Status NdsGameCardKey1::decrypt_block(
    uint8_t *bytes, uint32_t size) const noexcept
{
    if (bytes == nullptr) return Status::InvalidRegion;
    if (size != BlockBytes) return Status::InvalidWidth;
    if (!initialized_) return Status::PermissionDenied;

    uint32_t first =
        static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8u) |
        (static_cast<uint32_t>(bytes[2]) << 16u) |
        (static_cast<uint32_t>(bytes[3]) << 24u);
    uint32_t second =
        static_cast<uint32_t>(bytes[4]) |
        (static_cast<uint32_t>(bytes[5]) << 8u) |
        (static_cast<uint32_t>(bytes[6]) << 16u) |
        (static_cast<uint32_t>(bytes[7]) << 24u);

    decrypt_words(first, second);

    for (uint32_t index = 0u; index < 4u; ++index) {
        bytes[index] = static_cast<uint8_t>(first >> (index * 8u));
        bytes[index + 4u] =
            static_cast<uint8_t>(second >> (index * 8u));
    }
    return Status::Ok;
}

} // namespace oracis::memory

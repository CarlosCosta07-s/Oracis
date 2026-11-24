#include "oracis/memory/nds_game_card_key1.hpp"

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace oracis::memory {

namespace {

uint16_t read_u16_le(const uint8_t *bytes) noexcept
{
    return static_cast<uint16_t>(
        static_cast<uint16_t>(bytes[0]) |
        (static_cast<uint16_t>(bytes[1]) << 8u));
}

uint32_t read_u32_le(const uint8_t *bytes) noexcept
{
    return static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8u) |
        (static_cast<uint32_t>(bytes[2]) << 16u) |
        (static_cast<uint32_t>(bytes[3]) << 24u);
}

uint16_t firmware_header_crc16(
    const uint8_t *bytes, size_t size) noexcept
{
    uint16_t crc = 0u;
    for (size_t index = 0u; index < size; ++index) {
        crc = static_cast<uint16_t>(crc ^ bytes[index]);
        for (uint32_t bit = 0u; bit < 8u; ++bit) {
            crc = static_cast<uint16_t>(
                (crc >> 1u) ^
                ((crc & 1u) != 0u
                    ? UINT16_C(0xA001) : UINT16_C(0)));
        }
    }
    return crc;
}

uint16_t firmware_boot_crc16(
    const std::vector<uint8_t> &arm9,
    const std::vector<uint8_t> &arm7) noexcept
{
    uint16_t crc = UINT16_C(0xFFFF);
    const auto update = [&crc](const std::vector<uint8_t> &bytes) noexcept {
        for (const uint8_t byte : bytes) {
            crc = static_cast<uint16_t>(crc ^ byte);
            for (uint32_t bit = 0u; bit < 8u; ++bit) {
                crc = static_cast<uint16_t>(
                    (crc >> 1u) ^
                    ((crc & 1u) != 0u
                        ? UINT16_C(0xA001) : UINT16_C(0)));
            }
        }
    };
    update(arm9);
    update(arm7);
    return crc;
}

class Key1FirmwareStream final {
public:
    Key1FirmwareStream(
        NdsGameCardKey1 &key,
        const uint8_t *image,
        size_t image_size,
        size_t source_offset) noexcept
        : key_(key), image_(image), image_size_(image_size),
          image_offset_(source_offset)
    {
    }

    bool read_byte(uint8_t &value) noexcept
    {
        if (cursor_ == block_.size()) {
            if (image_offset_ > image_size_ ||
                image_size_ - image_offset_ < block_.size()) {
                return false;
            }
            std::memcpy(block_.data(), image_ + image_offset_, block_.size());
            if (key_.decrypt_block(
                    block_.data(), static_cast<uint32_t>(block_.size())) !=
                Status::Ok) {
                return false;
            }
            image_offset_ += block_.size();
            cursor_ = 0u;
        }
        value = block_[cursor_++];
        return true;
    }

private:
    NdsGameCardKey1 &key_;
    const uint8_t *image_;
    size_t image_size_;
    size_t image_offset_;
    std::array<uint8_t, NdsGameCardKey1::BlockBytes> block_{};
    size_t cursor_ = NdsGameCardKey1::BlockBytes;
};

Status decompress_firmware_lz77(
    NdsGameCardKey1 &key,
    const uint8_t *firmware,
    size_t firmware_size,
    size_t source_offset,
    std::vector<uint8_t> &output) noexcept
{
    constexpr size_t MaxFirmwarePartBytes = size_t{1} << 24u;
    if (firmware == nullptr || source_offset >= firmware_size) {
        return Status::InvalidRegion;
    }

    Key1FirmwareStream stream(key, firmware, firmware_size, source_offset);
    uint8_t header[4]{};
    for (uint8_t &byte : header) {
        if (!stream.read_byte(byte)) return Status::InvalidRegion;
    }
    if (header[0] != UINT8_C(0x10)) return Status::Unsupported;

    const uint32_t output_size =
        static_cast<uint32_t>(header[1]) |
        (static_cast<uint32_t>(header[2]) << 8u) |
        (static_cast<uint32_t>(header[3]) << 16u);
    if (output_size == 0u ||
        static_cast<size_t>(output_size) > MaxFirmwarePartBytes) {
        return Status::InvalidRegion;
    }

    std::vector<uint8_t> decoded;
    try {
        decoded.reserve(output_size);
        while (decoded.size() < output_size) {
            uint8_t flags = 0u;
            if (!stream.read_byte(flags)) return Status::InvalidRegion;

            for (uint32_t bit = 0u; bit < 8u &&
                 decoded.size() < output_size; ++bit) {
                if ((flags & (UINT8_C(0x80) >> bit)) == 0u) {
                    uint8_t literal = 0u;
                    if (!stream.read_byte(literal)) return Status::InvalidRegion;
                    decoded.push_back(literal);
                    continue;
                }

                uint8_t first = 0u;
                uint8_t second = 0u;
                if (!stream.read_byte(first) ||
                    !stream.read_byte(second)) {
                    return Status::InvalidRegion;
                }

                const uint32_t length =
                    static_cast<uint32_t>((first >> 4u) + 3u);
                const uint32_t displacement =
                    (static_cast<uint32_t>(first & UINT8_C(0x0F)) << 8u) |
                    static_cast<uint32_t>(second);
                const uint32_t distance = displacement + 1u;
                if (distance > decoded.size() ||
                    length > output_size - decoded.size()) {
                    return Status::InvalidRegion;
                }

                for (uint32_t index = 0u; index < length; ++index) {
                    decoded.push_back(decoded[decoded.size() - distance]);
                }
            }
        }
    } catch (const std::bad_alloc &) {
        return Status::Unsupported;
    }

    output = std::move(decoded);
    return Status::Ok;
}

} // namespace

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

void NdsGameCardKey1::reset() noexcept
{
    active_table_ = {};
    id_code_ = 0u;
    modulo_bytes_ = 0u;
    level_ = 0u;
    initialized_ = false;
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

Status NdsGameCardKey1::encrypt_command(
    uint8_t *wire_bytes, uint32_t size) const noexcept
{
    if (wire_bytes == nullptr) return Status::InvalidRegion;
    if (size != BlockBytes) return Status::InvalidWidth;

    uint8_t block[BlockBytes]{};
    for (uint32_t index = 0u; index < BlockBytes; ++index) {
        block[index] = wire_bytes[BlockBytes - 1u - index];
    }

    const Status status = encrypt_block(block, size);
    if (status != Status::Ok) return status;

    for (uint32_t index = 0u; index < BlockBytes; ++index) {
        wire_bytes[index] = block[BlockBytes - 1u - index];
    }
    return Status::Ok;
}

Status NdsGameCardKey1::decrypt_command(
    uint8_t *wire_bytes, uint32_t size) const noexcept
{
    if (wire_bytes == nullptr) return Status::InvalidRegion;
    if (size != BlockBytes) return Status::InvalidWidth;

    uint8_t block[BlockBytes]{};
    for (uint32_t index = 0u; index < BlockBytes; ++index) {
        block[index] = wire_bytes[BlockBytes - 1u - index];
    }

    const Status status = decrypt_block(block, size);
    if (status != Status::Ok) return status;

    for (uint32_t index = 0u; index < BlockBytes; ++index) {
        wire_bytes[index] = block[BlockBytes - 1u - index];
    }
    return Status::Ok;
}

Status NdsGameCardKey1::decode_command(
    const uint8_t *wire_bytes, uint32_t size,
    uint32_t expected_counter20, uint16_t expected_random_i,
    uint16_t expected_random_j,
    NdsGameCardKey1Command &decoded) const noexcept
{
    if (wire_bytes == nullptr) return Status::InvalidRegion;
    if (size != BlockBytes) return Status::InvalidWidth;
    if (!initialized_) return Status::PermissionDenied;
    if (expected_counter20 > UINT32_C(0x000FFFFF) ||
        expected_random_i > UINT16_C(0x0FFF) ||
        expected_random_j > UINT16_C(0x0FFF)) {
        return Status::InvalidRegion;
    }

    uint8_t plain[BlockBytes]{};
    for (uint32_t index = 0u; index < BlockBytes; ++index) {
        plain[index] = wire_bytes[index];
    }
    const Status cipher_status = decrypt_command(plain, size);
    if (cipher_status != Status::Ok) return cipher_status;

    const uint8_t opcode = static_cast<uint8_t>(plain[0] >> 4u);
    const uint16_t parameter16 = static_cast<uint16_t>(
        (static_cast<uint16_t>(plain[0] & UINT8_C(0x0F)) << 12u) |
        (static_cast<uint16_t>(plain[1]) << 4u) |
        (static_cast<uint16_t>(plain[2]) >> 4u));
    const uint32_t following24 =
        (static_cast<uint32_t>(plain[2] & UINT8_C(0x0F)) << 20u) |
        (static_cast<uint32_t>(plain[3]) << 12u) |
        (static_cast<uint32_t>(plain[4]) << 4u) |
        (static_cast<uint32_t>(plain[5]) >> 4u);
    const uint32_t counter20 =
        (static_cast<uint32_t>(plain[5] & UINT8_C(0x0F)) << 16u) |
        (static_cast<uint32_t>(plain[6]) << 8u) |
        static_cast<uint32_t>(plain[7]);

    if (counter20 != expected_counter20) return Status::PermissionDenied;

    NdsGameCardKey1Command candidate{};
    candidate.opcode = opcode;
    candidate.parameter16 = parameter16;
    candidate.counter20 = counter20;

    switch (opcode) {
    case 0x04u:
        candidate.kind = NdsGameCardKey1CommandKind::ActivateKey2;
        candidate.key2_seed24 = following24;
        break;
    case 0x01u:
        candidate.kind = NdsGameCardKey1CommandKind::ReadChipId;
        candidate.random_i = static_cast<uint16_t>(following24 >> 12u);
        candidate.random_j = static_cast<uint16_t>(following24 & UINT32_C(0x0FFF));
        if (candidate.random_i != expected_random_i ||
            candidate.random_j != expected_random_j) {
            return Status::PermissionDenied;
        }
        break;
    case 0x02u:
        candidate.kind = NdsGameCardKey1CommandKind::ReadSecureAreaBlock;
        candidate.secure_area_block = parameter16;
        candidate.random_i = static_cast<uint16_t>(following24 >> 12u);
        candidate.random_j = static_cast<uint16_t>(following24 & UINT32_C(0x0FFF));
        if (candidate.secure_area_block < 4u ||
            candidate.secure_area_block > 7u ||
            candidate.random_i != expected_random_i ||
            candidate.random_j != expected_random_j) {
            return Status::PermissionDenied;
        }
        break;
    case 0x06u:
        candidate.kind = NdsGameCardKey1CommandKind::DisableKey2;
        candidate.random_i = static_cast<uint16_t>(following24 >> 12u);
        candidate.random_j = static_cast<uint16_t>(following24 & UINT32_C(0x0FFF));
        if (candidate.random_i != expected_random_i ||
            candidate.random_j != expected_random_j) {
            return Status::PermissionDenied;
        }
        break;
    case 0x0Au:
        candidate.kind = NdsGameCardKey1CommandKind::EnterMainDataMode;
        candidate.random_i = static_cast<uint16_t>(following24 >> 12u);
        candidate.random_j = static_cast<uint16_t>(following24 & UINT32_C(0x0FFF));
        if (candidate.random_i != expected_random_i ||
            candidate.random_j != expected_random_j) {
            return Status::PermissionDenied;
        }
        break;
    default:
        return Status::PermissionDenied;
    }

    decoded = candidate;
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

Status NdsGameCardKey1::decrypt_secure_area(
    uint8_t *bytes, uint32_t size, bool &identifier_valid) noexcept
{
    identifier_valid = false;
    if (bytes == nullptr) return Status::InvalidRegion;
    if (size != SecureAreaEncryptedBytes) return Status::InvalidWidth;
    if (!table_loaded_ || !initialized_) return Status::PermissionDenied;

    const uint32_t saved_id_code = id_code_;
    const uint32_t saved_level = level_;
    const uint32_t saved_modulo = modulo_bytes_;

    Status status = initialize(saved_id_code, 2u, 8u);
    if (status != Status::Ok) return status;

    /* The Secure Area ID is encrypted once with level 2 before the
     * entire 0x800-byte region receives its level-3 encryption. */
    status = decrypt_block(bytes, BlockBytes);
    if (status != Status::Ok) {
        (void)initialize(saved_id_code, saved_level, saved_modulo);
        return status;
    }

    status = initialize(saved_id_code, 3u, 8u);
    if (status != Status::Ok) {
        (void)initialize(saved_id_code, saved_level, saved_modulo);
        return status;
    }

    for (uint32_t offset = 0u; offset < SecureAreaEncryptedBytes;
         offset += BlockBytes) {
        status = decrypt_block(bytes + offset, BlockBytes);
        if (status != Status::Ok) {
            (void)initialize(saved_id_code, saved_level, saved_modulo);
            return status;
        }
    }

    constexpr uint8_t expected_identifier[BlockBytes] = {
        UINT8_C('e'), UINT8_C('n'), UINT8_C('c'), UINT8_C('r'),
        UINT8_C('y'), UINT8_C('O'), UINT8_C('b'), UINT8_C('j')
    };
    identifier_valid = true;
    for (uint32_t index = 0u; index < BlockBytes; ++index) {
        if (bytes[index] != expected_identifier[index]) {
            identifier_valid = false;
            break;
        }
    }

    /* Keep the reusable KEY1 object in the schedule the caller had before
     * this one-shot Secure Area operation. */
    return initialize(saved_id_code, saved_level, saved_modulo);
}

Status NdsGameCardKey1::decrypt_firmware_boot_code(
    const uint8_t *firmware,
    size_t firmware_size,
    NdsFirmwareBootCode &output) noexcept
{
    constexpr size_t FirmwareHeaderBytes = 0x200u;
    constexpr uint32_t MaxFirmwarePartBytes = UINT32_C(0x01000000);
    if (firmware == nullptr || firmware_size < FirmwareHeaderBytes ||
        firmware_size > MaxFirmwarePartBytes) {
        return Status::InvalidRegion;
    }
    if (!table_loaded_) return Status::Unmapped;

    const uint32_t id_code = read_u32_le(firmware + 0x08u);
    if (id_code != UINT32_C(0x5043414D) &&
        id_code != UINT32_C(0x6843414D)) {
        return Status::Unsupported;
    }

    const uint16_t part1_source_info = read_u16_le(firmware + 0x0Cu);
    const uint16_t part1_destination_info = read_u16_le(firmware + 0x0Eu);
    const uint16_t part2_source_info = read_u16_le(firmware + 0x10u);
    const uint16_t part2_destination_info = read_u16_le(firmware + 0x12u);
    const uint16_t shift_data = read_u16_le(firmware + 0x14u);
    const uint16_t expected_crc = read_u16_le(firmware + 0x06u);
    const uint16_t expected_header_crc = read_u16_le(firmware + 0x2Au);
    const uint16_t remaining_header_size = read_u16_le(firmware + 0x2Cu);
    /*
     * Firmware header documentation defines the size from 0x2A and gives
     * 0x162 as the final included byte for size 0x138. Convert that inclusive
     * last-byte address to the exclusive end used by the checked span below.
     */
    const uint64_t remaining_header_end =
        UINT64_C(0x2B) + remaining_header_size;
    if (remaining_header_size < 4u ||
        remaining_header_end > firmware_size ||
        remaining_header_end < UINT64_C(0x2C)) {
        return Status::InvalidRegion;
    }
    const size_t remaining_header_data_size =
        static_cast<size_t>(remaining_header_end - UINT64_C(0x2C));
    if (firmware_header_crc16(
            firmware + 0x2Cu, remaining_header_data_size) !=
        expected_header_crc) {
        return Status::IdentityMismatch;
    }

    const uint32_t part1_source_shift = (shift_data >> 0u) & 7u;
    const uint32_t part1_destination_shift = (shift_data >> 3u) & 7u;
    const uint32_t part2_source_shift = (shift_data >> 6u) & 7u;
    const uint32_t part2_destination_shift = (shift_data >> 9u) & 7u;

    const uint64_t part1_source =
        static_cast<uint64_t>(part1_source_info) *
        (UINT64_C(4) << part1_source_shift);
    const uint64_t part2_source =
        static_cast<uint64_t>(part2_source_info) *
        (UINT64_C(4) << part2_source_shift);
    const uint64_t part1_destination_offset =
        static_cast<uint64_t>(part1_destination_info) *
        (UINT64_C(4) << part1_destination_shift);
    const uint64_t part2_destination_offset =
        static_cast<uint64_t>(part2_destination_info) *
        (UINT64_C(4) << part2_destination_shift);
    const uint64_t part1_destination_base = UINT64_C(0x02800000);
    const uint64_t part2_destination_base =
        (shift_data & UINT16_C(0x1000)) != 0u
            ? UINT64_C(0x02800000) : UINT64_C(0x03810000);

    if (part1_source_info == 0u || part2_source_info == 0u ||
        part1_destination_info == 0u || part2_destination_info == 0u ||
        part1_source < FirmwareHeaderBytes ||
        part2_source < FirmwareHeaderBytes ||
        part1_source >= firmware_size || part2_source >= firmware_size ||
        part1_destination_offset >= part1_destination_base ||
        part2_destination_offset >= part2_destination_base) {
        return Status::InvalidRegion;
    }

    const uint64_t part1_destination =
        part1_destination_base - part1_destination_offset;
    const uint64_t part2_destination =
        part2_destination_base - part2_destination_offset;
    if (part1_destination > UINT32_MAX ||
        part2_destination > UINT32_MAX ||
        part1_destination == part2_destination) {
        return Status::AddressOverflow;
    }

    const bool restore_schedule = initialized_;
    const uint32_t saved_id_code = id_code_;
    const uint32_t saved_level = level_;
    const uint32_t saved_modulo = modulo_bytes_;
    const auto restore = [this, restore_schedule, saved_id_code,
                          saved_level, saved_modulo]() noexcept {
        if (restore_schedule) {
            (void)initialize(saved_id_code, saved_level, saved_modulo);
        } else {
            reset();
        }
    };

    NdsFirmwareBootCode candidate{};
    candidate.arm9_load_address = static_cast<uint32_t>(part1_destination);
    candidate.arm7_load_address = static_cast<uint32_t>(part2_destination);
    candidate.expected_crc16 = expected_crc;

    Status status = initialize(id_code, 1u, 12u);
    if (status != Status::Ok) {
        restore();
        return status;
    }

    std::memcpy(candidate.decrypted_rom_info.data(), firmware + 0x18u, 8u);
    status = decrypt_block(
        candidate.decrypted_rom_info.data(),
        static_cast<uint32_t>(candidate.decrypted_rom_info.size()));
    if (status != Status::Ok) {
        restore();
        return status;
    }

    status = initialize(id_code, 2u, 12u);
    if (status == Status::Ok) {
        status = decompress_firmware_lz77(
            *this, firmware, firmware_size,
            static_cast<size_t>(part1_source), candidate.arm9_bytes);
    }
    if (status == Status::Ok) {
        status = decompress_firmware_lz77(
            *this, firmware, firmware_size,
            static_cast<size_t>(part2_source), candidate.arm7_bytes);
    }
    if (status != Status::Ok) {
        restore();
        return status;
    }

    if (candidate.arm9_bytes.empty() || candidate.arm7_bytes.empty() ||
        candidate.arm9_bytes.size() > UINT32_MAX ||
        candidate.arm7_bytes.size() > UINT32_MAX ||
        part1_destination + candidate.arm9_bytes.size() >
            UINT64_C(0x1'0000'0000) ||
        part2_destination + candidate.arm7_bytes.size() >
            UINT64_C(0x1'0000'0000)) {
        restore();
        return Status::AddressOverflow;
    }

    candidate.computed_crc16 =
        firmware_boot_crc16(candidate.arm9_bytes, candidate.arm7_bytes);
    if (candidate.computed_crc16 != candidate.expected_crc16) {
        restore();
        return Status::IdentityMismatch;
    }

    restore();
    output = std::move(candidate);
    return Status::Ok;
}


} // namespace oracis::memory

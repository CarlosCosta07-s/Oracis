/*
 * Nintendo DS boot-header validation used by the Oracis local Platinum loader.
 *
 * The bytes validated here are extracted from a ROM supplied by the user and
 * are never stored in the repository.
 */
#ifndef ORACIS_HOST_DETAIL_NDS_CARTRIDGE_HEADER_HPP
#define ORACIS_HOST_DETAIL_NDS_CARTRIDGE_HEADER_HPP

#include <cstddef>
#include <cstdint>

namespace oracis::host::detail {

constexpr std::size_t NdsCartridgeHeaderStoredSize = 0x170u;
constexpr uint32_t NdsCartridgeHeaderRamAddress = UINT32_C(0x027FFE00);

inline uint16_t read_u16_le(const uint8_t *bytes) noexcept
{
    return static_cast<uint16_t>(
        static_cast<uint16_t>(bytes[0]) |
        (static_cast<uint16_t>(bytes[1]) << 8u));
}

inline uint32_t read_u32_le(const uint8_t *bytes) noexcept
{
    return static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8u) |
        (static_cast<uint32_t>(bytes[2]) << 16u) |
        (static_cast<uint32_t>(bytes[3]) << 24u);
}

/* Reflected CRC-16/IBM polynomial; NDS header uses the 0xFFFF seed. */
inline uint16_t nds_header_crc16(
    const uint8_t *bytes, std::size_t length) noexcept
{
    if (bytes == nullptr) return 0u;
    uint16_t crc = UINT16_C(0xFFFF);
    for (std::size_t index = 0u; index < length; ++index) {
        crc = static_cast<uint16_t>(crc ^ bytes[index]);
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            crc = static_cast<uint16_t>(
                (crc >> 1u) ^
                ((crc & 1u) != 0u ? UINT16_C(0xA001) : UINT16_C(0)));
        }
    }
    return crc;
}

/*
 * The local Platinum boot path accepts the pinned USA Rev 1 header shape.
 * Segment byte identities are checked separately by the local image loader.
 */
inline bool validate_platinum_usa_rev1_boot_header(
    const uint8_t *header, std::size_t size) noexcept
{
    if (header == nullptr || size != NdsCartridgeHeaderStoredSize) {
        return false;
    }

    constexpr uint8_t ExpectedGameCode[4] = {'C', 'P', 'U', 'E'};
    for (std::size_t index = 0u; index < sizeof(ExpectedGameCode); ++index) {
        if (header[0x0Cu + index] != ExpectedGameCode[index]) return false;
    }

    if (header[0x12u] != 0u ||
        read_u32_le(header + 0x20u) != UINT32_C(0x00004000) ||
        read_u32_le(header + 0x24u) != UINT32_C(0x02000800) ||
        read_u32_le(header + 0x28u) != UINT32_C(0x02000000) ||
        read_u32_le(header + 0x2Cu) != UINT32_C(0x001023F8) ||
        read_u32_le(header + 0x30u) != UINT32_C(0x00409800) ||
        read_u32_le(header + 0x34u) != UINT32_C(0x02380000) ||
        read_u32_le(header + 0x38u) != UINT32_C(0x02380000) ||
        read_u32_le(header + 0x3Cu) != UINT32_C(0x000277FC) ||
        read_u16_le(header + 0x15Cu) != UINT16_C(0xCF56)) {
        return false;
    }

    return nds_header_crc16(header, 0x15Eu) ==
        read_u16_le(header + 0x15Eu);
}

} // namespace oracis::host::detail

#endif /* ORACIS_HOST_DETAIL_NDS_CARTRIDGE_HEADER_HPP */

/*
 * Nintendo DS KEY1 cryptographic primitive.
 *
 * The 0x1048-byte initial Blowfish table is supplied by the caller from a
 * user-owned NDS ARM7 BIOS image. No BIOS bytes or precomputed keys are shipped.
 */
#ifndef ORACIS_MEMORY_NDS_GAME_CARD_KEY1_HPP
#define ORACIS_MEMORY_NDS_GAME_CARD_KEY1_HPP

#include <array>
#include <cstdint>
#include <vector>

#include "oracis/memory/guest_memory.hpp"

namespace oracis::memory {

enum class NdsGameCardKey1CommandKind : uint8_t {
    ActivateKey2 = 0x04u,
    ReadChipId = 0x01u,
    ReadSecureAreaBlock = 0x02u,
    DisableKey2 = 0x06u,
    EnterMainDataMode = 0x0Au
};

struct NdsGameCardKey1Command {
    NdsGameCardKey1CommandKind kind = NdsGameCardKey1CommandKind::ActivateKey2;
    uint8_t opcode = 0u;
    uint16_t parameter16 = 0u;
    uint16_t random_i = 0u;
    uint16_t random_j = 0u;
    uint16_t secure_area_block = 0u;
    uint32_t key2_seed24 = 0u;
    uint32_t counter20 = 0u;
};

struct NdsFirmwareBootCode {
    uint32_t arm9_load_address = 0u;
    std::vector<uint8_t> arm9_bytes;
    uint32_t arm7_load_address = 0u;
    std::vector<uint8_t> arm7_bytes;
    uint16_t expected_crc16 = 0u;
    uint16_t computed_crc16 = 0u;
    std::array<uint8_t, 8> decrypted_rom_info{};
};

class NdsGameCardKey1 final {
public:
    static constexpr uint32_t KeyTableBytes = UINT32_C(0x1048);
    static constexpr uint32_t Arm7BiosBytes = UINT32_C(0x4000);
    static constexpr uint32_t Arm7BiosKeyTableOffset = UINT32_C(0x30);
    static constexpr uint32_t KeyTableWords = KeyTableBytes / 4u;
    static constexpr uint32_t BlockBytes = 8u;
    static constexpr uint32_t SecureAreaEncryptedBytes = UINT32_C(0x800);

    NdsGameCardKey1() noexcept = default;
    NdsGameCardKey1(const NdsGameCardKey1 &) = delete;
    NdsGameCardKey1 &operator=(const NdsGameCardKey1 &) = delete;

    /*
     * Load the raw table slice from a caller-owned BIOS copy:
     * NDS ARM7 BIOS offset 0x30, length 0x1048 bytes.
     * The table is copied; the caller may release its buffer afterwards.
     */
    Status load_key_table(const uint8_t *bytes, uint32_t size) noexcept;

    /*
     * Initialize from the four-byte cartridge Game Code as a little-endian
     * word. Retail cartridge commands use level 2 and modulo 8.
     */
    Status initialize(
        uint32_t id_code, uint32_t level,
        uint32_t modulo_bytes) noexcept;

    /* Clear the active schedule while retaining the caller-supplied BIOS table. */
    void reset() noexcept;

    Status encrypt_block(uint8_t *bytes, uint32_t size) const noexcept;
    Status decrypt_block(uint8_t *bytes, uint32_t size) const noexcept;

    /* Decrypt the retail 0x800-byte Secure Area prefix: decrypt its ID with
     * KEY1 level 2, then the complete prefix with level 3. Reports whether
     * the resulting identifier is "encryObj" and restores the prior schedule. */
    Status decrypt_secure_area(uint8_t *bytes, uint32_t size,
                               bool &identifier_valid) noexcept;

    /*
     * Decode the ARM9/ARM7 IPL blocks from a caller-supplied raw NDS firmware
     * flash image. Uses the BIOS-derived base table already loaded in this
     * object and the firmware's MACP/MACh identifier. The result is committed
     * only after both LZ77 streams and the combined CRC16 validate.
     */
    Status decrypt_firmware_boot_code(
        const uint8_t *firmware, size_t firmware_size,
        NdsFirmwareBootCode &output) noexcept;

    /* Game Card command bytes are wire-order (MSB first); these helpers
     * reverse them around the little-endian 64-bit block primitive. */
    Status encrypt_command(uint8_t *wire_bytes, uint32_t size) const noexcept;
    Status decrypt_command(uint8_t *wire_bytes, uint32_t size) const noexcept;

    /* Decrypt and validate an NDS KEY1 command. The expected counter is
     * supplied by the protocol owner so repeated slow-cart commands can
     * retain the same counter while normal commands advance it. */
    Status decode_command(
        const uint8_t *wire_bytes, uint32_t size,
        uint32_t expected_counter20, uint16_t expected_random_i,
        uint16_t expected_random_j,
        NdsGameCardKey1Command &decoded) const noexcept;

    bool table_loaded() const noexcept { return table_loaded_; }
    bool initialized() const noexcept { return initialized_; }
    uint32_t id_code() const noexcept { return id_code_; }
    uint32_t level() const noexcept { return level_; }
    uint32_t modulo_bytes() const noexcept { return modulo_bytes_; }

private:
    static uint32_t byte_swap32(uint32_t value) noexcept;
    uint32_t round_function(uint32_t value) const noexcept;
    void encrypt_words(uint32_t &first, uint32_t &second) const noexcept;
    void decrypt_words(uint32_t &first, uint32_t &second) const noexcept;
    void apply_key_code(std::array<uint32_t, 3> &key_code) noexcept;

    std::array<uint32_t, KeyTableWords> base_table_{};
    std::array<uint32_t, KeyTableWords> active_table_{};
    uint32_t id_code_ = 0u;
    uint32_t modulo_bytes_ = 0u;
    uint32_t level_ = 0u;
    bool table_loaded_ = false;
    bool initialized_ = false;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_GAME_CARD_KEY1_HPP */

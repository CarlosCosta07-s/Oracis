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

#include "oracis/memory/guest_memory.hpp"

namespace oracis::memory {

class NdsGameCardKey1 final {
public:
    static constexpr uint32_t KeyTableBytes = UINT32_C(0x1048);
    static constexpr uint32_t Arm7BiosBytes = UINT32_C(0x4000);
    static constexpr uint32_t Arm7BiosKeyTableOffset = UINT32_C(0x30);
    static constexpr uint32_t KeyTableWords = KeyTableBytes / 4u;
    static constexpr uint32_t BlockBytes = 8u;

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

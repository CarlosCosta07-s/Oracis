/*
 * Nintendo DS real-time clock (Seiko-style serial RTC) behind the ARM7-only
 * RTC register at 0x04000138.
 *
 * The CPU drives the chip by bit-banging three lines (DATA, CLK, CS). The
 * implemented wire protocol, per the public GBATEK description, is:
 *   - CS rises: a new transfer begins.
 *   - While CS is high, each rising CLK edge samples one DATA bit sent by the
 *     CPU. All bytes travel LSB first.
 *   - The first byte is the command: bits 0-3 = 0110b, bits 4-6 = register
 *     index, bit 7 = 1 for read / 0 for write.
 *   - For reads, the chip presents the next output bit on each falling CLK
 *     edge; the CPU samples it from bit 0 of the register.
 *   - CS falling aborts any incomplete transfer.
 *
 * The chip holds no wall-clock dependency: the host sets the time and moves
 * it forward with advance_seconds(), which keeps tests and replays
 * deterministic. Alarm/frequency interrupt generation, clock adjustment and
 * the INT pin are stored but not modeled. Commands outside this description
 * are rejected with Status::Unsupported and a diagnostic.
 */
#ifndef ORACIS_MEMORY_NDS_RTC_HPP
#define ORACIS_MEMORY_NDS_RTC_HPP

#include <array>
#include <cstdint>

#include "oracis/memory/guest_memory.hpp"

namespace oracis::memory {

struct NdsRtcDateTime {
    uint8_t year = 0u;     /* 0-99, representing 2000-2099 */
    uint8_t month = 1u;    /* 1-12 */
    uint8_t day = 1u;      /* 1-31, bounded by the month */
    uint8_t weekday = 0u;  /* 0-6 free-running counter; mapping is software-defined */
    uint8_t hour = 0u;     /* 0-23 */
    uint8_t minute = 0u;   /* 0-59 */
    uint8_t second = 0u;   /* 0-59 */
};

class NdsRtc final {
public:
    static constexpr uint32_t RegisterAddress = UINT32_C(0x04000138);

    /* Register bits. Bits 3 and 7 are not modeled and read as zero. */
    static constexpr uint8_t DataBit = 0x01u;
    static constexpr uint8_t ClockBit = 0x02u;
    static constexpr uint8_t SelectBit = 0x04u;
    static constexpr uint8_t DataOutputBit = 0x10u;
    static constexpr uint8_t ClockOutputBit = 0x20u;
    static constexpr uint8_t SelectOutputBit = 0x40u;
    static constexpr uint8_t RegisterMask = 0x77u;

    /* Chip registers addressed by the command index. */
    static constexpr uint8_t RegStatus1 = 0u;
    static constexpr uint8_t RegStatus2 = 1u;
    static constexpr uint8_t RegDateTime = 2u;
    static constexpr uint8_t RegTime = 3u;
    static constexpr uint8_t RegAlarm1 = 4u;
    static constexpr uint8_t RegAlarm2 = 5u;
    static constexpr uint8_t RegClockAdjust = 6u;
    static constexpr uint8_t RegFree = 7u;

    /* Status register 1 bits. */
    static constexpr uint8_t Status1Reset = 0x01u;
    static constexpr uint8_t Status1Hour24 = 0x02u;
    static constexpr uint8_t Status1UserMask = 0x0Cu;
    static constexpr uint8_t Status1LowPower = 0x40u;
    static constexpr uint8_t Status1PowerOn = 0x80u;

    NdsRtc() noexcept { reset(); }

    NdsRtc(const NdsRtc &) = delete;
    NdsRtc &operator=(const NdsRtc &) = delete;

    ReadResult read(NdsCpu cpu,
                    uint32_t address,
                    uint32_t width) const noexcept;

    Status write(NdsCpu cpu,
                 uint32_t address,
                 uint32_t width,
                 uint32_t value) noexcept;

    /* Power-on state: 2000-01-01 00:00:00, 12-hour mode, POC flag set. */
    void reset() noexcept;

    /* Host time source. Returns Unsupported when any field is out of range. */
    Status set_date_time(const NdsRtcDateTime &value) noexcept;
    NdsRtcDateTime date_time() const noexcept { return date_time_; }

    /* Moves the clock forward, rolling over minutes, days, months and years. */
    void advance_seconds(uint64_t seconds) noexcept;

    bool hour24() const noexcept {
        return (status1_ & Status1Hour24) != 0u;
    }
    uint8_t status1() const noexcept { return status1_; }
    uint8_t status2() const noexcept { return status2_; }

    /* Last rejected command or value; nullptr when nothing was rejected. */
    const char *last_diagnostic() const noexcept { return diagnostic_; }

private:
    enum class Phase : uint8_t { Idle, Command, WriteData, ReadData };

    static unsigned register_size(uint8_t index) noexcept;
    static uint8_t days_in_month(uint8_t year, uint8_t month) noexcept;
    static uint8_t to_bcd(unsigned value) noexcept;
    static bool from_bcd(uint8_t value, unsigned minimum, unsigned maximum,
                         uint8_t &result) noexcept;

    uint8_t encode_hour() const noexcept;
    bool decode_hour(uint8_t value, uint8_t &hour) const noexcept;

    void begin_transfer() noexcept;
    void abort_transfer() noexcept;
    Status on_clock_rising(bool data_bit) noexcept;
    void on_clock_falling() noexcept;
    Status on_command(uint8_t command) noexcept;
    Status commit_write() noexcept;
    void load_read_buffer() noexcept;
    void reset_registers() noexcept;

    uint8_t port_ = 0u;
    bool output_bit_ = false;

    Phase phase_ = Phase::Idle;
    uint8_t command_index_ = 0u;
    uint8_t shift_ = 0u;
    uint8_t bit_count_ = 0u;
    uint8_t byte_index_ = 0u;
    uint8_t byte_total_ = 0u;
    std::array<uint8_t, 7> buffer_{};

    NdsRtcDateTime date_time_{};
    uint8_t status1_ = 0u;
    uint8_t status2_ = 0u;
    std::array<uint8_t, 3> alarm1_{};
    std::array<uint8_t, 3> alarm2_{};
    uint8_t clock_adjust_ = 0u;
    uint8_t free_register_ = 0u;

    const char *diagnostic_ = nullptr;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_RTC_HPP */

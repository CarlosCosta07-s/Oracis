#include "oracis/memory/nds_rtc.hpp"

namespace oracis::memory {

namespace {

bool drives(uint8_t port, uint8_t value_bit, uint8_t output_bit) noexcept
{
    /* A line only reaches the chip while the CPU configures it as output. */
    return (port & value_bit) != 0u && (port & output_bit) != 0u;
}

} // namespace

unsigned NdsRtc::register_size(uint8_t index) noexcept
{
    switch (index) {
    case RegDateTime: return 7u;
    case RegTime:
    case RegAlarm1:
    case RegAlarm2: return 3u;
    default: return 1u;
    }
}

uint8_t NdsRtc::days_in_month(uint8_t year, uint8_t month) noexcept
{
    switch (month) {
    case 2: return (year % 4u) == 0u ? 29u : 28u;
    case 4:
    case 6:
    case 9:
    case 11: return 30u;
    default: return 31u;
    }
}

uint8_t NdsRtc::to_bcd(unsigned value) noexcept
{
    return static_cast<uint8_t>(((value / 10u) << 4u) | (value % 10u));
}

bool NdsRtc::from_bcd(uint8_t value, unsigned minimum, unsigned maximum,
                      uint8_t &result) noexcept
{
    const unsigned tens = value >> 4u;
    const unsigned ones = value & 0x0Fu;
    if (tens > 9u || ones > 9u) return false;
    const unsigned decoded = tens * 10u + ones;
    if (decoded < minimum || decoded > maximum) return false;
    result = static_cast<uint8_t>(decoded);
    return true;
}

void NdsRtc::reset_registers() noexcept
{
    date_time_ = {};
    status2_ = 0u;
    alarm1_.fill(0u);
    alarm2_.fill(0u);
    clock_adjust_ = 0u;
    free_register_ = 0u;
    status1_ = static_cast<uint8_t>(
        status1_ & (Status1PowerOn | Status1LowPower));
}

void NdsRtc::reset() noexcept
{
    port_ = 0u;
    output_bit_ = false;
    phase_ = Phase::Idle;
    command_index_ = 0u;
    shift_ = 0u;
    bit_count_ = 0u;
    byte_index_ = 0u;
    byte_total_ = 0u;
    buffer_.fill(0u);
    status1_ = 0u;
    reset_registers();
    status1_ = Status1PowerOn;
    diagnostic_ = nullptr;
}

Status NdsRtc::set_date_time(const NdsRtcDateTime &value) noexcept
{
    if (value.year > 99u || value.month < 1u || value.month > 12u ||
        value.day < 1u || value.day > days_in_month(value.year, value.month) ||
        value.weekday > 6u || value.hour > 23u || value.minute > 59u ||
        value.second > 59u) {
        diagnostic_ = "RTC host date/time is out of range";
        return Status::Unsupported;
    }
    date_time_ = value;
    return Status::Ok;
}

void NdsRtc::advance_seconds(uint64_t seconds) noexcept
{
    uint64_t total = static_cast<uint64_t>(date_time_.second) + seconds;
    date_time_.second = static_cast<uint8_t>(total % 60u);
    total = static_cast<uint64_t>(date_time_.minute) + total / 60u;
    date_time_.minute = static_cast<uint8_t>(total % 60u);
    total = static_cast<uint64_t>(date_time_.hour) + total / 60u;
    date_time_.hour = static_cast<uint8_t>(total % 24u);
    uint64_t days = total / 24u;

    date_time_.weekday = static_cast<uint8_t>(
        (static_cast<uint64_t>(date_time_.weekday) + days) % 7u);

    while (days != 0u) {
        const uint8_t month_days =
            days_in_month(date_time_.year, date_time_.month);
        const uint8_t left_in_month =
            static_cast<uint8_t>(month_days - date_time_.day);
        if (days <= left_in_month) {
            date_time_.day = static_cast<uint8_t>(date_time_.day + days);
            days = 0u;
            break;
        }
        days -= static_cast<uint64_t>(left_in_month) + 1u;
        date_time_.day = 1u;
        if (date_time_.month == 12u) {
            date_time_.month = 1u;
            /* The chip counts 00-99 and wraps; the span is 2000-2099. */
            date_time_.year = static_cast<uint8_t>((date_time_.year + 1u) % 100u);
        } else {
            date_time_.month = static_cast<uint8_t>(date_time_.month + 1u);
        }
    }
}

uint8_t NdsRtc::encode_hour() const noexcept
{
    const uint8_t hour = date_time_.hour;
    const bool pm = hour >= 12u;
    /*
     * Bit 6 mirrors the afternoon state in both modes so that toggling the
     * 12/24-hour setting does not change what the chip stores. This follows
     * the public description and is not yet checked against hardware.
     */
    uint8_t value = hour24() ? to_bcd(hour) : to_bcd(hour % 12u);
    if (pm) value = static_cast<uint8_t>(value | 0x40u);
    return value;
}

bool NdsRtc::decode_hour(uint8_t value, uint8_t &hour) const noexcept
{
    const bool pm = (value & 0x40u) != 0u;
    const uint8_t bcd = static_cast<uint8_t>(value & 0x3Fu);
    uint8_t decoded = 0u;
    if (hour24()) {
        if (!from_bcd(bcd, 0u, 23u, decoded)) return false;
    } else {
        if (!from_bcd(bcd, 0u, 11u, decoded)) return false;
        if (pm) decoded = static_cast<uint8_t>(decoded + 12u);
    }
    hour = decoded;
    return true;
}

ReadResult NdsRtc::read(NdsCpu cpu, uint32_t address, uint32_t width) const noexcept
{
    if (address != RegisterAddress) return {Status::Unmapped, 0u};
    if (cpu != NDS_ARM9 && cpu != NDS_ARM7) return {Status::InvalidCpu, 0u};
    if (cpu != NDS_ARM7) return {Status::CpuAccessDenied, 0u};
    if (width != 1u && width != 2u) return {Status::InvalidWidth, 0u};

    uint8_t value = port_;
    /* The chip owns the DATA line unless the CPU drives it. */
    if ((port_ & DataOutputBit) == 0u) {
        value = static_cast<uint8_t>(
            (value & ~DataBit) | (output_bit_ ? DataBit : 0u));
    }
    return {Status::Ok, static_cast<uint32_t>(value)};
}

Status NdsRtc::write(NdsCpu cpu, uint32_t address, uint32_t width,
                     uint32_t value) noexcept
{
    if (address != RegisterAddress) return Status::Unmapped;
    if (cpu != NDS_ARM9 && cpu != NDS_ARM7) return Status::InvalidCpu;
    if (cpu != NDS_ARM7) return Status::CpuAccessDenied;
    if (width != 1u && width != 2u) return Status::InvalidWidth;

    const uint8_t previous = port_;
    const uint8_t next = static_cast<uint8_t>(value & RegisterMask);
    port_ = next;

    const bool cs_before = drives(previous, SelectBit, SelectOutputBit);
    const bool cs_after = drives(next, SelectBit, SelectOutputBit);
    const bool clk_before = drives(previous, ClockBit, ClockOutputBit);
    const bool clk_after = drives(next, ClockBit, ClockOutputBit);

    if (!cs_before && cs_after) {
        begin_transfer();
    } else if (cs_before && !cs_after) {
        abort_transfer();
    }

    if (!cs_after) return Status::Ok;

    if (!clk_before && clk_after) {
        return on_clock_rising(drives(next, DataBit, DataOutputBit));
    }
    if (clk_before && !clk_after) {
        on_clock_falling();
    }
    return Status::Ok;
}

void NdsRtc::begin_transfer() noexcept
{
    phase_ = Phase::Command;
    shift_ = 0u;
    bit_count_ = 0u;
    byte_index_ = 0u;
    byte_total_ = 0u;
    output_bit_ = false;
}

void NdsRtc::abort_transfer() noexcept
{
    phase_ = Phase::Idle;
    shift_ = 0u;
    bit_count_ = 0u;
    byte_index_ = 0u;
    byte_total_ = 0u;
    output_bit_ = false;
}

Status NdsRtc::on_clock_rising(bool data_bit) noexcept
{
    if (phase_ == Phase::Command) {
        shift_ = static_cast<uint8_t>(shift_ | ((data_bit ? 1u : 0u) << bit_count_));
        if (++bit_count_ < 8u) return Status::Ok;
        const uint8_t command = shift_;
        shift_ = 0u;
        bit_count_ = 0u;
        return on_command(command);
    }

    if (phase_ == Phase::WriteData) {
        shift_ = static_cast<uint8_t>(shift_ | ((data_bit ? 1u : 0u) << bit_count_));
        if (++bit_count_ < 8u) return Status::Ok;
        buffer_[byte_index_] = shift_;
        shift_ = 0u;
        bit_count_ = 0u;
        if (++byte_index_ < byte_total_) return Status::Ok;
        phase_ = Phase::Idle;
        return commit_write();
    }

    /* Rising edges carry no CPU data while the chip is sending. */
    return Status::Ok;
}

void NdsRtc::on_clock_falling() noexcept
{
    if (phase_ != Phase::ReadData) return;
    if (byte_index_ >= byte_total_) {
        output_bit_ = false;
        return;
    }
    output_bit_ = ((buffer_[byte_index_] >> bit_count_) & 1u) != 0u;
    if (++bit_count_ == 8u) {
        bit_count_ = 0u;
        ++byte_index_;
    }
}

Status NdsRtc::on_command(uint8_t command) noexcept
{
    if ((command & 0x0Fu) != 0x06u) {
        phase_ = Phase::Idle;
        diagnostic_ = "RTC command byte lacks the 0110b signature";
        return Status::Unsupported;
    }

    command_index_ = static_cast<uint8_t>((command >> 4u) & 0x07u);
    byte_total_ = static_cast<uint8_t>(register_size(command_index_));
    byte_index_ = 0u;
    bit_count_ = 0u;
    shift_ = 0u;

    if ((command & 0x80u) != 0u) {
        load_read_buffer();
        phase_ = Phase::ReadData;
    } else {
        buffer_.fill(0u);
        phase_ = Phase::WriteData;
    }
    return Status::Ok;
}

void NdsRtc::load_read_buffer() noexcept
{
    buffer_.fill(0u);
    switch (command_index_) {
    case RegStatus1:
        buffer_[0] = status1_;
        /* Power-on and low-power flags clear once the CPU has read them. */
        status1_ = static_cast<uint8_t>(
            status1_ & ~(Status1PowerOn | Status1LowPower));
        break;
    case RegStatus2:
        buffer_[0] = status2_;
        break;
    case RegDateTime:
        buffer_[0] = to_bcd(date_time_.year);
        buffer_[1] = to_bcd(date_time_.month);
        buffer_[2] = to_bcd(date_time_.day);
        buffer_[3] = date_time_.weekday;
        buffer_[4] = encode_hour();
        buffer_[5] = to_bcd(date_time_.minute);
        buffer_[6] = to_bcd(date_time_.second);
        break;
    case RegTime:
        buffer_[0] = encode_hour();
        buffer_[1] = to_bcd(date_time_.minute);
        buffer_[2] = to_bcd(date_time_.second);
        break;
    case RegAlarm1:
        for (unsigned i = 0u; i < 3u; ++i) buffer_[i] = alarm1_[i];
        break;
    case RegAlarm2:
        for (unsigned i = 0u; i < 3u; ++i) buffer_[i] = alarm2_[i];
        break;
    case RegClockAdjust:
        buffer_[0] = clock_adjust_;
        break;
    default:
        buffer_[0] = free_register_;
        break;
    }
}

Status NdsRtc::commit_write() noexcept
{
    switch (command_index_) {
    case RegStatus1: {
        const uint8_t value = buffer_[0];
        if ((value & Status1Reset) != 0u) reset_registers();
        status1_ = static_cast<uint8_t>(
            (status1_ & (Status1PowerOn | Status1LowPower)) |
            (value & (Status1Hour24 | Status1UserMask)));
        return Status::Ok;
    }
    case RegStatus2:
        status2_ = buffer_[0];
        return Status::Ok;
    case RegDateTime: {
        NdsRtcDateTime parsed{};
        if (!from_bcd(buffer_[0], 0u, 99u, parsed.year) ||
            !from_bcd(buffer_[1], 1u, 12u, parsed.month) ||
            !from_bcd(buffer_[2], 1u, 31u, parsed.day) ||
            buffer_[3] > 6u ||
            !decode_hour(buffer_[4], parsed.hour) ||
            !from_bcd(buffer_[5], 0u, 59u, parsed.minute) ||
            !from_bcd(static_cast<uint8_t>(buffer_[6] & 0x7Fu),
                      0u, 59u, parsed.second) ||
            parsed.day > days_in_month(parsed.year, parsed.month)) {
            diagnostic_ = "RTC date/time write contains an invalid BCD field";
            return Status::Unsupported;
        }
        parsed.weekday = buffer_[3];
        date_time_ = parsed;
        return Status::Ok;
    }
    case RegTime: {
        uint8_t hour = 0u;
        uint8_t minute = 0u;
        uint8_t second = 0u;
        if (!decode_hour(buffer_[0], hour) ||
            !from_bcd(buffer_[1], 0u, 59u, minute) ||
            !from_bcd(static_cast<uint8_t>(buffer_[2] & 0x7Fu),
                      0u, 59u, second)) {
            diagnostic_ = "RTC time write contains an invalid BCD field";
            return Status::Unsupported;
        }
        date_time_.hour = hour;
        date_time_.minute = minute;
        date_time_.second = second;
        return Status::Ok;
    }
    case RegAlarm1:
        for (unsigned i = 0u; i < 3u; ++i) alarm1_[i] = buffer_[i];
        return Status::Ok;
    case RegAlarm2:
        for (unsigned i = 0u; i < 3u; ++i) alarm2_[i] = buffer_[i];
        return Status::Ok;
    case RegClockAdjust:
        clock_adjust_ = buffer_[0];
        return Status::Ok;
    default:
        free_register_ = buffer_[0];
        return Status::Ok;
    }
}

} // namespace oracis::memory

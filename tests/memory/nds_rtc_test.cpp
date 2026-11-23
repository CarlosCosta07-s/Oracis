#include "oracis/memory/nds_rtc.hpp"
#include "oracis/recomp/runtime_arm.h"

#include <array>
#include <cstdint>
#include <cstdio>

namespace {

using oracis::memory::NdsRtc;
using oracis::memory::NdsRtcDateTime;
using oracis::memory::Status;

int fail(const char *message)
{
    std::fprintf(stderr, "nds_rtc_test: %s\n", message);
    return 1;
}

/* Bit-bangs the RTC register the way ARM7 driver code does. */
class Wire {
public:
    explicit Wire(NdsRtc &rtc) : rtc_(rtc) {}

    Status last() const { return last_; }

    void put(uint32_t value)
    {
        const Status status =
            rtc_.write(NDS_ARM7, NdsRtc::RegisterAddress, 1u, value);
        if (status != Status::Ok) last_ = status;
    }

    void begin()
    {
        last_ = Status::Ok;
        put(0x70u);
        put(0x74u);
    }

    void end() { put(0x70u); }

    void send(uint8_t byte)
    {
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            const uint32_t data = (byte >> bit) & 1u;
            put(0x74u | data);
            put(0x76u | data);
        }
    }

    uint8_t receive()
    {
        uint8_t byte = 0u;
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            put(0x64u); /* DATA is an input, CLK falls, chip presents a bit */
            put(0x66u);
            const auto sample =
                rtc_.read(NDS_ARM7, NdsRtc::RegisterAddress, 1u);
            byte = static_cast<uint8_t>(byte | ((sample.value & 1u) << bit));
        }
        return byte;
    }

    static uint8_t command(uint8_t index, bool read)
    {
        return static_cast<uint8_t>(
            0x06u | (index << 4u) | (read ? 0x80u : 0u));
    }

    template <size_t N>
    std::array<uint8_t, N> read_register(uint8_t index)
    {
        std::array<uint8_t, N> bytes{};
        begin();
        send(command(index, true));
        for (auto &byte : bytes) byte = receive();
        end();
        return bytes;
    }

    template <size_t N>
    void write_register(uint8_t index, const std::array<uint8_t, N> &bytes)
    {
        begin();
        send(command(index, false));
        for (uint8_t byte : bytes) send(byte);
        end();
    }

private:
    NdsRtc &rtc_;
    Status last_ = Status::Ok;
};

bool same(const NdsRtcDateTime &a, const NdsRtcDateTime &b)
{
    return a.year == b.year && a.month == b.month && a.day == b.day &&
           a.weekday == b.weekday && a.hour == b.hour &&
           a.minute == b.minute && a.second == b.second;
}

} // namespace

int main()
{
    NdsRtc rtc;
    Wire wire(rtc);

    /* Power-on flag is visible once, then clears. */
    if (wire.read_register<1>(NdsRtc::RegStatus1)[0] != 0x80u) {
        return fail("power-on flag was not reported by status register 1");
    }
    if (wire.read_register<1>(NdsRtc::RegStatus1)[0] != 0x00u) {
        return fail("power-on flag did not clear after being read");
    }

    /* Default clock: 2000-01-01 00:00:00. */
    {
        const auto bytes = wire.read_register<7>(NdsRtc::RegDateTime);
        const std::array<uint8_t, 7> expected{0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00};
        if (bytes != expected) return fail("default date/time did not read back as 2000-01-01");
    }

    /* Host time source, rollover across a year boundary. */
    {
        const NdsRtcDateTime start{25u, 12u, 31u, 3u, 23u, 59u, 58u};
        if (rtc.set_date_time(start) != Status::Ok) return fail("valid host date/time was rejected");
        rtc.advance_seconds(3u);
        const NdsRtcDateTime expected{26u, 1u, 1u, 4u, 0u, 0u, 1u};
        if (!same(rtc.date_time(), expected)) return fail("year rollover produced the wrong date/time");
        const auto bytes = wire.read_register<7>(NdsRtc::RegDateTime);
        const std::array<uint8_t, 7> wire_expected{0x26, 0x01, 0x01, 0x04, 0x00, 0x00, 0x01};
        if (bytes != wire_expected) return fail("BCD date/time read did not match the host clock");
    }

    /* Leap-year handling and the 99 -> 00 wrap. */
    {
        (void)rtc.set_date_time({24u, 2u, 28u, 0u, 23u, 59u, 59u});
        rtc.advance_seconds(1u);
        if (!same(rtc.date_time(), NdsRtcDateTime{24u, 2u, 29u, 1u, 0u, 0u, 0u})) {
            return fail("2024 leap day was skipped");
        }
        rtc.advance_seconds(86400u);
        if (!same(rtc.date_time(), NdsRtcDateTime{24u, 3u, 1u, 2u, 0u, 0u, 0u})) {
            return fail("leap day did not roll into March");
        }
        (void)rtc.set_date_time({99u, 12u, 31u, 6u, 23u, 59u, 59u});
        rtc.advance_seconds(1u);
        if (!same(rtc.date_time(), NdsRtcDateTime{0u, 1u, 1u, 0u, 0u, 0u, 0u})) {
            return fail("year 99 did not wrap to 00");
        }
        (void)rtc.set_date_time({1u, 1u, 1u, 0u, 0u, 0u, 0u});
        rtc.advance_seconds(366ull * 86400ull);
        if (!same(rtc.date_time(), NdsRtcDateTime{2u, 1u, 2u, 2u, 0u, 0u, 0u})) {
            return fail("multi-day advance did not accumulate correctly");
        }
    }

    /* Host values outside the chip's ranges are refused and change nothing. */
    {
        const NdsRtcDateTime before = rtc.date_time();
        if (rtc.set_date_time({0u, 2u, 30u, 0u, 0u, 0u, 0u}) != Status::Unsupported ||
            rtc.set_date_time({0u, 13u, 1u, 0u, 0u, 0u, 0u}) != Status::Unsupported ||
            rtc.set_date_time({0u, 1u, 1u, 7u, 0u, 0u, 0u}) != Status::Unsupported ||
            rtc.set_date_time({0u, 1u, 1u, 0u, 24u, 0u, 0u}) != Status::Unsupported ||
            rtc.set_date_time({100u, 1u, 1u, 0u, 0u, 0u, 0u}) != Status::Unsupported) {
            return fail("out-of-range host date/time was accepted");
        }
        if (!same(rtc.date_time(), before)) return fail("rejected host date/time changed the clock");
        if (rtc.last_diagnostic() == nullptr) return fail("rejection left no diagnostic");
    }

    /* 24-hour mode through the wire, then a time write. */
    wire.write_register<1>(NdsRtc::RegStatus1, {NdsRtc::Status1Hour24});
    if (!rtc.hour24()) return fail("status register 1 write did not enable 24-hour mode");
    wire.write_register<3>(NdsRtc::RegTime, {0x15, 0x30, 0x45});
    if (wire.last() != Status::Ok) return fail("valid time write was rejected");
    {
        const NdsRtcDateTime now = rtc.date_time();
        if (now.hour != 15u || now.minute != 30u || now.second != 45u) {
            return fail("time write did not update hour/minute/second");
        }
        const auto bytes = wire.read_register<3>(NdsRtc::RegTime);
        const std::array<uint8_t, 3> expected{0x55, 0x30, 0x45}; /* PM flag set */
        if (bytes != expected) return fail("time read did not include the PM flag");
    }

    /* 12-hour mode accepts hour + PM flag. */
    wire.write_register<1>(NdsRtc::RegStatus1, {0x00});
    wire.write_register<3>(NdsRtc::RegTime, {0x43, 0x10, 0x20}); /* 03 PM */
    if (rtc.date_time().hour != 15u) return fail("12-hour PM write did not map to 15:00");

    /* Invalid BCD is refused at the final bit and leaves the time alone. */
    {
        const NdsRtcDateTime before = rtc.date_time();
        wire.write_register<3>(NdsRtc::RegTime, {0x2A, 0x00, 0x00});
        if (wire.last() != Status::Unsupported) return fail("invalid BCD hour was accepted");
        if (!same(rtc.date_time(), before)) return fail("invalid BCD write changed the clock");
    }

    /* A command without the 0110b signature is rejected. */
    wire.begin();
    wire.send(0x07u);
    if (wire.last() != Status::Unsupported) return fail("bad command signature was accepted");
    wire.end();

    /* CS falling mid-transfer discards the partial write. */
    {
        const NdsRtcDateTime before = rtc.date_time();
        wire.begin();
        wire.send(Wire::command(NdsRtc::RegTime, false));
        wire.send(0x01u);
        wire.send(0x02u);
        wire.end();
        if (!same(rtc.date_time(), before)) return fail("an aborted write modified the clock");
    }

    /* Alarm and free registers hold written bytes. */
    wire.write_register<3>(NdsRtc::RegAlarm1, {0x03, 0x12, 0x34});
    if (wire.read_register<3>(NdsRtc::RegAlarm1) != std::array<uint8_t, 3>{0x03, 0x12, 0x34}) {
        return fail("alarm 1 register did not retain its bytes");
    }
    wire.write_register<1>(NdsRtc::RegFree, {0xA5});
    if (wire.read_register<1>(NdsRtc::RegFree)[0] != 0xA5u) {
        return fail("free register did not retain its byte");
    }

    /* Reset bit restores the default clock and registers. */
    wire.write_register<1>(NdsRtc::RegStatus1, {NdsRtc::Status1Reset});
    if (!same(rtc.date_time(), NdsRtcDateTime{}) ||
        wire.read_register<1>(NdsRtc::RegFree)[0] != 0u) {
        return fail("status register 1 reset did not clear the registers");
    }

    /* DATA reads the host's own bit while the CPU drives the line. */
    (void)rtc.write(NDS_ARM7, NdsRtc::RegisterAddress, 1u, 0x11u);
    if ((rtc.read(NDS_ARM7, NdsRtc::RegisterAddress, 1u).value & 1u) != 1u) {
        return fail("DATA did not echo the host-driven value");
    }
    /* Unmodeled bits 3 and 7 are not stored. */
    (void)rtc.write(NDS_ARM7, NdsRtc::RegisterAddress, 2u, 0xFFFFu);
    if (rtc.read(NDS_ARM7, NdsRtc::RegisterAddress, 2u).value != 0x77u) {
        return fail("register mask did not keep only bits 0-2 and 4-6");
    }

    /* Access control and width checks. */
    if (rtc.read(NDS_ARM9, NdsRtc::RegisterAddress, 1u).status != Status::CpuAccessDenied ||
        rtc.write(NDS_ARM9, NdsRtc::RegisterAddress, 1u, 0u) != Status::CpuAccessDenied) {
        return fail("ARM9 was allowed to reach the ARM7-only RTC register");
    }
    if (rtc.read(NDS_ARM7, NdsRtc::RegisterAddress, 4u).status != Status::InvalidWidth ||
        rtc.write(NDS_ARM7, NdsRtc::RegisterAddress, 4u, 0u) != Status::InvalidWidth) {
        return fail("32-bit RTC access was accepted");
    }
    if (rtc.read(NDS_ARM7, NdsRtc::RegisterAddress + 2u, 1u).status != Status::Unmapped ||
        rtc.read(static_cast<NdsCpu>(77), NdsRtc::RegisterAddress, 1u).status != Status::InvalidCpu) {
        return fail("unmapped address or invalid CPU was accepted");
    }

    std::puts("nds_rtc_test: ok");
    return 0;
}

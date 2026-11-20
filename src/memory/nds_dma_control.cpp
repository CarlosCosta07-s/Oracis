#include "oracis/memory/nds_dma_control.hpp"

namespace oracis::memory {

bool NdsDmaControl::valid_cpu(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 || cpu == NDS_ARM7;
}

unsigned NdsDmaControl::cpu_index(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 ? 0u : 1u;
}

bool NdsDmaControl::decode_address(
    uint32_t address, unsigned &channel, uint32_t &offset) noexcept
{
    if (address < FirstRegisterAddress || address > LastRegisterAddress) {
        return false;
    }
    const uint32_t relative = address - FirstRegisterAddress;
    channel = relative / 12u;
    offset = relative % 12u;
    return channel < ChannelCount;
}

uint32_t NdsDmaControl::count_mask(
    NdsCpu cpu, unsigned channel) noexcept
{
    if (cpu == NDS_ARM9) return UINT32_C(0x001FFFFF);
    return channel == 3u ? UINT32_C(0x0000FFFF)
                         : UINT32_C(0x00003FFF);
}

uint32_t NdsDmaControl::control_mask(NdsCpu cpu) noexcept
{
    /* ARM7 leaves CNT bits 16-20 and bit 27 unused. */
    return cpu == NDS_ARM9
        ? UINT32_MAX
        : UINT32_C(0xF7E0FFFF);
}

uint32_t NdsDmaControl::transfer_count(
    NdsCpu cpu, unsigned channel, uint32_t programmed_count) noexcept
{
    const uint32_t masked = programmed_count & count_mask(cpu, channel);
    if (masked != 0u) return masked;
    if (cpu == NDS_ARM9) return UINT32_C(0x00200000);
    return channel == 3u ? UINT32_C(0x00010000)
                         : UINT32_C(0x00004000);
}

uint32_t NdsDmaControl::start_timing(
    NdsCpu cpu, uint32_t control) noexcept
{
    return cpu == NDS_ARM9
        ? ((control >> 27u) & 7u)
        : ((control >> 28u) & 3u);
}

ReadResult NdsDmaControl::read(
    NdsCpu cpu, uint32_t address, uint32_t width) noexcept
{
    if (!valid_cpu(cpu)) return {Status::InvalidCpu, 0u};

    if (address >= FirstFillAddress &&
        address < FirstFillAddress + FillRegisterBytes) {
        if (cpu != NDS_ARM9) return {Status::Unmapped, 0u};
        const uint32_t fill_offset = address - FirstFillAddress;
        if (width != 4u || (fill_offset & 3u) != 0u) {
            return {Status::InvalidWidth, 0u};
        }
        const unsigned fill_channel =
            static_cast<unsigned>(fill_offset / 4u);
        return {Status::Ok, channels_[0][fill_channel].fill_data};
    }

    unsigned channel_index = 0u;
    uint32_t offset = 0u;
    if (!decode_address(address, channel_index, offset)) {
        return {Status::Unmapped, 0u};
    }

    Channel &channel = channels_[cpu_index(cpu)][channel_index];
    if (offset == 0u || offset == 4u) {
        if (width != 4u) return {Status::InvalidWidth, 0u};
        return {Status::Ok, offset == 0u
            ? channel.source : channel.destination};
    }

    if (offset != 8u && offset != 10u) {
        return {Status::Unmapped, 0u};
    }
    if (width == 4u && offset == 8u) {
        return {Status::Ok, channel.control};
    }
    if (width != 2u) return {Status::InvalidWidth, 0u};

    const uint32_t shift = offset == 8u ? 0u : 16u;
    return {Status::Ok, (channel.control >> shift) & UINT32_C(0xFFFF)};
}

Status NdsDmaControl::execute_transfer(
    NdsCpu cpu, unsigned channel_index, Channel &state,
    bool immediate) noexcept
{
    const uint32_t control = state.control;
    const uint32_t source_mode =
        (control >> SourceControlShift) & 3u;
    const uint32_t destination_mode =
        (control >> DestinationControlShift) & 3u;
    if (source_mode == 3u) return Status::PermissionDenied;

    const uint32_t width =
        (control & WordTransferMask) != 0u ? 4u : 2u;
    const uint32_t count = transfer_count(
        cpu, channel_index, control);
    const uint64_t transfer_bytes =
        static_cast<uint64_t>(count) * width;

    const uint32_t address_limit =
        cpu == NDS_ARM9 ? UINT32_C(0x0FFFFFFE)
                        : UINT32_C(0x07FFFFFE);
    if ((state.current_source % width) != 0u ||
        (state.current_destination % width) != 0u) {
        return Status::InvalidRegion;
    }

    const auto get_span = [width, count, transfer_bytes, address_limit](
        uint32_t start, uint32_t mode,
        uint32_t &base, uint32_t &size) noexcept -> bool {
        const uint64_t unit_offset =
            static_cast<uint64_t>(count - 1u) * width;
        const uint64_t span_bytes = mode == 2u ? width : transfer_bytes;
        uint64_t low = start;
        uint64_t last_unit = start;
        if (mode == 1u) {
            if (static_cast<uint64_t>(start) < unit_offset) return false;
            low = static_cast<uint64_t>(start) - unit_offset;
        } else if (mode != 2u) {
            last_unit = static_cast<uint64_t>(start) + unit_offset;
        }
        if (start > address_limit || last_unit > address_limit ||
            low + span_bytes > UINT64_C(0x1'0000'0000)) {
            return false;
        }
        base = static_cast<uint32_t>(low);
        size = static_cast<uint32_t>(span_bytes);
        return true;
    };

    uint32_t source_base = 0u;
    uint32_t source_span = 0u;
    uint32_t destination_base = 0u;
    uint32_t destination_span = 0u;
    if (!get_span(state.current_source, source_mode, source_base, source_span) ||
        !get_span(state.current_destination, destination_mode,
                  destination_base, destination_span)) {
        return Status::AddressOverflow;
    }

    const bool fill_source = cpu == NDS_ARM9 && source_mode == 2u &&
        state.current_source ==
            FirstFillAddress + channel_index * UINT32_C(4);
    if (!fill_source) {
        const Status source_status =
            memory_.validate_read(cpu, source_base, source_span);
        if (source_status != Status::Ok) return source_status;
    }
    const Status destination_status =
        memory_.validate_write(cpu, destination_base, destination_span);
    if (destination_status != Status::Ok) return destination_status;

    uint32_t source = state.current_source;
    uint32_t destination = state.current_destination;
    for (uint32_t index = 0u; index < count; ++index) {
        const ReadResult value = fill_source
            ? ReadResult{
                Status::Ok,
                width == 2u
                    ? (state.fill_data & UINT32_C(0xFFFF))
                    : state.fill_data
            }
            : memory_.read(cpu, source, width);
        if (!value) return value.status;
        const Status write_status =
            memory_.write(cpu, destination, width, value.value);
        if (write_status != Status::Ok) return write_status;

        if (source_mode == 0u) source += width;
        else if (source_mode == 1u) source -= width;

        if (destination_mode == 0u || destination_mode == 3u) {
            destination += width;
        } else if (destination_mode == 1u) {
            destination -= width;
        }
    }

    const bool repeat = !immediate && (control & RepeatMask) != 0u;
    state.current_source = source;
    /* Increment/reload reloads DAD for the next repeated transfer only. */
    state.current_destination = repeat && destination_mode == 3u
        ? state.destination : destination;

    if (!repeat) state.control &= ~EnableMask;
    if ((control & IrqEnableMask) != 0u) {
        (void)interrupts_.request_irq(
            cpu, UINT32_C(1) << (8u + channel_index));
    }
    return Status::Ok;
}

Status NdsDmaControl::trigger_event(
    NdsCpu cpu, uint32_t timing) noexcept
{
    if (!valid_cpu(cpu)) return Status::InvalidCpu;
    Status first_error = Status::Ok;
    for (unsigned index = 0u; index < ChannelCount; ++index) {
        Channel &channel = channels_[cpu_index(cpu)][index];
        if ((channel.control & EnableMask) == 0u ||
            start_timing(cpu, channel.control) != timing) {
            continue;
        }
        const Status result = execute_transfer(cpu, index, channel, false);
        if (result != Status::Ok) {
            /* A failing event-triggered channel is disarmed, never retried blindly. */
            channel.control &= ~EnableMask;
            if (first_error == Status::Ok) first_error = result;
        }
    }
    return first_error;
}

Status NdsDmaControl::trigger_vblank(NdsCpu cpu) noexcept
{
    return trigger_event(cpu, 1u);
}

Status NdsDmaControl::trigger_hblank(NdsCpu cpu) noexcept
{
    if (!valid_cpu(cpu)) return Status::InvalidCpu;
    if (cpu != NDS_ARM9) return Status::Ok;
    return trigger_event(cpu, 2u);
}

Status NdsDmaControl::trigger_display_sync(NdsCpu cpu) noexcept
{
    if (!valid_cpu(cpu)) return Status::InvalidCpu;
    /* ARM7 timing 3 is channel-specific wireless/GBA-slot DMA, not LCD sync. */
    if (cpu != NDS_ARM9) return Status::Ok;
    return trigger_event(cpu, 3u);
}

Status NdsDmaControl::write(
    NdsCpu cpu, uint32_t address,
    uint32_t width, uint32_t value) noexcept
{
    if (!valid_cpu(cpu)) return Status::InvalidCpu;

    if (address >= FirstFillAddress &&
        address < FirstFillAddress + FillRegisterBytes) {
        if (cpu != NDS_ARM9) return Status::Unmapped;
        const uint32_t fill_offset = address - FirstFillAddress;
        if (width != 4u || (fill_offset & 3u) != 0u) {
            return Status::InvalidWidth;
        }
        const unsigned fill_channel =
            static_cast<unsigned>(fill_offset / 4u);
        channels_[0][fill_channel].fill_data = value;
        return Status::Ok;
    }

    unsigned channel_index = 0u;
    uint32_t offset = 0u;
    if (!decode_address(address, channel_index, offset)) {
        return Status::Unmapped;
    }

    Channel &channel = channels_[cpu_index(cpu)][channel_index];
    if (offset == 0u || offset == 4u) {
        if (width != 4u) return Status::InvalidWidth;
        if (offset == 0u) channel.source = value;
        else channel.destination = value;
        return Status::Ok;
    }

    if (offset != 8u && offset != 10u) {
        return Status::Unmapped;
    }

    const uint32_t previous_control = channel.control;
    uint32_t candidate = previous_control;
    if (width == 4u && offset == 8u) {
        candidate = value;
    } else if (width == 2u) {
        const uint32_t shift = offset == 8u ? 0u : 16u;
        const uint32_t mask = UINT32_C(0xFFFF) << shift;
        candidate = (candidate & ~mask) |
            ((value & UINT32_C(0xFFFF)) << shift);
    } else {
        return Status::InvalidWidth;
    }
    candidate &= control_mask(cpu);

    const bool enable_rising =
        (previous_control & EnableMask) == 0u &&
        (candidate & EnableMask) != 0u;
    channel.control = candidate;
    if (!enable_rising) return Status::Ok;

    const uint32_t previous_source = channel.current_source;
    const uint32_t previous_destination = channel.current_destination;
    /* Hardware snapshots SAD/DAD when software raises the enable bit. */
    channel.current_source = channel.source;
    channel.current_destination = channel.destination;

    if (start_timing(cpu, candidate) != 0u) return Status::Ok;

    const Status result = execute_transfer(
        cpu, channel_index, channel, true);
    if (result != Status::Ok) {
        channel.control = previous_control;
        channel.current_source = previous_source;
        channel.current_destination = previous_destination;
        return result;
    }
    return Status::Ok;
}

} // namespace oracis::memory
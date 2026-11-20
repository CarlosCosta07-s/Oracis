#include "oracis/memory/nds_ipc_control.hpp"

namespace oracis::memory {

bool NdsIpcControl::valid_cpu(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 || cpu == NDS_ARM7;
}

unsigned NdsIpcControl::cpu_index(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 ? 0u : 1u;
}

NdsCpu NdsIpcControl::cpu_from_index(unsigned index) noexcept
{
    return index == 0u ? NDS_ARM9 : NDS_ARM7;
}

uint16_t NdsIpcControl::read_fifo_control(unsigned index) const noexcept
{
    const unsigned peer = 1u - index;
    const Fifo &send = outgoing_[index];
    const Fifo &receive = outgoing_[peer];
    const CpuControl &control = controls_[index];

    uint16_t value = 0u;
    if (send.count == 0u) value |= UINT16_C(1) << 0u;
    if (send.count == FifoCapacity) value |= UINT16_C(1) << 1u;
    if (control.send_empty_irq_enabled) value |= SendEmptyIrqEnableMask;
    if (receive.count == 0u) value |= UINT16_C(1) << 8u;
    if (receive.count == FifoCapacity) value |= UINT16_C(1) << 9u;
    if (control.receive_not_empty_irq_enabled) {
        value |= ReceiveNotEmptyIrqEnableMask;
    }
    if (control.error) value |= ErrorMask;
    if (control.fifo_enabled) value |= FifoEnableMask;
    return value;
}

std::array<NdsIpcControl::IrqConditions, 2>
NdsIpcControl::capture_irq_conditions() const noexcept
{
    std::array<IrqConditions, 2> result{};
    for (unsigned index = 0u; index < result.size(); ++index) {
        const CpuControl &control = controls_[index];
        result[index].send_empty =
            control.send_empty_irq_enabled && outgoing_[index].count == 0u;
        result[index].receive_not_empty =
            control.receive_not_empty_irq_enabled &&
            outgoing_[1u - index].count != 0u;
    }
    return result;
}

void NdsIpcControl::notify_irq_edges(
    const std::array<IrqConditions, 2> &before) noexcept
{
    const auto after = capture_irq_conditions();
    for (unsigned index = 0u; index < after.size(); ++index) {
        const NdsCpu cpu = cpu_from_index(index);
        if (!before[index].send_empty && after[index].send_empty) {
            (void)interrupts_.request_irq(cpu, SendEmptyIrqMask);
        }
        if (!before[index].receive_not_empty &&
            after[index].receive_not_empty) {
            (void)interrupts_.request_irq(cpu, ReceiveNotEmptyIrqMask);
        }
    }
}

ReadResult NdsIpcControl::read(
    NdsCpu cpu, uint32_t address, uint32_t width) noexcept
{
    if (!valid_cpu(cpu)) return {Status::InvalidCpu, 0u};
    const unsigned index = cpu_index(cpu);

    switch (address) {
    case SyncAddress:
        if (width != 2u) return {Status::InvalidWidth, 0u};
        return {
            Status::Ok,
            static_cast<uint32_t>(
                static_cast<uint16_t>(controls_[1u - index].sync_output) |
                (static_cast<uint16_t>(controls_[index].sync_output) << 8u) |
                (controls_[index].sync_irq_enabled
                    ? SyncReceiveIrqEnableMask : 0u))
        };

    case FifoControlAddress:
        if (width != 2u) return {Status::InvalidWidth, 0u};
        return {Status::Ok, read_fifo_control(index)};

    case FifoSendAddress:
        if (width != 4u) return {Status::InvalidWidth, 0u};
        return {Status::PermissionDenied, 0u};

    case FifoReceiveAddress: {
        if (width != 4u) return {Status::InvalidWidth, 0u};
        CpuControl &control = controls_[index];
        Fifo &receive = outgoing_[1u - index];
        if (receive.count == 0u) {
            control.error = true;
            return {Status::Ok, control.last_received};
        }

        const uint32_t value = receive.words[receive.head];
        if (control.fifo_enabled) {
            const auto before = capture_irq_conditions();
            receive.head = static_cast<uint8_t>(
                (receive.head + 1u) % FifoCapacity);
            --receive.count;
            notify_irq_edges(before);
        }
        return {Status::Ok, value};
    }

    default:
        return {Status::Unmapped, 0u};
    }
}

Status NdsIpcControl::write(
    NdsCpu cpu, uint32_t address, uint32_t width, uint32_t value) noexcept
{
    if (!valid_cpu(cpu)) return Status::InvalidCpu;
    const unsigned index = cpu_index(cpu);

    switch (address) {
    case SyncAddress: {
        if (width != 2u) return Status::InvalidWidth;
        CpuControl &control = controls_[index];
        control.sync_output = static_cast<uint8_t>((value >> 8u) & 0x0Fu);
        control.sync_irq_enabled =
            (value & SyncReceiveIrqEnableMask) != 0u;
        const unsigned peer = 1u - index;
        if ((value & SyncSendIrqMask) != 0u &&
            controls_[peer].sync_irq_enabled) {
            (void)interrupts_.request_irq(
                cpu_from_index(peer), SyncIrqMask);
        }
        return Status::Ok;
    }

    case FifoControlAddress: {
        if (width != 2u) return Status::InvalidWidth;
        const auto before = capture_irq_conditions();
        CpuControl &control = controls_[index];

        control.send_empty_irq_enabled =
            (value & SendEmptyIrqEnableMask) != 0u;
        control.receive_not_empty_irq_enabled =
            (value & ReceiveNotEmptyIrqEnableMask) != 0u;
        control.fifo_enabled = (value & FifoEnableMask) != 0u;
        if ((value & ErrorMask) != 0u) control.error = false;

        if ((value & ClearSendFifoMask) != 0u) {
            outgoing_[index] = {};
            controls_[1u - index].last_received = 0u;
        }

        notify_irq_edges(before);
        return Status::Ok;
    }

    case FifoSendAddress: {
        if (width != 4u) return Status::InvalidWidth;
        CpuControl &control = controls_[index];
        if (!control.fifo_enabled) return Status::Ok;

        Fifo &send = outgoing_[index];
        if (send.count == FifoCapacity) {
            control.error = true;
            return Status::Ok;
        }

        const auto before = capture_irq_conditions();
        const uint8_t tail = static_cast<uint8_t>(
            (send.head + send.count) % FifoCapacity);
        send.words[tail] = value;
        ++send.count;
        controls_[1u - index].last_received = value;
        notify_irq_edges(before);
        return Status::Ok;
    }

    case FifoReceiveAddress:
        return width == 4u ? Status::PermissionDenied : Status::InvalidWidth;

    default:
        return Status::Unmapped;
    }
}

} // namespace oracis::memory

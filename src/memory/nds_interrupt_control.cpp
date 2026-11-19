#include "oracis/memory/nds_interrupt_control.hpp"

namespace oracis::memory {

bool NdsInterruptControl::valid_cpu(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 || cpu == NDS_ARM7;
}

unsigned NdsInterruptControl::cpu_index(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 ? 0u : 1u;
}

ReadResult NdsInterruptControl::read(
    NdsCpu cpu,
    uint32_t address,
    uint32_t width) const noexcept
{
    if (address != ImeAddress &&
        address != IeAddress &&
        address != IfAddress) {
        return {Status::Unmapped, 0u};
    }
    if (!valid_cpu(cpu)) return {Status::InvalidCpu, 0u};
    if (width != 4u) return {Status::InvalidWidth, 0u};

    const CpuRegisters &registers = cpu_registers_[cpu_index(cpu)];
    switch (address) {
    case ImeAddress:
        return {Status::Ok, registers.ime & UINT32_C(1)};
    case IeAddress:
        return {Status::Ok, registers.ie};
    case IfAddress:
        return {Status::Ok, registers.flags};
    default:
        return {Status::Unmapped, 0u};
    }
}

Status NdsInterruptControl::write(
    NdsCpu cpu,
    uint32_t address,
    uint32_t width,
    uint32_t value) noexcept
{
    if (address != ImeAddress &&
        address != IeAddress &&
        address != IfAddress) {
        return Status::Unmapped;
    }
    if (!valid_cpu(cpu)) return Status::InvalidCpu;
    if (width != 4u) return Status::InvalidWidth;

    CpuRegisters &registers = cpu_registers_[cpu_index(cpu)];
    switch (address) {
    case ImeAddress:
        /* Only bit zero is implemented by the retail DS. */
        registers.ime = value & UINT32_C(1);
        return Status::Ok;
    case IeAddress:
        registers.ie = value;
        return Status::Ok;
    case IfAddress:
        /* IF is write-one-to-clear: zero leaves a request unchanged. */
        registers.flags &= ~value;
        return Status::Ok;
    default:
        return Status::Unmapped;
    }
}

bool NdsInterruptControl::request_irq(
    NdsCpu cpu, uint32_t mask) noexcept
{
    if (!valid_cpu(cpu)) return false;
    cpu_registers_[cpu_index(cpu)].flags |= mask;
    return true;
}

uint32_t NdsInterruptControl::pending_enabled(NdsCpu cpu) const noexcept
{
    if (!valid_cpu(cpu)) return 0u;
    const CpuRegisters &registers = cpu_registers_[cpu_index(cpu)];
    return registers.ie & registers.flags;
}

bool NdsInterruptControl::irq_asserted(NdsCpu cpu) const noexcept
{
    if (!valid_cpu(cpu)) return false;
    const CpuRegisters &registers = cpu_registers_[cpu_index(cpu)];
    return (registers.ime & UINT32_C(1)) != 0u &&
           (registers.ie & registers.flags) != 0u;
}

} // namespace oracis::memory

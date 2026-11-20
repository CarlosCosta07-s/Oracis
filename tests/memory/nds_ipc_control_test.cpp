#include "oracis/memory/guest_memory_abi.h"
#include "oracis/memory/nds_interrupt_control.hpp"
#include "oracis/memory/nds_ipc_control.hpp"
#include "oracis/recomp/runtime_arm.h"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_ipc_control_test: %s\n", message);
    return 1;
}

constexpr uint32_t irq_mask =
    oracis::memory::NdsIpcControl::SyncIrqMask |
    oracis::memory::NdsIpcControl::SendEmptyIrqMask |
    oracis::memory::NdsIpcControl::ReceiveNotEmptyIrqMask;

} // namespace

int main()
{
    using oracis::memory::GuestMemory;
    using oracis::memory::NdsInterruptControl;
    using oracis::memory::NdsIpcControl;
    using oracis::memory::Status;

    GuestMemory memory;
    NdsInterruptControl interrupts(memory);
    NdsIpcControl ipc(interrupts);

    if (ipc.read(NDS_ARM9, NdsIpcControl::FifoControlAddress, 2u).value !=
            UINT32_C(0x0101) ||
        ipc.read(NDS_ARM7, NdsIpcControl::FifoControlAddress, 2u).value !=
            UINT32_C(0x0101)) {
        return fail("both CPUs did not begin with empty send and receive FIFOs");
    }

    if (ipc.read(NDS_ARM9, NdsIpcControl::SyncAddress, 1u).status !=
            Status::InvalidWidth ||
        ipc.read(NDS_ARM9, NdsIpcControl::FifoSendAddress, 4u).status !=
            Status::PermissionDenied ||
        ipc.write(NDS_ARM9, NdsIpcControl::FifoReceiveAddress, 4u, 1u) !=
            Status::PermissionDenied ||
        ipc.write(NDS_ARM9, NdsIpcControl::FifoControlAddress, 4u, 0u) !=
            Status::InvalidWidth ||
        ipc.read(static_cast<NdsCpu>(77),
                 NdsIpcControl::SyncAddress, 2u).status != Status::InvalidCpu ||
        ipc.read(NDS_ARM9, NdsIpcControl::FifoReceiveAddress + 4u, 4u).status !=
            Status::Unmapped) {
        return fail("invalid IPC register access was accepted");
    }

    ArmCpuState arm9{};
    ArmCpuState arm7{};
    arm9.cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm7.cpsr = ORACIS_ARM_MODE_SYSTEM;
    if (!oracis_arm_runtime_bind_cpus(&arm9, &arm7) ||
        !oracis_bus_bind_memory(&memory) ||
        !oracis_bus_bind_interrupt_control(&interrupts) ||
        !oracis_bus_bind_ipc_control(&ipc)) {
        return fail("IPC bus binding failed");
    }
    if (oracis_bus_bind_ipc_control(&ipc) ||
        oracis_bus_unbind_interrupt_control(&interrupts) ||
        oracis_bus_unbind_memory(&memory)) {
        return fail("duplicate IPC binding or premature dependency teardown was accepted");
    }

    /* IPCSYNC transfers four status bits and gates the remote sync IRQ. */
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7)) {
        return fail("ARM7 selection failed for IPCSYNC test");
    }
    bus_write_u32(NdsInterruptControl::ImeAddress, 1u);
    bus_write_u32(NdsInterruptControl::IeAddress, irq_mask);
    bus_write_u16(NdsIpcControl::SyncAddress,
                  NdsIpcControl::SyncReceiveIrqEnableMask);

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 selection failed for IPCSYNC test");
    }
    bus_write_u32(NdsInterruptControl::ImeAddress, 1u);
    bus_write_u32(NdsInterruptControl::IeAddress, irq_mask);
    bus_write_u16(NdsIpcControl::SyncAddress,
                  static_cast<uint16_t>((UINT16_C(0xA) << 8u) |
                      NdsIpcControl::SyncSendIrqMask));
    if ((bus_read_u16(NdsIpcControl::SyncAddress) & UINT16_C(0x0F00)) !=
            UINT16_C(0x0A00)) {
        return fail("IPCSYNC did not retain ARM9's local output nibble");
    }
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7) ||
        (bus_read_u16(NdsIpcControl::SyncAddress) & UINT16_C(0x000F)) != 0xAu ||
        bus_read_u32(NdsInterruptControl::IfAddress) !=
            NdsIpcControl::SyncIrqMask) {
        return fail("ARM7 did not receive ARM9 IPCSYNC data and IRQ");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 selection failed for reverse IPCSYNC test");
    }
    bus_write_u16(NdsIpcControl::SyncAddress,
                  NdsIpcControl::SyncReceiveIrqEnableMask);
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7)) {
        return fail("ARM7 selection failed for reverse IPCSYNC test");
    }
    bus_write_u16(NdsIpcControl::SyncAddress,
                  static_cast<uint16_t>((UINT16_C(0x5) << 8u) |
                      NdsIpcControl::SyncSendIrqMask));
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9) ||
        (bus_read_u16(NdsIpcControl::SyncAddress) & UINT16_C(0x000F)) != 0x5u ||
        (bus_read_u32(NdsInterruptControl::IfAddress) &
            NdsIpcControl::SyncIrqMask) == 0u) {
        return fail("reverse IPCSYNC transfer did not reach ARM9");
    }

    /* Enable both FIFO interfaces and their edge-triggered IRQ sources. */
    bus_write_u32(NdsInterruptControl::IfAddress, irq_mask);
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 selection failed for FIFO setup");
    }
    bus_write_u16(NdsIpcControl::FifoControlAddress,
        static_cast<uint16_t>(NdsIpcControl::FifoEnableMask |
            NdsIpcControl::SendEmptyIrqEnableMask |
            NdsIpcControl::ReceiveNotEmptyIrqEnableMask |
            NdsIpcControl::ClearSendFifoMask));
    if ((bus_read_u32(NdsInterruptControl::IfAddress) &
            NdsIpcControl::SendEmptyIrqMask) == 0u) {
        return fail("enabling send-empty IRQ while empty did not latch its rising edge");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM7)) {
        return fail("ARM7 selection failed for FIFO setup");
    }
    bus_write_u16(NdsIpcControl::FifoControlAddress,
        static_cast<uint16_t>(NdsIpcControl::FifoEnableMask |
            NdsIpcControl::SendEmptyIrqEnableMask |
            NdsIpcControl::ReceiveNotEmptyIrqEnableMask |
            NdsIpcControl::ClearSendFifoMask));
    bus_write_u32(NdsInterruptControl::IfAddress, irq_mask);

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 selection failed for FIFO send");
    }
    bus_write_u32(NdsIpcControl::FifoSendAddress, UINT32_C(0x11223344));
    if ((bus_read_u16(NdsIpcControl::FifoControlAddress) & 1u) != 0u) {
        return fail("ARM9 send FIFO remained empty after a write");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM7) ||
        (bus_read_u16(NdsIpcControl::FifoControlAddress) &
            (UINT16_C(1) << 8u)) != 0u ||
        (bus_read_u32(NdsInterruptControl::IfAddress) &
            NdsIpcControl::ReceiveNotEmptyIrqMask) == 0u) {
        return fail("ARM7 receive-not-empty state or IRQ edge was not raised");
    }
    bus_write_u32(NdsInterruptControl::IfAddress, irq_mask);
    if (bus_read_u32(NdsIpcControl::FifoReceiveAddress) !=
            UINT32_C(0x11223344)) {
        return fail("ARM7 did not receive the ARM9 FIFO word");
    }
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9) ||
        (bus_read_u32(NdsInterruptControl::IfAddress) &
            NdsIpcControl::SendEmptyIrqMask) == 0u) {
        return fail("consuming the final word did not raise ARM9 send-empty IRQ");
    }
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7) ||
        (bus_read_u16(NdsIpcControl::FifoControlAddress) &
            (UINT16_C(1) << 8u)) == 0u ||
        (bus_read_u32(NdsInterruptControl::IfAddress) &
            NdsIpcControl::ReceiveNotEmptyIrqMask) != 0u) {
        return fail("reading the final word did not empty ARM7 receive FIFO");
    }

    /* The reverse queue is independent and uses the same register window. */
    bus_write_u32(NdsIpcControl::FifoSendAddress, UINT32_C(0xAABBCCDD));
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9) ||
        bus_read_u32(NdsIpcControl::FifoReceiveAddress) !=
            UINT32_C(0xAABBCCDD)) {
        return fail("ARM9 did not receive the ARM7 FIFO word");
    }

    /* Sixteen words fit; the seventeenth sets error without corrupting data. */
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 selection failed for FIFO capacity test");
    }
    bus_write_u32(NdsInterruptControl::IfAddress, irq_mask);
    for (uint32_t index = 0u; index < NdsIpcControl::FifoCapacity; ++index) {
        bus_write_u32(NdsIpcControl::FifoSendAddress,
                      UINT32_C(0xC0000000) + index);
    }
    bus_write_u32(NdsIpcControl::FifoSendAddress, UINT32_C(0xDEADBEEF));
    const uint16_t arm9_full = bus_read_u16(NdsIpcControl::FifoControlAddress);
    if ((arm9_full & (UINT16_C(1) << 1u)) == 0u ||
        (arm9_full & NdsIpcControl::ErrorMask) == 0u) {
        return fail("FIFO full status or overflow error was not reported");
    }
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7) ||
        (bus_read_u16(NdsIpcControl::FifoControlAddress) &
            (UINT16_C(1) << 9u)) == 0u) {
        return fail("peer receive FIFO did not report full");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 selection failed for FIFO clear test");
    }
    bus_write_u16(NdsIpcControl::FifoControlAddress,
        static_cast<uint16_t>(NdsIpcControl::FifoEnableMask |
            NdsIpcControl::SendEmptyIrqEnableMask |
            NdsIpcControl::ReceiveNotEmptyIrqEnableMask |
            NdsIpcControl::ClearSendFifoMask |
            NdsIpcControl::ErrorMask));
    if ((bus_read_u16(NdsIpcControl::FifoControlAddress) &
            (NdsIpcControl::ErrorMask | (UINT16_C(1) << 1u))) != 0u) {
        return fail("W1C error acknowledge or sender FIFO clear failed");
    }
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7) ||
        (bus_read_u16(NdsIpcControl::FifoControlAddress) &
            (UINT16_C(1) << 8u)) == 0u ||
        bus_read_u32(NdsIpcControl::FifoReceiveAddress) != 0u ||
        (bus_read_u16(NdsIpcControl::FifoControlAddress) &
            NdsIpcControl::ErrorMask) == 0u) {
        return fail("clear did not empty peer receive FIFO and reset empty-read value");
    }

    /* Disabled receiver reads without popping; re-enable resumes normal pop. */
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7)) {
        return fail("ARM7 selection failed for disabled-read test");
    }
    bus_write_u16(NdsIpcControl::FifoControlAddress,
                  NdsIpcControl::ReceiveNotEmptyIrqEnableMask);
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 selection failed for disabled-read send");
    }
    bus_write_u32(NdsIpcControl::FifoSendAddress, UINT32_C(0x55667788));
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7) ||
        bus_read_u32(NdsIpcControl::FifoReceiveAddress) !=
            UINT32_C(0x55667788) ||
        bus_read_u32(NdsIpcControl::FifoReceiveAddress) !=
            UINT32_C(0x55667788)) {
        return fail("disabled receive FIFO read did not preserve its head word");
    }
    bus_write_u16(NdsIpcControl::FifoControlAddress,
        static_cast<uint16_t>(NdsIpcControl::FifoEnableMask |
            NdsIpcControl::ReceiveNotEmptyIrqEnableMask));
    if (bus_read_u32(NdsIpcControl::FifoReceiveAddress) !=
            UINT32_C(0x55667788) ||
        (bus_read_u16(NdsIpcControl::FifoControlAddress) &
            (UINT16_C(1) << 8u)) == 0u) {
        return fail("re-enabling receive FIFO did not consume the queued word");
    }
    if (bus_read_u32(NdsIpcControl::FifoReceiveAddress) !=
            UINT32_C(0x55667788) ||
        (bus_read_u16(NdsIpcControl::FifoControlAddress) &
            NdsIpcControl::ErrorMask) == 0u) {
        return fail("empty FIFO did not return the last word and set error");
    }

    /* Disabled sender writes are ignored and do not set the error bit. */
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9)) {
        return fail("ARM9 selection failed for disabled-send test");
    }
    bus_write_u16(NdsIpcControl::FifoControlAddress, 0u);
    bus_write_u32(NdsIpcControl::FifoSendAddress, UINT32_C(0xCAFEBABE));
    if ((bus_read_u16(NdsIpcControl::FifoControlAddress) &
            (UINT16_C(1) | NdsIpcControl::ErrorMask)) != UINT16_C(1)) {
        return fail("disabled FIFO send was not ignored without error");
    }

    if (!oracis_bus_unbind_ipc_control(&ipc) ||
        !oracis_bus_unbind_interrupt_control(&interrupts) ||
        !oracis_bus_unbind_memory(&memory) ||
        !oracis_arm_runtime_unbind_cpus(&arm9, &arm7)) {
        return fail("IPC bus teardown failed");
    }

    std::puts("NDS IPCSYNC and bidirectional FIFO tests passed");
    return 0;
}

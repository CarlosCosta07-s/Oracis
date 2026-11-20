/*
 * Nintendo DS ARM9/ARM7 IPC synchronization and FIFO device.
 *
 * The two directions use independent 16-word queues. Register state is
 * processor-local while each outgoing queue is visible as the peer's input.
 */
#ifndef ORACIS_MEMORY_NDS_IPC_CONTROL_HPP
#define ORACIS_MEMORY_NDS_IPC_CONTROL_HPP

#include <array>
#include <cstdint>

#include "oracis/memory/nds_interrupt_control.hpp"

namespace oracis::memory {

class NdsIpcControl final {
public:
    static constexpr uint32_t SyncAddress = UINT32_C(0x04000180);
    static constexpr uint32_t FifoControlAddress = UINT32_C(0x04000184);
    static constexpr uint32_t FifoSendAddress = UINT32_C(0x04000188);
    static constexpr uint32_t FifoReceiveAddress = UINT32_C(0x04100000);

    static constexpr uint32_t FifoCapacity = 16u;

    static constexpr uint16_t SyncSendIrqMask = UINT16_C(1) << 13u;
    static constexpr uint16_t SyncReceiveIrqEnableMask = UINT16_C(1) << 14u;

    static constexpr uint16_t SendEmptyIrqEnableMask = UINT16_C(1) << 2u;
    static constexpr uint16_t ClearSendFifoMask = UINT16_C(1) << 3u;
    static constexpr uint16_t ReceiveNotEmptyIrqEnableMask = UINT16_C(1) << 10u;
    static constexpr uint16_t ErrorMask = UINT16_C(1) << 14u;
    static constexpr uint16_t FifoEnableMask = UINT16_C(1) << 15u;

    static constexpr uint32_t SyncIrqMask = UINT32_C(1) << 16u;
    static constexpr uint32_t SendEmptyIrqMask = UINT32_C(1) << 17u;
    static constexpr uint32_t ReceiveNotEmptyIrqMask = UINT32_C(1) << 18u;

    explicit NdsIpcControl(NdsInterruptControl &interrupts) noexcept
        : interrupts_(interrupts) {}

    NdsIpcControl(const NdsIpcControl &) = delete;
    NdsIpcControl &operator=(const NdsIpcControl &) = delete;

    ReadResult read(NdsCpu cpu, uint32_t address, uint32_t width) noexcept;
    Status write(NdsCpu cpu, uint32_t address,
                 uint32_t width, uint32_t value) noexcept;

    NdsInterruptControl &interrupt_control() noexcept { return interrupts_; }

private:
    struct Fifo {
        std::array<uint32_t, FifoCapacity> words{};
        uint8_t head = 0u;
        uint8_t count = 0u;
    };

    struct CpuControl {
        uint8_t sync_output = 0u;
        bool sync_irq_enabled = false;
        bool send_empty_irq_enabled = false;
        bool receive_not_empty_irq_enabled = false;
        bool fifo_enabled = false;
        bool error = false;
        uint32_t last_received = 0u;
    };

    struct IrqConditions {
        bool send_empty = false;
        bool receive_not_empty = false;
    };

    static bool valid_cpu(NdsCpu cpu) noexcept;
    static unsigned cpu_index(NdsCpu cpu) noexcept;
    static NdsCpu cpu_from_index(unsigned index) noexcept;

    uint16_t read_fifo_control(unsigned index) const noexcept;
    std::array<IrqConditions, 2> capture_irq_conditions() const noexcept;
    void notify_irq_edges(
        const std::array<IrqConditions, 2> &before) noexcept;

    NdsInterruptControl &interrupts_;
    std::array<CpuControl, 2> controls_{};
    /* outgoing_[0] is ARM9 -> ARM7; outgoing_[1] is ARM7 -> ARM9. */
    std::array<Fifo, 2> outgoing_{};
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_IPC_CONTROL_HPP */

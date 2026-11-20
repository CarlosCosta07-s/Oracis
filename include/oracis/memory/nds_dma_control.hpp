/*
 * Nintendo DS general-purpose DMA controller.
 *
 * Four independent channels are provided to each CPU. This first runtime
 * slice executes immediate transfers between mapped guest-memory regions.
 * Event-triggered modes remain armed until their event sources are integrated.
 */
#ifndef ORACIS_MEMORY_NDS_DMA_CONTROL_HPP
#define ORACIS_MEMORY_NDS_DMA_CONTROL_HPP

#include <array>
#include <cstdint>

#include "oracis/memory/guest_memory.hpp"
#include "oracis/memory/nds_interrupt_control.hpp"

namespace oracis::memory {

class NdsDmaControl final {
public:
    static constexpr uint32_t ChannelCount = 4u;
    static constexpr uint32_t FirstRegisterAddress = UINT32_C(0x040000B0);
    static constexpr uint32_t LastRegisterAddress = UINT32_C(0x040000DF);

    static constexpr uint32_t DestinationControlShift = 21u;
    static constexpr uint32_t SourceControlShift = 23u;
    static constexpr uint32_t RepeatMask = UINT32_C(1) << 25u;
    static constexpr uint32_t WordTransferMask = UINT32_C(1) << 26u;
    static constexpr uint32_t IrqEnableMask = UINT32_C(1) << 30u;
    static constexpr uint32_t EnableMask = UINT32_C(1) << 31u;

    explicit NdsDmaControl(
        GuestMemory &memory,
        NdsInterruptControl &interrupts) noexcept
        : memory_(memory), interrupts_(interrupts) {}

    NdsDmaControl(const NdsDmaControl &) = delete;
    NdsDmaControl &operator=(const NdsDmaControl &) = delete;

    ReadResult read(NdsCpu cpu, uint32_t address,
                    uint32_t width) noexcept;
    Status write(NdsCpu cpu, uint32_t address,
                 uint32_t width, uint32_t value) noexcept;

    GuestMemory &backing_memory() noexcept { return memory_; }
    NdsInterruptControl &interrupt_control() noexcept { return interrupts_; }

private:
    struct Channel {
        uint32_t source = 0u;
        uint32_t destination = 0u;
        uint32_t control = 0u;
    };

    static bool valid_cpu(NdsCpu cpu) noexcept;
    static unsigned cpu_index(NdsCpu cpu) noexcept;
    static bool decode_address(
        uint32_t address, unsigned &channel, uint32_t &offset) noexcept;
    static uint32_t count_mask(NdsCpu cpu, unsigned channel) noexcept;
    static uint32_t control_mask(NdsCpu cpu) noexcept;
    static uint32_t transfer_count(
        NdsCpu cpu, unsigned channel, uint32_t programmed_count) noexcept;
    static uint32_t start_timing(NdsCpu cpu, uint32_t control) noexcept;

    Status execute_immediate(
        NdsCpu cpu, unsigned channel, Channel &state) noexcept;

    GuestMemory &memory_;
    NdsInterruptControl &interrupts_;
    std::array<std::array<Channel, ChannelCount>, 2> channels_{};
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_DMA_CONTROL_HPP */

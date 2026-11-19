/*
 * Nintendo DS interrupt controller register block.
 *
 * ARM9 and ARM7 have independent IME/IE/IF state. Hardware event sources
 * assert IF bits through request_irq(); software acknowledges requests by
 * writing one bits to IF. IRQ exception entry remains the machine scheduler's
 * responsibility.
 */
#ifndef ORACIS_MEMORY_NDS_INTERRUPT_CONTROL_HPP
#define ORACIS_MEMORY_NDS_INTERRUPT_CONTROL_HPP

#include <array>
#include <cstdint>

#include "oracis/memory/guest_memory.hpp"

namespace oracis::memory {

class NdsInterruptControl final {
public:
    explicit NdsInterruptControl(GuestMemory &memory) noexcept
        : memory_(memory) {}

    NdsInterruptControl(const NdsInterruptControl &) = delete;
    NdsInterruptControl &operator=(const NdsInterruptControl &) = delete;

    ReadResult read(NdsCpu cpu,
                    uint32_t address,
                    uint32_t width) const noexcept;

    Status write(NdsCpu cpu,
                 uint32_t address,
                 uint32_t width,
                 uint32_t value) noexcept;

    /* Called by hardware devices when an interrupt source becomes active. */
    bool request_irq(NdsCpu cpu, uint32_t mask) noexcept;

    /* True when IME is enabled and at least one enabled IF bit is pending. */
    bool irq_asserted(NdsCpu cpu) const noexcept;

    /*
     * Enter IRQ for the supplied CPU state if its IRQ line is asserted and
     * CPSR.I is clear. The scheduler supplies the architectural return address
     * and selected exception vector base; IF remains pending until guest code
     * acknowledges it through IF.
     */
    bool try_enter_irq(NdsCpu cpu,
                       ArmCpuState &state,
                       uint32_t return_address,
                       uint32_t vector_base) const noexcept;

    uint32_t pending_enabled(NdsCpu cpu) const noexcept;

    GuestMemory &backing_memory() const noexcept { return memory_; }

    static constexpr uint32_t ImeAddress = UINT32_C(0x04000208);
    static constexpr uint32_t IeAddress  = UINT32_C(0x04000210);
    static constexpr uint32_t IfAddress  = UINT32_C(0x04000214);

private:
    struct CpuRegisters {
        uint32_t ime = 0u;
        uint32_t ie = 0u;
        uint32_t flags = 0u;
    };

    static bool valid_cpu(NdsCpu cpu) noexcept;
    static unsigned cpu_index(NdsCpu cpu) noexcept;

    GuestMemory &memory_;
    std::array<CpuRegisters, 2> cpu_registers_{};
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_INTERRUPT_CONTROL_HPP */

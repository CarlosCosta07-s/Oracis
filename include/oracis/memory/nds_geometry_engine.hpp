/*
 * Nintendo DS Geometry Engine command FIFO and matrix command processor.
 *
 * This first hardware slice implements command intake, FIFO status/IRQ,
 * command timing for documented GX commands, and the matrix command family.
 * Vertex submission and polygon rasterization are separate subsystems.
 */
#ifndef ORACIS_MEMORY_NDS_GEOMETRY_ENGINE_HPP
#define ORACIS_MEMORY_NDS_GEOMETRY_ENGINE_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include "oracis/memory/guest_memory.hpp"
#include "oracis/memory/nds_interrupt_control.hpp"

namespace oracis::memory {

class NdsGeometryEngine final {
public:
    static constexpr uint32_t GxFifoBase = UINT32_C(0x04000400);
    static constexpr uint32_t GxFifoMirrorEnd = UINT32_C(0x04000440);
    static constexpr uint32_t CommandPortsBase = UINT32_C(0x04000440);
    static constexpr uint32_t CommandPortsEnd = UINT32_C(0x04000600);
    static constexpr uint32_t GxStatAddress = UINT32_C(0x04000600);
    static constexpr uint32_t RamCountAddress = UINT32_C(0x04000604);
    static constexpr uint32_t ClipMatrixBase = UINT32_C(0x04000640);
    static constexpr uint32_t VectorMatrixBase = UINT32_C(0x04000680);
    static constexpr uint32_t CommandFifoCapacity = 256u;
    static constexpr uint32_t GxFifoIrqMask = UINT32_C(1) << 21u;

    using Matrix = std::array<int32_t, 16>;

    explicit NdsGeometryEngine(NdsInterruptControl &interrupts) noexcept
        : interrupts_(interrupts) {}
    NdsGeometryEngine(const NdsGeometryEngine &) = delete;
    NdsGeometryEngine &operator=(const NdsGeometryEngine &) = delete;

    Status install() noexcept;
    bool installed() const noexcept { return installed_; }
    NdsInterruptControl &interrupt_control() const noexcept {
        return interrupts_;
    }

    ReadResult read(NdsCpu cpu, uint32_t address, uint32_t width) const noexcept;
    Status write(NdsCpu cpu, uint32_t address,
                 uint32_t width, uint32_t value) noexcept;

    /* Advance the command processor in the common 33.51 MHz bus-clock domain. */
    void advance_bus_cycles(uint64_t cycles) noexcept;

    /* Reassert level-sensitive GX FIFO IRQ after software writes IF. */
    void refresh_fifo_irq() noexcept;

    uint32_t fifo_entries() const noexcept { return fifo_count_; }
    bool busy() const noexcept;
    bool matrix_stack_error() const noexcept { return matrix_stack_error_; }
    uint8_t matrix_mode() const noexcept { return matrix_mode_; }
    uint8_t position_stack_pointer() const noexcept {
        return static_cast<uint8_t>(position_stack_pointer_ & 0x1Fu);
    }
    uint8_t projection_stack_pointer() const noexcept {
        return projection_stack_pointer_;
    }
    const Matrix &position_matrix() const noexcept { return position_matrix_; }
    const Matrix &directional_matrix() const noexcept { return directional_matrix_; }
    const Matrix &projection_matrix() const noexcept { return projection_matrix_; }
    const Matrix &texture_matrix() const noexcept { return texture_matrix_; }
    uint64_t completed_commands() const noexcept { return completed_commands_; }

    static Matrix identity_matrix() noexcept;

private:
    struct FifoEntry {
        uint8_t command = 0u;
        uint32_t parameter = 0u;
    };

    struct InputCommand {
        uint8_t command = 0u;
        uint8_t parameter_count = 0u;
    };

    struct CommandInfo {
        uint8_t parameter_count = 0u;
        uint16_t cycles = 1u;
        bool valid = false;
    };

    static bool valid_cpu(NdsCpu cpu) noexcept;
    static CommandInfo command_info(uint8_t command) noexcept;
    static uint16_t command_cycles(uint8_t command, uint8_t matrix_mode) noexcept;
    static int32_t signed_fixed(uint32_t value) noexcept;
    static Matrix matrix_from_parameters(
        uint8_t command, const uint32_t *parameters) noexcept;
    static Matrix multiply(const Matrix &left, const Matrix &right) noexcept;

    bool fifo_push(uint8_t command, uint32_t parameter) noexcept;
    bool fifo_peek(uint32_t offset, FifoEntry &entry) const noexcept;
    bool fifo_pop(FifoEntry &entry) noexcept;
    Status write_gxfifo(uint32_t value) noexcept;
    Status write_command_port(uint8_t command, uint32_t value) noexcept;
    Status enqueue_command(uint8_t command, uint32_t parameter) noexcept;
    Status parse_command_word(uint32_t value) noexcept;
    void advance_input_commands() noexcept;
    void update_fifo_irq() noexcept;
    uint32_t status_register() const noexcept;

    bool begin_next_command() noexcept;
    void complete_active_command() noexcept;
    void execute_active_command() noexcept;
    void execute_matrix_command() noexcept;
    void set_current_matrix(const Matrix &matrix) noexcept;
    void multiply_current_matrix(const Matrix &matrix,
                                 bool position_only = false) noexcept;
    void refresh_result_matrices() noexcept;
    void stack_push() noexcept;
    void stack_pop(uint32_t parameter) noexcept;
    void stack_store(uint32_t parameter) noexcept;
    void stack_restore(uint32_t parameter) noexcept;

    NdsInterruptControl &interrupts_;
    std::array<FifoEntry, CommandFifoCapacity> fifo_{};
    uint16_t fifo_head_ = 0u;
    uint16_t fifo_count_ = 0u;

    std::array<InputCommand, 4> input_commands_{};
    uint8_t input_command_count_ = 0u;
    uint8_t input_command_index_ = 0u;
    uint8_t input_parameters_remaining_ = 0u;

    std::array<uint32_t, 32> active_parameters_{};
    uint8_t active_command_ = 0u;
    uint8_t active_parameter_count_ = 0u;
    uint16_t active_cycles_remaining_ = 0u;
    bool active_command_valid_ = false;
    bool matrix_stack_busy_ = false;

    uint8_t matrix_mode_ = 0u;
    uint8_t projection_stack_pointer_ = 0u;
    uint8_t texture_stack_pointer_ = 0u;
    uint8_t position_stack_pointer_ = 0u;
    uint8_t fifo_irq_mode_ = 0u;
    bool matrix_stack_error_ = false;

    Matrix projection_matrix_{};
    Matrix position_matrix_{};
    Matrix directional_matrix_{};
    Matrix texture_matrix_{};
    Matrix clip_matrix_{};
    std::array<Matrix, 1> projection_stack_{};
    std::array<Matrix, 32> position_stack_{};
    std::array<Matrix, 32> directional_stack_{};
    std::array<Matrix, 1> texture_stack_{};

    uint64_t completed_commands_ = 0u;
    bool installed_ = false;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_GEOMETRY_ENGINE_HPP */

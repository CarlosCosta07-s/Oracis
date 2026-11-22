#include "oracis/memory/nds_geometry_engine.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace oracis::memory {
namespace {

constexpr uint32_t kMatrixModeMask = UINT32_C(3);
constexpr uint32_t kStackOffsetMask = UINT32_C(0x3F);
constexpr uint32_t kStackAddressMask = UINT32_C(0x1F);
constexpr int32_t kFixedOne = INT32_C(1) << 12u;
constexpr uint32_t kPositionIrqBit = UINT32_C(1) << 8u;
constexpr uint32_t kProjectionIrqBit = UINT32_C(1) << 13u;
constexpr uint32_t kMatrixStackBusyBit = UINT32_C(1) << 14u;
constexpr uint32_t kMatrixStackErrorBit = UINT32_C(1) << 15u;
constexpr uint32_t kFifoHalfFullBit = UINT32_C(1) << 25u;
constexpr uint32_t kFifoEmptyBit = UINT32_C(1) << 26u;
constexpr uint32_t kGeometryBusyBit = UINT32_C(1) << 27u;
constexpr uint32_t kFifoIrqShift = 30u;

int32_t low_signed_word(uint64_t value) noexcept
{
    return std::bit_cast<int32_t>(static_cast<uint32_t>(value));
}

/* Computes floor(magnitude * numerator / denominator) without overflowing. */
uint64_t multiply_divide_fraction(
    uint64_t magnitude, uint64_t numerator, uint64_t denominator) noexcept
{
    if (denominator == 0u || numerator == 0u || magnitude == 0u) return 0u;
    if (numerator >= denominator) return magnitude;

    uint64_t quotient = 0u;
    uint64_t remainder = 0u;
    for (int bit = 63; bit >= 0; --bit) {
        quotient <<= 1u;
        remainder <<= 1u;
        if (((magnitude >> static_cast<unsigned>(bit)) & 1u) != 0u) {
            remainder += numerator;
        }
        while (remainder >= denominator) {
            remainder -= denominator;
            ++quotient;
        }
    }
    return quotient;
}

uint64_t absolute_value(int64_t value) noexcept
{
    return value < 0
        ? static_cast<uint64_t>(-value)
        : static_cast<uint64_t>(value);
}

int32_t interpolate_i32(
    int32_t from, int32_t to,
    uint64_t numerator, uint64_t denominator) noexcept
{
    const int64_t delta =
        static_cast<int64_t>(to) - static_cast<int64_t>(from);
    const int64_t adjustment = static_cast<int64_t>(
        multiply_divide_fraction(
            absolute_value(delta), numerator, denominator));
    const int64_t value = static_cast<int64_t>(from) +
        (delta < 0 ? -adjustment : adjustment);
    return static_cast<int32_t>(std::clamp<int64_t>(
        value, std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::max()));
}

int16_t interpolate_i16(
    int16_t from, int16_t to,
    uint64_t numerator, uint64_t denominator) noexcept
{
    const int64_t delta =
        static_cast<int64_t>(to) - static_cast<int64_t>(from);
    const int64_t adjustment = static_cast<int64_t>(
        multiply_divide_fraction(
            absolute_value(delta), numerator, denominator));
    const int64_t value = static_cast<int64_t>(from) +
        (delta < 0 ? -adjustment : adjustment);
    return static_cast<int16_t>(std::clamp<int64_t>(
        value, std::numeric_limits<int16_t>::min(),
        std::numeric_limits<int16_t>::max()));
}

} // namespace

bool NdsGeometryEngine::valid_cpu(NdsCpu cpu) noexcept
{
    return cpu == NDS_ARM9 || cpu == NDS_ARM7;
}

NdsGeometryEngine::CommandInfo NdsGeometryEngine::command_info(
    uint8_t command) noexcept
{
    switch (command) {
    case 0x10u: return {1u, 1u, true};
    case 0x11u: return {0u, 17u, true};
    case 0x12u: return {1u, 36u, true};
    case 0x13u: return {1u, 17u, true};
    case 0x14u: return {1u, 36u, true};
    case 0x15u: return {0u, 19u, true};
    case 0x16u: return {16u, 34u, true};
    case 0x17u: return {12u, 30u, true};
    case 0x18u: return {16u, 35u, true};
    case 0x19u: return {12u, 31u, true};
    case 0x1Au: return {9u, 28u, true};
    case 0x1Bu: return {3u, 22u, true};
    case 0x1Cu: return {3u, 22u, true};

    case 0x20u: return {1u, 1u, true};
    case 0x21u: return {1u, 9u, true};
    case 0x22u: return {1u, 1u, true};
    case 0x23u: return {2u, 9u, true};
    case 0x24u:
    case 0x25u:
    case 0x26u:
    case 0x27u:
    case 0x28u: return {1u, 8u, true};
    case 0x29u:
    case 0x2Au:
    case 0x2Bu: return {1u, 1u, true};

    case 0x30u:
    case 0x31u: return {1u, 4u, true};
    case 0x32u: return {1u, 6u, true};
    case 0x33u: return {1u, 1u, true};
    case 0x34u: return {32u, 32u, true};

    case 0x40u: return {1u, 1u, true};
    case 0x41u: return {0u, 1u, true};
    case 0x50u: return {1u, 392u, true};
    case 0x60u: return {1u, 1u, true};
    case 0x70u: return {3u, 103u, true};
    case 0x71u: return {2u, 9u, true};
    case 0x72u: return {1u, 5u, true};
    default: return {};
    }
}

uint16_t NdsGeometryEngine::command_cycles(
    uint8_t command, uint8_t matrix_mode) noexcept
{
    const CommandInfo info = command_info(command);
    if (!info.valid) return 1u;

    uint16_t cycles = info.cycles;
    if (matrix_mode == 2u &&
        (command == 0x18u || command == 0x19u ||
         command == 0x1Au || command == 0x1Cu)) {
        cycles = static_cast<uint16_t>(cycles + 30u);
    }
    return cycles;
}

int32_t NdsGeometryEngine::signed_fixed(uint32_t value) noexcept
{
    return std::bit_cast<int32_t>(value);
}

int32_t NdsGeometryEngine::sign_extend(
    uint32_t value, unsigned width) noexcept
{
    if (width == 0u || width >= 32u) {
        return std::bit_cast<int32_t>(value);
    }
    const uint32_t mask = (UINT32_C(1) << width) - 1u;
    const uint32_t sign = UINT32_C(1) << (width - 1u);
    const uint32_t raw = value & mask;
    const int64_t extended = (raw & sign) != 0u
        ? static_cast<int64_t>(raw) -
            (INT64_C(1) << width)
        : static_cast<int64_t>(raw);
    return static_cast<int32_t>(extended);
}

NdsGeometryEngine::Matrix NdsGeometryEngine::identity_matrix() noexcept
{
    Matrix matrix{};
    matrix[0] = kFixedOne;
    matrix[5] = kFixedOne;
    matrix[10] = kFixedOne;
    matrix[15] = kFixedOne;
    return matrix;
}

NdsGeometryEngine::Matrix NdsGeometryEngine::matrix_from_parameters(
    uint8_t command, const uint32_t *parameters) noexcept
{
    Matrix matrix = identity_matrix();
    if (parameters == nullptr) return matrix;

    if (command == 0x16u || command == 0x18u) {
        for (unsigned index = 0u; index < 16u; ++index) {
            matrix[index] = signed_fixed(parameters[index]);
        }
        return matrix;
    }

    if (command == 0x17u || command == 0x19u) {
        matrix = {};
        for (unsigned index = 0u; index < 3u; ++index) {
            matrix[index] = signed_fixed(parameters[index]);
            matrix[4u + index] = signed_fixed(parameters[3u + index]);
            matrix[8u + index] = signed_fixed(parameters[6u + index]);
            matrix[12u + index] = signed_fixed(parameters[9u + index]);
        }
        matrix[15] = kFixedOne;
        return matrix;
    }

    if (command == 0x1Au) {
        matrix = {};
        matrix[0] = signed_fixed(parameters[0]);
        matrix[1] = signed_fixed(parameters[1]);
        matrix[2] = signed_fixed(parameters[2]);
        matrix[4] = signed_fixed(parameters[3]);
        matrix[5] = signed_fixed(parameters[4]);
        matrix[6] = signed_fixed(parameters[5]);
        matrix[8] = signed_fixed(parameters[6]);
        matrix[9] = signed_fixed(parameters[7]);
        matrix[10] = signed_fixed(parameters[8]);
        matrix[15] = kFixedOne;
        return matrix;
    }

    if (command == 0x1Bu) {
        matrix[0] = signed_fixed(parameters[0]);
        matrix[5] = signed_fixed(parameters[1]);
        matrix[10] = signed_fixed(parameters[2]);
        return matrix;
    }

    if (command == 0x1Cu) {
        matrix[12] = signed_fixed(parameters[0]);
        matrix[13] = signed_fixed(parameters[1]);
        matrix[14] = signed_fixed(parameters[2]);
        return matrix;
    }

    return matrix;
}

NdsGeometryEngine::Matrix NdsGeometryEngine::multiply(
    const Matrix &left, const Matrix &right) noexcept
{
    Matrix result{};
    for (unsigned row = 0u; row < 4u; ++row) {
        for (unsigned column = 0u; column < 4u; ++column) {
            uint64_t accumulator = 0u;
            for (unsigned inner = 0u; inner < 4u; ++inner) {
                const int64_t product =
                    static_cast<int64_t>(left[row * 4u + inner]) *
                    static_cast<int64_t>(right[inner * 4u + column]);
                accumulator += static_cast<uint64_t>(product);
            }
            const int64_t signed_accumulator =
                std::bit_cast<int64_t>(accumulator);
            result[row * 4u + column] =
                low_signed_word(static_cast<uint64_t>(
                    signed_accumulator >> 12u));
        }
    }
    return result;
}

Status NdsGeometryEngine::install() noexcept
{
    if (installed_) return Status::InvalidRegion;

    fifo_ = {};
    fifo_head_ = 0u;
    fifo_count_ = 0u;
    pipe_ = {};
    pipe_head_ = 0u;
    pipe_count_ = 0u;
    input_commands_ = {};
    input_command_count_ = 0u;
    input_command_index_ = 0u;
    input_parameters_remaining_ = 0u;
    active_parameters_ = {};
    active_command_ = 0u;
    active_parameter_count_ = 0u;
    active_cycles_remaining_ = 0u;
    active_command_valid_ = false;
    matrix_stack_busy_ = false;
    matrix_mode_ = 0u;
    projection_stack_pointer_ = 0u;
    texture_stack_pointer_ = 0u;
    position_stack_pointer_ = 0u;
    fifo_irq_mode_ = 0u;
    matrix_stack_error_ = false;
    projection_matrix_ = identity_matrix();
    position_matrix_ = identity_matrix();
    directional_matrix_ = identity_matrix();
    texture_matrix_ = identity_matrix();
    clip_matrix_ = identity_matrix();
    viewport_x1_ = 0u;
    viewport_y1_ = 0u;
    viewport_x2_ = 255u;
    viewport_y2_ = 191u;
    projection_stack_ = {};
    position_stack_ = {};
    directional_stack_ = {};
    texture_stack_ = {};
    current_vertex_position_ = {};
    current_vertex_normal_ = {};
    current_vertex_color_ = UINT16_C(0x7FFF);
    current_texture_s_ = 0;
    current_texture_t_ = 0;
    buffers_ = {};
    build_buffer_index_ = 0u;
    render_buffer_index_ = 1u;
    swap_waiting_vblank_ = false;
    swap_lockup_ = false;
    swap_buffer_clear_cycles_ = 0u;
    swap_parameters_ = 0u;
    pending_polygon_attributes_ = 0u;
    active_polygon_attributes_ = 0u;
    primitive_mode_ = 0u;
    vertex_list_open_ = false;
    incomplete_polygon_list_ = false;
    vertex_list_vertex_count_ = 0u;
    primitive_vertices_ = {};
    primitive_vertex_count_ = 0u;
    completed_commands_ = 0u;
    installed_ = true;
    update_fifo_irq();
    return Status::Ok;
}

bool NdsGeometryEngine::busy() const noexcept
{
    return fifo_count_ != 0u || pipe_count_ != 0u ||
        active_command_valid_ || swap_lockup_;
}

bool NdsGeometryEngine::fifo_push(
    uint8_t command, uint32_t parameter) noexcept
{
    const FifoEntry entry{command, parameter};
    /* Direct PIPE writes are possible only while FIFO is empty. */
    if (fifo_count_ == 0u && pipe_count_ < CommandPipeCapacity) {
        const uint8_t tail = static_cast<uint8_t>(
            (static_cast<uint32_t>(pipe_head_) + pipe_count_) %
            CommandPipeCapacity);
        pipe_[tail] = entry;
        ++pipe_count_;
        return true;
    }
    if (fifo_count_ >= CommandFifoCapacity) return false;
    const uint16_t tail = static_cast<uint16_t>(
        (static_cast<uint32_t>(fifo_head_) + fifo_count_) %
        CommandFifoCapacity);
    fifo_[tail] = entry;
    ++fifo_count_;
    update_fifo_irq();
    return true;
}

bool NdsGeometryEngine::fifo_peek(
    uint32_t offset, FifoEntry &entry) const noexcept
{
    const uint32_t queued =
        static_cast<uint32_t>(pipe_count_) + fifo_count_;
    if (offset >= queued) return false;
    if (offset < pipe_count_) {
        const uint8_t index = static_cast<uint8_t>(
            (static_cast<uint32_t>(pipe_head_) + offset) %
            CommandPipeCapacity);
        entry = pipe_[index];
        return true;
    }
    const uint32_t fifo_offset = offset - pipe_count_;
    const uint16_t index = static_cast<uint16_t>(
        (static_cast<uint32_t>(fifo_head_) + fifo_offset) %
        CommandFifoCapacity);
    entry = fifo_[index];
    return true;
}

void NdsGeometryEngine::refill_pipe() noexcept
{
    if (pipe_count_ > 2u || fifo_count_ == 0u) return;
    uint8_t moved = 0u;
    while (moved < 2u && pipe_count_ < CommandPipeCapacity &&
           fifo_count_ != 0u) {
        const uint8_t tail = static_cast<uint8_t>(
            (static_cast<uint32_t>(pipe_head_) + pipe_count_) %
            CommandPipeCapacity);
        pipe_[tail] = fifo_[fifo_head_];
        fifo_head_ = static_cast<uint16_t>(
            (static_cast<uint32_t>(fifo_head_) + 1u) %
            CommandFifoCapacity);
        --fifo_count_;
        ++pipe_count_;
        ++moved;
    }
    if (moved != 0u) update_fifo_irq();
}

bool NdsGeometryEngine::fifo_pop(FifoEntry &entry) noexcept
{
    if (pipe_count_ == 0u) refill_pipe();
    if (pipe_count_ == 0u) return false;
    entry = pipe_[pipe_head_];
    pipe_head_ = static_cast<uint8_t>(
        (static_cast<uint32_t>(pipe_head_) + 1u) %
        CommandPipeCapacity);
    --pipe_count_;
    refill_pipe();
    return true;
}

void NdsGeometryEngine::advance_input_commands() noexcept
{
    while (input_command_index_ < input_command_count_ &&
           input_parameters_remaining_ == 0u) {
        const InputCommand command =
            input_commands_[input_command_index_];
        if (!fifo_push(command.command, 0u)) {
            matrix_stack_error_ = true;
            return;
        }
        ++input_command_index_;
        if (input_command_index_ < input_command_count_) {
            input_parameters_remaining_ =
                input_commands_[input_command_index_].parameter_count;
        }
    }

    if (input_command_index_ >= input_command_count_) {
        input_commands_ = {};
        input_command_count_ = 0u;
        input_command_index_ = 0u;
        input_parameters_remaining_ = 0u;
    }
}

Status NdsGeometryEngine::parse_command_word(uint32_t value) noexcept
{
    if (input_command_count_ != 0u) return Status::PermissionDenied;

    std::array<InputCommand, 4> parsed{};
    uint8_t parsed_count = 0u;
    uint32_t entry_reservation = 0u;

    const auto add = [&parsed, &parsed_count, &entry_reservation](
        uint8_t command) noexcept {
        if (command == 0u) return;
        const CommandInfo info = command_info(command);
        if (!info.valid || parsed_count >= parsed.size()) return;
        parsed[parsed_count++] = {command, info.parameter_count};
        entry_reservation += info.parameter_count == 0u
            ? 1u : info.parameter_count;
    };

    if ((value & UINT32_C(0xFFFFFF00)) == 0u) {
        add(static_cast<uint8_t>(value));
    } else {
        add(static_cast<uint8_t>(value & UINT32_C(0xFF)));
        add(static_cast<uint8_t>((value >> 8u) & UINT32_C(0xFF)));
        add(static_cast<uint8_t>((value >> 16u) & UINT32_C(0xFF)));
        add(static_cast<uint8_t>((value >> 24u) & UINT32_C(0xFF)));
    }

    if (parsed_count == 0u) return Status::Ok;
    const uint32_t total_queued =
        static_cast<uint32_t>(fifo_count_) + pipe_count_;
    constexpr uint32_t total_capacity =
        CommandFifoCapacity + CommandPipeCapacity;
    if (entry_reservation > total_capacity - total_queued) {
        return Status::PermissionDenied;
    }

    input_commands_ = parsed;
    input_command_count_ = parsed_count;
    input_command_index_ = 0u;
    input_parameters_remaining_ = parsed[0].parameter_count;
    advance_input_commands();
    return Status::Ok;
}

Status NdsGeometryEngine::write_gxfifo(uint32_t value) noexcept
{
    if (input_command_index_ < input_command_count_) {
        const InputCommand command =
            input_commands_[input_command_index_];
        if (input_parameters_remaining_ == 0u) {
            advance_input_commands();
            if (input_command_index_ >= input_command_count_) {
                return parse_command_word(value);
            }
        } else {
            if (!fifo_push(command.command, value)) {
                return Status::PermissionDenied;
            }
            --input_parameters_remaining_;
            if (input_parameters_remaining_ == 0u) {
                ++input_command_index_;
                if (input_command_index_ < input_command_count_) {
                    input_parameters_remaining_ =
                        input_commands_[input_command_index_].parameter_count;
                }
                advance_input_commands();
            }
            return Status::Ok;
        }
    }
    return parse_command_word(value);
}

Status NdsGeometryEngine::enqueue_command(
    uint8_t command, uint32_t parameter) noexcept
{
    const CommandInfo info = command_info(command);
    if (!info.valid) return Status::Unmapped;
    if (info.parameter_count == 0u) {
        return fifo_push(command, 0u) ? Status::Ok : Status::PermissionDenied;
    }
    return fifo_push(command, parameter)
        ? Status::Ok : Status::PermissionDenied;
}

Status NdsGeometryEngine::write_command_port(
    uint8_t command, uint32_t value) noexcept
{
    const CommandInfo info = command_info(command);
    if (!info.valid) return Status::Ok;
    return enqueue_command(command, value);
}

ReadResult NdsGeometryEngine::read(
    NdsCpu cpu, uint32_t address, uint32_t width) const noexcept
{
    if (address < GxStatAddress ||
        address >= VectorMatrixBase + UINT32_C(0x24)) {
        return {Status::Unmapped, 0u};
    }
    if (!installed_) return {Status::Unmapped, 0u};
    if (!valid_cpu(cpu)) return {Status::InvalidCpu, 0u};
    if (cpu != NDS_ARM9) return {Status::Unmapped, 0u};

    if (address == GxStatAddress) {
        if (width != 4u) return {Status::InvalidWidth, 0u};
        return {Status::Ok, status_register()};
    }
    if (address == RamCountAddress) {
        if (width != 4u) return {Status::InvalidWidth, 0u};
        const auto &buffer = buffers_[build_buffer_index_];
        const uint32_t vertices = buffer.vertex_count & UINT32_C(0x1FFF);
        const uint32_t polygons = buffer.polygon_count & UINT32_C(0x0FFF);
        return {Status::Ok, (vertices << 16u) | polygons};
    }
    if (address >= ClipMatrixBase &&
        address < ClipMatrixBase + UINT32_C(0x40)) {
        if (width != 4u || ((address - ClipMatrixBase) & 3u) != 0u) {
            return {Status::InvalidWidth, 0u};
        }
        if (busy()) return {Status::PermissionDenied, 0u};
        const unsigned index =
            static_cast<unsigned>((address - ClipMatrixBase) / 4u);
        return {Status::Ok, static_cast<uint32_t>(clip_matrix_[index])};
    }
    if (address >= VectorMatrixBase &&
        address < VectorMatrixBase + UINT32_C(0x24)) {
        if (width != 4u || ((address - VectorMatrixBase) & 3u) != 0u) {
            return {Status::InvalidWidth, 0u};
        }
        if (busy()) return {Status::PermissionDenied, 0u};
        const unsigned index =
            static_cast<unsigned>((address - VectorMatrixBase) / 4u);
        const unsigned row = index / 3u;
        const unsigned column = index % 3u;
        return {
            Status::Ok,
            static_cast<uint32_t>(
                directional_matrix_[row * 4u + column])
        };
    }
    return {Status::Unmapped, 0u};
}

Status NdsGeometryEngine::write(
    NdsCpu cpu, uint32_t address,
    uint32_t width, uint32_t value) noexcept
{
    if (!installed_) return Status::Unmapped;
    if (!valid_cpu(cpu)) return Status::InvalidCpu;
    if (cpu != NDS_ARM9) return Status::Unmapped;

    if (address >= GxFifoBase && address < GxFifoMirrorEnd) {
        if (width != 4u) return Status::InvalidWidth;
        return write_gxfifo(value);
    }

    if (address >= CommandPortsBase && address < CommandPortsEnd) {
        if (width != 4u) return Status::InvalidWidth;
        const uint8_t command = static_cast<uint8_t>(
            (address - GxFifoBase) / 4u);
        return write_command_port(command, value);
    }

    if (address == GxStatAddress) {
        if (width != 4u) return Status::InvalidWidth;
        fifo_irq_mode_ = static_cast<uint8_t>((value >> 30u) & 3u);
        if ((value & kMatrixStackErrorBit) != 0u) {
            matrix_stack_error_ = false;
            projection_stack_pointer_ = 0u;
            texture_stack_pointer_ = 0u;
        }
        update_fifo_irq();
        return Status::Ok;
    }

    if ((address >= RamCountAddress &&
         address < RamCountAddress + 4u) ||
        (address >= ClipMatrixBase &&
         address < VectorMatrixBase + UINT32_C(0x24))) {
        return Status::PermissionDenied;
    }
    return Status::Unmapped;
}

uint32_t NdsGeometryEngine::status_register() const noexcept
{
    uint32_t value = 0u;
    value |= static_cast<uint32_t>(position_stack_pointer_ & 0x1Fu)
        << 8u;
    value |= static_cast<uint32_t>(projection_stack_pointer_ & 1u)
        << 13u;
    if (matrix_stack_busy_) value |= kMatrixStackBusyBit;
    if (matrix_stack_error_) value |= kMatrixStackErrorBit;
    value |= static_cast<uint32_t>(fifo_count_ & 0x1FFu) << 16u;
    if (fifo_count_ < (CommandFifoCapacity / 2u)) {
        value |= kFifoHalfFullBit;
    }
    if (fifo_count_ == 0u) value |= kFifoEmptyBit;
    if (busy()) value |= kGeometryBusyBit;
    value |= static_cast<uint32_t>(fifo_irq_mode_ & 3u)
        << kFifoIrqShift;
    return value;
}

void NdsGeometryEngine::update_fifo_irq() noexcept
{
    bool condition = false;
    if (fifo_irq_mode_ == 1u) {
        condition = fifo_count_ < (CommandFifoCapacity / 2u);
    } else if (fifo_irq_mode_ == 2u) {
        condition = fifo_count_ == 0u;
    }
    if (condition) {
        static_cast<void>(interrupts_.request_irq(NDS_ARM9, GxFifoIrqMask));
    }
}

void NdsGeometryEngine::refresh_fifo_irq() noexcept
{
    if (installed_) update_fifo_irq();
}

bool NdsGeometryEngine::begin_next_command() noexcept
{
    if (active_command_valid_) return false;
    refill_pipe();
    if (pipe_count_ == 0u) return false;

    FifoEntry first{};
    if (!fifo_peek(0u, first)) return false;
    const CommandInfo info = command_info(first.command);
    if (!info.valid) {
        static_cast<void>(fifo_pop(first));
        matrix_stack_error_ = true;
        return false;
    }
    const uint32_t required = info.parameter_count == 0u
        ? 1u : info.parameter_count;
    if (static_cast<uint32_t>(pipe_count_) + fifo_count_ < required) {
        return false;
    }

    for (uint32_t index = 0u; index < required; ++index) {
        FifoEntry entry{};
        if (!fifo_peek(index, entry) || entry.command != first.command) {
            static_cast<void>(fifo_pop(first));
            matrix_stack_error_ = true;
            return false;
        }
    }

    active_parameter_count_ = info.parameter_count;
    active_parameters_ = {};
    for (uint32_t index = 0u; index < required; ++index) {
        FifoEntry entry{};
        static_cast<void>(fifo_pop(entry));
        if (info.parameter_count != 0u) {
            active_parameters_[index] = entry.parameter;
        }
    }

    active_command_ = first.command;
    active_cycles_remaining_ =
        command_cycles(active_command_, matrix_mode_);
    active_command_valid_ = true;
    if (active_command_ == 0x50u) {
        /* SwapBuffers halts GX processing until the next VBlank edge. */
        swap_waiting_vblank_ = true;
        active_cycles_remaining_ = 0u;
        if (has_incomplete_primitive()) {
            swap_lockup_ = true;
        }
    }
    matrix_stack_busy_ =
        active_command_ >= 0x11u && active_command_ <= 0x14u;
    update_fifo_irq();
    return true;
}

void NdsGeometryEngine::complete_active_command() noexcept
{
    execute_active_command();
    ++completed_commands_;
    active_command_valid_ = false;
    active_cycles_remaining_ = 0u;
    active_parameter_count_ = 0u;
    matrix_stack_busy_ = false;
    update_fifo_irq();
}

void NdsGeometryEngine::reset_build_buffer() noexcept
{
    buffers_[build_buffer_index_] = {};
}

void NdsGeometryEngine::notify_vblank() noexcept
{
    if (!installed_ || !active_command_valid_ ||
        active_command_ != 0x50u || !swap_waiting_vblank_) {
        return;
    }

    if (has_incomplete_primitive()) {
        swap_lockup_ = true;
        swap_waiting_vblank_ = false;
        return;
    }

    const uint8_t old_build = build_buffer_index_;
    build_buffer_index_ = render_buffer_index_;
    render_buffer_index_ = old_build;
    swap_parameters_ =
        static_cast<uint8_t>(active_parameters_[0] & UINT32_C(3));
    swap_buffer_clear_cycles_ = 10u;
    swap_waiting_vblank_ = false;
    vertex_list_open_ = false;
    incomplete_polygon_list_ = false;
    vertex_list_vertex_count_ = 0u;
    primitive_vertex_count_ = 0u;
    primitive_vertices_ = {};
    active_cycles_remaining_ = 392u;
}

void NdsGeometryEngine::advance_bus_cycles(uint64_t cycles) noexcept
{
    if (!installed_ || swap_lockup_) return;
    while (cycles != 0u) {
        if (!active_command_valid_) {
            if (!begin_next_command()) break;
        }

        /* A newly-started SWAP_BUFFERS command must stop here until VBlank. */
        if (swap_waiting_vblank_) break;

        if (active_command_valid_ && active_command_ == 0x50u) {
            const uint64_t step = std::min<uint64_t>(
                cycles, active_cycles_remaining_);
            cycles -= step;
            active_cycles_remaining_ = static_cast<uint16_t>(
                active_cycles_remaining_ - step);
            if (swap_buffer_clear_cycles_ != 0u) {
                const uint64_t clear_step = std::min<uint64_t>(
                    step, swap_buffer_clear_cycles_);
                swap_buffer_clear_cycles_ = static_cast<uint16_t>(
                    swap_buffer_clear_cycles_ - clear_step);
                if (swap_buffer_clear_cycles_ == 0u) {
                    reset_build_buffer();
                }
            }
            if (active_cycles_remaining_ == 0u) {
                complete_active_command();
            }
            continue;
        }

        if (cycles < active_cycles_remaining_) {
            active_cycles_remaining_ = static_cast<uint16_t>(
                active_cycles_remaining_ - cycles);
            cycles = 0u;
            break;
        }

        cycles -= active_cycles_remaining_;
        active_cycles_remaining_ = 0u;
        complete_active_command();
    }
}

void NdsGeometryEngine::set_current_matrix(const Matrix &matrix) noexcept
{
    switch (matrix_mode_) {
    case 0u:
        projection_matrix_ = matrix;
        break;
    case 1u:
        position_matrix_ = matrix;
        break;
    case 2u:
        position_matrix_ = matrix;
        directional_matrix_ = matrix;
        break;
    case 3u:
        texture_matrix_ = matrix;
        break;
    default:
        break;
    }
    refresh_result_matrices();
}

void NdsGeometryEngine::multiply_current_matrix(
    const Matrix &matrix, bool position_only) noexcept
{
    switch (matrix_mode_) {
    case 0u:
        projection_matrix_ = multiply(matrix, projection_matrix_);
        break;
    case 1u:
        position_matrix_ = multiply(matrix, position_matrix_);
        break;
    case 2u:
        position_matrix_ = multiply(matrix, position_matrix_);
        if (!position_only) {
            directional_matrix_ = multiply(matrix, directional_matrix_);
        }
        break;
    case 3u:
        texture_matrix_ = multiply(matrix, texture_matrix_);
        break;
    default:
        break;
    }
    refresh_result_matrices();
}

void NdsGeometryEngine::refresh_result_matrices() noexcept
{
    clip_matrix_ = multiply(position_matrix_, projection_matrix_);
}

void NdsGeometryEngine::stack_push() noexcept
{
    if (matrix_mode_ == 0u) {
        if (projection_stack_pointer_ != 0u) {
            matrix_stack_error_ = true;
            return;
        }
        projection_stack_[0] = projection_matrix_;
        projection_stack_pointer_ = 1u;
        return;
    }

    if (matrix_mode_ == 3u) {
        if (texture_stack_pointer_ != 0u) {
            matrix_stack_error_ = true;
            return;
        }
        texture_stack_[0] = texture_matrix_;
        texture_stack_pointer_ = 1u;
        return;
    }

    const uint8_t index = static_cast<uint8_t>(
        position_stack_pointer_ & 0x1Fu);
    position_stack_[index] = position_matrix_;
    directional_stack_[index] = directional_matrix_;
    if (index == 31u) matrix_stack_error_ = true;
    position_stack_pointer_ = static_cast<uint8_t>(
        (position_stack_pointer_ + 1u) & 0x3Fu);
}

void NdsGeometryEngine::stack_pop(uint32_t parameter) noexcept
{
    if (matrix_mode_ == 0u) {
        if (projection_stack_pointer_ == 0u) {
            matrix_stack_error_ = true;
            return;
        }
        projection_stack_pointer_ = 0u;
        projection_matrix_ = projection_stack_[0];
        refresh_result_matrices();
        return;
    }

    if (matrix_mode_ == 3u) {
        if (texture_stack_pointer_ == 0u) {
            matrix_stack_error_ = true;
            return;
        }
        texture_stack_pointer_ = 0u;
        texture_matrix_ = texture_stack_[0];
        return;
    }

    const int32_t offset = (parameter & kStackOffsetMask) >= 32u
        ? static_cast<int32_t>(parameter & kStackOffsetMask) - 64
        : static_cast<int32_t>(parameter & kStackOffsetMask);
    const int32_t next = static_cast<int32_t>(position_stack_pointer_) -
        offset;
    position_stack_pointer_ = static_cast<uint8_t>(
        static_cast<uint32_t>(next) & 0x3Fu);
    const uint8_t index = static_cast<uint8_t>(
        position_stack_pointer_ & 0x1Fu);
    if (index == 31u) matrix_stack_error_ = true;
    position_matrix_ = position_stack_[index];
    directional_matrix_ = directional_stack_[index];
    refresh_result_matrices();
}

void NdsGeometryEngine::stack_store(uint32_t parameter) noexcept
{
    if (matrix_mode_ == 0u) {
        projection_stack_[0] = projection_matrix_;
        return;
    }
    if (matrix_mode_ == 3u) {
        texture_stack_[0] = texture_matrix_;
        return;
    }

    const uint8_t index = static_cast<uint8_t>(
        parameter & kStackAddressMask);
    if (index == 31u) matrix_stack_error_ = true;
    position_stack_[index] = position_matrix_;
    directional_stack_[index] = directional_matrix_;
}

void NdsGeometryEngine::stack_restore(uint32_t parameter) noexcept
{
    if (matrix_mode_ == 0u) {
        projection_matrix_ = projection_stack_[0];
        refresh_result_matrices();
        return;
    }
    if (matrix_mode_ == 3u) {
        texture_matrix_ = texture_stack_[0];
        return;
    }

    const uint8_t index = static_cast<uint8_t>(
        parameter & kStackAddressMask);
    if (index == 31u) matrix_stack_error_ = true;
    position_matrix_ = position_stack_[index];
    directional_matrix_ = directional_stack_[index];
    refresh_result_matrices();
}

void NdsGeometryEngine::execute_matrix_command() noexcept
{
    const uint32_t parameter = active_parameters_[0];
    if (active_command_ == 0x10u) {
        matrix_mode_ = static_cast<uint8_t>(parameter & kMatrixModeMask);
        return;
    }
    if (active_command_ == 0x11u) {
        stack_push();
        return;
    }
    if (active_command_ == 0x12u) {
        stack_pop(parameter);
        return;
    }
    if (active_command_ == 0x13u) {
        stack_store(parameter);
        return;
    }
    if (active_command_ == 0x14u) {
        stack_restore(parameter);
        return;
    }
    if (active_command_ == 0x15u) {
        set_current_matrix(identity_matrix());
        return;
    }
    if (active_command_ == 0x16u || active_command_ == 0x17u) {
        set_current_matrix(matrix_from_parameters(
            active_command_, active_parameters_.data()));
        return;
    }
    if (active_command_ >= 0x18u && active_command_ <= 0x1Au) {
        multiply_current_matrix(matrix_from_parameters(
            active_command_, active_parameters_.data()));
        return;
    }
    if (active_command_ == 0x1Bu || active_command_ == 0x1Cu) {
        multiply_current_matrix(
            matrix_from_parameters(active_command_, active_parameters_.data()),
            active_command_ == 0x1Bu);
    }
}

void NdsGeometryEngine::execute_vertex_command() noexcept
{
    const uint32_t parameter = active_parameters_[0];
    switch (active_command_) {
    case 0x20u:
        current_vertex_color_ =
            static_cast<uint16_t>(parameter & UINT32_C(0x7FFF));
        return;

    case 0x21u: {
        const std::array<int32_t, 3> source_normal = {
            sign_extend(parameter, 10u) * 8,
            sign_extend(parameter >> 10u, 10u) * 8,
            sign_extend(parameter >> 20u, 10u) * 8
        };
        /* NORMAL uses a signed 1.9 vector and the Directional Matrix. */
        for (unsigned row = 0u; row < 3u; ++row) {
            uint64_t accumulator = 0u;
            for (unsigned column = 0u; column < 3u; ++column) {
                const int64_t product =
                    static_cast<int64_t>(
                        directional_matrix_[row * 4u + column]) *
                    static_cast<int64_t>(source_normal[column]);
                accumulator += static_cast<uint64_t>(product);
            }
            const int64_t signed_accumulator =
                std::bit_cast<int64_t>(accumulator);
            current_vertex_normal_[row] = std::bit_cast<int32_t>(
                static_cast<uint32_t>(
                    static_cast<uint64_t>(signed_accumulator >> 12u)));
        }
        return;
    }

    case 0x22u:
        current_texture_s_ = static_cast<int16_t>(
            sign_extend(parameter, 16u));
        current_texture_t_ = static_cast<int16_t>(
            sign_extend(parameter >> 16u, 16u));
        return;

    case 0x23u:
        current_vertex_position_[0] =
            sign_extend(parameter, 16u);
        current_vertex_position_[1] =
            sign_extend(parameter >> 16u, 16u);
        current_vertex_position_[2] =
            sign_extend(active_parameters_[1], 16u);
        break;

    case 0x24u:
        current_vertex_position_[0] =
            sign_extend(parameter, 10u) * 64;
        current_vertex_position_[1] =
            sign_extend(parameter >> 10u, 10u) * 64;
        current_vertex_position_[2] =
            sign_extend(parameter >> 20u, 10u) * 64;
        break;

    case 0x25u:
        current_vertex_position_[0] = sign_extend(parameter, 16u);
        current_vertex_position_[1] = sign_extend(parameter >> 16u, 16u);
        break;

    case 0x26u:
        current_vertex_position_[0] = sign_extend(parameter, 16u);
        current_vertex_position_[2] = sign_extend(parameter >> 16u, 16u);
        break;

    case 0x27u:
        current_vertex_position_[1] = sign_extend(parameter, 16u);
        current_vertex_position_[2] = sign_extend(parameter >> 16u, 16u);
        break;

    case 0x28u: {
        /* VTX_DIFF supplies signed 1.9 values; expand to 20.12. */
        const std::array<int32_t, 3> delta = {
            sign_extend(parameter, 10u) * 8,
            sign_extend(parameter >> 10u, 10u) * 8,
            sign_extend(parameter >> 20u, 10u) * 8
        };
        for (unsigned axis = 0u; axis < 3u; ++axis) {
            const uint32_t sum =
                std::bit_cast<uint32_t>(current_vertex_position_[axis]) +
                std::bit_cast<uint32_t>(delta[axis]);
            current_vertex_position_[axis] = std::bit_cast<int32_t>(sum);
        }
        break;
    }

    default:
        return;
    }

    if (vertex_list_open_) {
        submit_vertex();
    }
}

void NdsGeometryEngine::submit_vertex() noexcept
{
    Vertex vertex{};
    vertex.source_position = current_vertex_position_;
    vertex.normal = current_vertex_normal_;
    vertex.color_rgb555 = current_vertex_color_;
    vertex.texture_s = current_texture_s_;
    vertex.texture_t = current_texture_t_;

    const std::array<int32_t, 4> input = {
        current_vertex_position_[0],
        current_vertex_position_[1],
        current_vertex_position_[2],
        kFixedOne
    };

    /*
     * DS vertices are row-vectors: (x,y,z,1) * ClipMatrix, where
     * ClipMatrix is Position * Projection. Inputs and matrix elements are
     * signed 20.12 fixed-point; accumulate in 64 bits, shift once, then
     * retain the hardware-sized 32-bit result.
     */
    for (unsigned column = 0u; column < 4u; ++column) {
        uint64_t accumulator = 0u;
        for (unsigned row = 0u; row < 4u; ++row) {
            const int64_t product =
                static_cast<int64_t>(input[row]) *
                static_cast<int64_t>(clip_matrix_[row * 4u + column]);
            accumulator += static_cast<uint64_t>(product);
        }
        const int64_t signed_accumulator =
            std::bit_cast<int64_t>(accumulator);
        const uint32_t shifted = static_cast<uint32_t>(
            static_cast<uint64_t>(signed_accumulator >> 12u));
        vertex.clip_position[column] = std::bit_cast<int32_t>(shifted);
    }

    /* Hold submitted vertices in the primitive cache until a polygon exists. */
    append_vertex_to_list(vertex);
}

void NdsGeometryEngine::calculate_screen_position(
    Vertex &vertex) const noexcept
{
    const int64_t x = vertex.clip_position[0];
    const int64_t y = vertex.clip_position[1];
    const int64_t z = vertex.clip_position[2];
    const int64_t w = vertex.clip_position[3];

    /*
     * Viewport conversion is performed after clipping in hardware. Until the
     * polygon clipper is connected, only vertices already inside all six
     * homogeneous clip planes receive valid screen coordinates.
     */
    if (w <= 0 || x < -w || x > w || y < -w || y > w ||
        z < -w || z > w) {
        vertex.screen_position = {};
        vertex.screen_position_valid = false;
        return;
    }

    const int64_t viewport_width =
        static_cast<int64_t>(viewport_x2_) - viewport_x1_;
    const int64_t viewport_height =
        static_cast<int64_t>(viewport_y2_) - viewport_y1_;
    const int64_t denominator = 2 * w;

    const int64_t screen_x =
        static_cast<int64_t>(viewport_x1_) +
        ((x + w) * viewport_width) / denominator;
    const int64_t screen_y =
        static_cast<int64_t>(viewport_y1_) +
        ((y + w) * viewport_height) / denominator;

    vertex.screen_position = {
        static_cast<int32_t>(screen_x),
        static_cast<int32_t>(screen_y)
    };
    vertex.screen_position_valid = true;
}

int64_t NdsGeometryEngine::clip_distance(
    const Vertex &vertex, uint8_t plane) noexcept
{
    const int64_t x = vertex.clip_position[0];
    const int64_t y = vertex.clip_position[1];
    const int64_t z = vertex.clip_position[2];
    const int64_t w = vertex.clip_position[3];

    switch (plane) {
    case 0u: return x + w; /* left */
    case 1u: return w - x; /* right */
    case 2u: return y + w; /* bottom */
    case 3u: return w - y; /* top */
    case 4u: return z + w; /* near */
    case 5u: return w - z; /* far */
    default: return -1;
    }
}

NdsGeometryEngine::Vertex NdsGeometryEngine::interpolate_vertex(
    const Vertex &from, const Vertex &to,
    int64_t from_distance, int64_t to_distance) noexcept
{
    const uint64_t numerator = absolute_value(from_distance);
    const uint64_t denominator = absolute_value(from_distance - to_distance);
    if (denominator == 0u) return from;

    Vertex result{};
    for (unsigned component = 0u; component < 4u; ++component) {
        result.clip_position[component] = interpolate_i32(
            from.clip_position[component], to.clip_position[component],
            numerator, denominator);
    }
    for (unsigned component = 0u; component < 3u; ++component) {
        result.source_position[component] = interpolate_i32(
            from.source_position[component], to.source_position[component],
            numerator, denominator);
        result.normal[component] = interpolate_i32(
            from.normal[component], to.normal[component],
            numerator, denominator);
    }
    result.texture_s = interpolate_i16(
        from.texture_s, to.texture_s, numerator, denominator);
    result.texture_t = interpolate_i16(
        from.texture_t, to.texture_t, numerator, denominator);

    const auto interpolate_color = [numerator, denominator](
        uint16_t from_color, uint16_t to_color, unsigned shift) noexcept {
        const int64_t a = (from_color >> shift) & UINT16_C(0x1F);
        const int64_t b = (to_color >> shift) & UINT16_C(0x1F);
        const int64_t delta = b - a;
        const int64_t adjustment = static_cast<int64_t>(
            multiply_divide_fraction(
                absolute_value(delta), numerator, denominator));
        return static_cast<uint16_t>(std::clamp<int64_t>(
            a + (delta < 0 ? -adjustment : adjustment), 0, 31));
    };
    const uint16_t red = interpolate_color(
        from.color_rgb555, to.color_rgb555, 0u);
    const uint16_t green = interpolate_color(
        from.color_rgb555, to.color_rgb555, 5u);
    const uint16_t blue = interpolate_color(
        from.color_rgb555, to.color_rgb555, 10u);
    result.color_rgb555 = static_cast<uint16_t>(
        red | (green << 5u) | (blue << 10u));
    return result;
}

bool NdsGeometryEngine::clip_polygon(
    const Vertex *vertices, uint8_t count,
    std::array<Vertex, MaxClippedPolygonVertices> &clipped,
    uint8_t &clipped_count) const noexcept
{
    clipped = {};
    clipped_count = 0u;
    if (vertices == nullptr || count < 3u || count > 4u) return false;

    std::array<Vertex, MaxClippedPolygonVertices> source{};
    std::array<Vertex, MaxClippedPolygonVertices> destination{};
    for (uint8_t index = 0u; index < count; ++index) {
        source[index] = vertices[index];
    }

    uint8_t current_count = count;
    for (uint8_t plane = 0u; plane < 6u; ++plane) {
        if (current_count == 0u) break;
        uint8_t next_count = 0u;

        const auto append = [&destination, &next_count](
            const Vertex &vertex) noexcept {
            if (next_count >= MaxClippedPolygonVertices) return false;
            destination[next_count++] = vertex;
            return true;
        };

        Vertex previous = source[current_count - 1u];
        int64_t previous_distance = clip_distance(previous, plane);
        bool previous_inside = previous_distance >= 0;

        for (uint8_t index = 0u; index < current_count; ++index) {
            const Vertex &current = source[index];
            const int64_t current_distance = clip_distance(current, plane);
            const bool current_inside = current_distance >= 0;

            if (current_inside) {
                if (!previous_inside &&
                    !append(interpolate_vertex(
                        previous, current,
                        previous_distance, current_distance))) {
                    return false;
                }
                if (!append(current)) return false;
            } else if (previous_inside) {
                if (!append(interpolate_vertex(
                        previous, current,
                        previous_distance, current_distance))) {
                    return false;
                }
            }

            previous = current;
            previous_distance = current_distance;
            previous_inside = current_inside;
        }

        source.swap(destination);
        destination = {};
        current_count = next_count;
    }

    if (current_count < 3u) return true;
    for (uint8_t index = 0u; index < current_count; ++index) {
        calculate_screen_position(source[index]);
        clipped[index] = source[index];
    }
    clipped_count = current_count;
    return true;
}

void NdsGeometryEngine::begin_vertex_list(uint32_t parameter) noexcept
{
    if (has_incomplete_primitive()) {
        incomplete_polygon_list_ = true;
    }
    vertex_list_open_ = true;
    primitive_mode_ = static_cast<uint8_t>(parameter & 3u);
    active_polygon_attributes_ = pending_polygon_attributes_;
    vertex_list_vertex_count_ = 0u;
    primitive_vertex_count_ = 0u;
    primitive_vertices_ = {};
}

void NdsGeometryEngine::end_vertex_list() noexcept
{
    /*
     * END_VTXS is a compatibility dummy on real DS hardware. It does not
     * close, flush, or discard the active primitive list.
     */
}

bool NdsGeometryEngine::has_incomplete_primitive() const noexcept
{
    if (incomplete_polygon_list_) return true;
    if (!vertex_list_open_ || vertex_list_vertex_count_ == 0u) return false;

    switch (primitive_mode_) {
    case 0u:
    case 1u:
        return primitive_vertex_count_ != 0u;
    case 2u:
        return vertex_list_vertex_count_ < 3u;
    case 3u:
        return vertex_list_vertex_count_ < 4u ||
            (vertex_list_vertex_count_ & 1u) != 0u;
    default:
        return true;
    }
}

void NdsGeometryEngine::emit_polygon(
    const Vertex *vertices, uint8_t count) noexcept
{
    if (vertices == nullptr || (count != 3u && count != 4u)) return;
    auto &buffer = buffers_[build_buffer_index_];

    std::array<Vertex, MaxClippedPolygonVertices> clipped{};
    uint8_t clipped_count = 0u;
    if (!clip_polygon(vertices, count, clipped, clipped_count)) {
        buffer.vertex_overflow = true;
        return;
    }
    if (clipped_count < 3u) return;

    if (buffer.polygon_count >= PolygonRamCapacity) {
        buffer.polygon_overflow = true;
        return;
    }
    if (clipped_count > VertexRamCapacity - buffer.vertex_count) {
        buffer.vertex_overflow = true;
        return;
    }

    Polygon polygon{};
    polygon.attributes = active_polygon_attributes_;
    polygon.vertex_count = clipped_count;
    const uint32_t base_vertex = buffer.vertex_count;
    for (uint8_t index = 0u; index < clipped_count; ++index) {
        polygon.vertex_indices[index] = base_vertex + index;
        buffer.vertices[buffer.vertex_count++] = clipped[index];
    }
    buffer.polygons[buffer.polygon_count++] = polygon;
}

void NdsGeometryEngine::append_vertex_to_list(const Vertex &vertex) noexcept
{
    if (!vertex_list_open_) return;

    const uint32_t ordinal = vertex_list_vertex_count_++;
    if (primitive_mode_ == 0u || primitive_mode_ == 1u) {
        primitive_vertices_[primitive_vertex_count_++] = vertex;
        const uint8_t required = primitive_mode_ == 0u ? 3u : 4u;
        if (primitive_vertex_count_ == required) {
            emit_polygon(primitive_vertices_.data(), required);
            primitive_vertex_count_ = 0u;
        }
        return;
    }

    if (primitive_mode_ == 2u) {
        if (ordinal < 2u) {
            primitive_vertices_[ordinal] = vertex;
            return;
        }

        Vertex triangle[3] = {};
        if (ordinal == 2u) {
            triangle[0] = primitive_vertices_[0];
            triangle[1] = primitive_vertices_[1];
        } else if ((ordinal & 1u) != 0u) {
            triangle[0] = primitive_vertices_[1];
            triangle[1] = primitive_vertices_[0];
        } else {
            triangle[0] = primitive_vertices_[0];
            triangle[1] = primitive_vertices_[1];
        }
        triangle[2] = vertex;
        emit_polygon(triangle, 3u);
        primitive_vertices_[0] = primitive_vertices_[1];
        primitive_vertices_[1] = vertex;
        return;
    }

    /* Quad strip sequence: top0,bottom0,top1,bottom1,top2,bottom2... */
    if (ordinal == 0u) {
        primitive_vertices_[0] = vertex;
        return;
    }
    if (ordinal == 1u) {
        primitive_vertices_[1] = vertex;
        return;
    }
    if ((ordinal & 1u) == 0u) {
        primitive_vertices_[2] = vertex;
        return;
    }

    const Vertex quad[4] = {
        primitive_vertices_[0],
        primitive_vertices_[1],
        vertex,
        primitive_vertices_[2]
    };
    emit_polygon(quad, 4u);
    primitive_vertices_[0] = primitive_vertices_[2];
    primitive_vertices_[1] = vertex;
}

void NdsGeometryEngine::execute_active_command() noexcept
{
    if (active_command_ >= 0x10u && active_command_ <= 0x1Cu) {
        execute_matrix_command();
    } else if (active_command_ >= 0x20u && active_command_ <= 0x28u) {
        execute_vertex_command();
    } else if (active_command_ == 0x29u) {
        /* POLYGON_ATTR is latched only by the next BEGIN_VTXS. */
        pending_polygon_attributes_ = active_parameters_[0];
    } else if (active_command_ == 0x40u) {
        begin_vertex_list(active_parameters_[0]);
    } else if (active_command_ == 0x41u) {
        end_vertex_list();
    } else if (active_command_ == 0x60u) {
        const uint32_t parameter = active_parameters_[0];
        viewport_x1_ = static_cast<uint8_t>(parameter & UINT32_C(0xFF));
        viewport_y1_ = static_cast<uint8_t>((parameter >> 8u) & UINT32_C(0xFF));
        viewport_x2_ = static_cast<uint8_t>((parameter >> 16u) & UINT32_C(0xFF));
        viewport_y2_ = static_cast<uint8_t>((parameter >> 24u) & UINT32_C(0xFF));
    }
    /* Six-plane clipping, lighting and rasterization remain separate stages. */
}

} // namespace oracis::memory

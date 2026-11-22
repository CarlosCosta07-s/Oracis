/*
 * Nintendo DS Geometry Engine command FIFO and matrix command processor.
 *
 * Implements command intake, FIFO status/IRQ, matrix commands, vertex
 * capture, polygon assembly, and the dual Geometry/Rendering RAM handoff.
 * Clipping, rasterization and the 48-line output cache remain separate.
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
    static constexpr uint32_t OneDotDepthAddress = UINT32_C(0x04000610);
    static constexpr uint32_t ClipMatrixBase = UINT32_C(0x04000640);
    static constexpr uint32_t VectorMatrixBase = UINT32_C(0x04000680);
    static constexpr uint32_t CommandFifoCapacity = 256u;
    static constexpr uint32_t CommandPipeCapacity = 4u;
    static constexpr uint32_t VertexRamCapacity = 6144u;
    static constexpr uint32_t PolygonRamCapacity = 2048u;
    static constexpr uint32_t MaxClippedPolygonVertices = 10u;
    static constexpr uint32_t GxFifoIrqMask = UINT32_C(1) << 21u;

    using Matrix = std::array<int32_t, 16>;

    struct Vertex {
        std::array<int32_t, 4> clip_position{};
        std::array<int32_t, 2> screen_position{};
        bool screen_position_valid = false;
        std::array<int32_t, 3> source_position{};
        std::array<int32_t, 3> normal{};
        uint16_t color_rgb555 = UINT16_C(0x7FFF);
        int16_t texture_s = 0;
        int16_t texture_t = 0;
    };

    struct Polygon {
        std::array<uint32_t, MaxClippedPolygonVertices> vertex_indices{};
        uint32_t attributes = 0u;
        uint32_t texture_parameters = 0u;
        uint16_t texture_palette_base = 0u;
        uint8_t vertex_count = 0u;
        uint8_t light_enable_mask = 0u;
        uint8_t polygon_mode = 0u;
        uint8_t alpha = 0u;
        uint8_t polygon_id = 0u;
        bool translucent_depth_write = false;
        bool depth_equal = false;
        bool fog_enabled = false;
        bool front_facing = false;
        bool line_segment = false;
        bool far_plane_intersection = false;
        bool zero_dot_polygon = false;
        bool one_dot_depth_visible = true;
        bool render_enabled = false;
    };

    struct GeometryBuffer {
        std::array<Vertex, VertexRamCapacity> vertices{};
        uint32_t vertex_count = 0u;
        bool vertex_overflow = false;
        std::array<Polygon, PolygonRamCapacity> polygons{};
        uint32_t polygon_count = 0u;
        bool polygon_overflow = false;
        uint8_t swap_parameters = 0u;
    };

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

    /* Notify the command processor when the display enters scanline 192. */
    void notify_vblank() noexcept;

    /* Reassert level-sensitive GX FIFO IRQ after software writes IF. */
    void refresh_fifo_irq() noexcept;

    /* GXSTAT counts only the 256-entry FIFO; the four-entry PIPE is separate. */
    uint32_t fifo_entries() const noexcept { return fifo_count_; }
    uint32_t pipe_entries() const noexcept { return pipe_count_; }
    uint32_t queued_entries() const noexcept {
        return static_cast<uint32_t>(fifo_count_) + pipe_count_;
    }
    uint32_t vertex_count() const noexcept {
        return buffers_[build_buffer_index_].vertex_count;
    }
    bool vertex_ram_overflow() const noexcept {
        return buffers_[build_buffer_index_].vertex_overflow;
    }
    uint32_t polygon_count() const noexcept {
        return buffers_[build_buffer_index_].polygon_count;
    }
    bool polygon_ram_overflow() const noexcept {
        return buffers_[build_buffer_index_].polygon_overflow;
    }
    uint32_t render_vertex_count() const noexcept {
        return buffers_[render_buffer_index_].vertex_count;
    }
    uint32_t render_polygon_count() const noexcept {
        return buffers_[render_buffer_index_].polygon_count;
    }
    const Vertex *render_vertex_at(uint32_t index) const noexcept {
        const auto &buffer = buffers_[render_buffer_index_];
        return index < buffer.vertex_count ? &buffer.vertices[index] : nullptr;
    }
    const Polygon *render_polygon_at(uint32_t index) const noexcept {
        const auto &buffer = buffers_[render_buffer_index_];
        return index < buffer.polygon_count ? &buffer.polygons[index] : nullptr;
    }
    bool swap_waiting_for_vblank() const noexcept {
        return swap_waiting_vblank_;
    }
    bool swap_lockup() const noexcept { return swap_lockup_; }
    uint8_t build_buffer_index() const noexcept { return build_buffer_index_; }
    uint8_t render_buffer_index() const noexcept { return render_buffer_index_; }
    uint8_t swap_parameters() const noexcept { return swap_parameters_; }
    uint8_t render_swap_parameters() const noexcept {
        return buffers_[render_buffer_index_].swap_parameters;
    }
    bool render_uses_w_buffer() const noexcept {
        return (render_swap_parameters() & UINT8_C(2)) != 0u;
    }
    bool vertex_list_open() const noexcept { return vertex_list_open_; }
    bool incomplete_polygon_list() const noexcept {
        return has_incomplete_primitive();
    }
    uint32_t vertex_list_vertex_count() const noexcept {
        return vertex_list_vertex_count_;
    }
    uint8_t primitive_mode() const noexcept { return primitive_mode_; }
    const Polygon *polygon_at(uint32_t index) const noexcept {
        const auto &buffer = buffers_[build_buffer_index_];
        return index < buffer.polygon_count ? &buffer.polygons[index] : nullptr;
    }
    const Vertex *vertex_at(uint32_t index) const noexcept {
        const auto &buffer = buffers_[build_buffer_index_];
        return index < buffer.vertex_count ? &buffer.vertices[index] : nullptr;
    }
    bool busy() const noexcept;
    bool matrix_stack_error() const noexcept { return matrix_stack_error_; }
    uint8_t matrix_mode() const noexcept { return matrix_mode_; }
    uint8_t position_stack_pointer() const noexcept {
        return static_cast<uint8_t>(position_stack_pointer_ & 0x1Fu);
    }
    uint8_t projection_stack_pointer() const noexcept {
        return projection_stack_pointer_;
    }
    std::array<uint8_t, 4> viewport() const noexcept {
        return {viewport_x1_, viewport_y1_, viewport_x2_, viewport_y2_};
    }
    const Matrix &position_matrix() const noexcept { return position_matrix_; }
    const Matrix &directional_matrix() const noexcept { return directional_matrix_; }
    const Matrix &projection_matrix() const noexcept { return projection_matrix_; }
    const Matrix &texture_matrix() const noexcept { return texture_matrix_; }
    uint64_t completed_commands() const noexcept { return completed_commands_; }

    static Matrix identity_matrix() noexcept;

private:
    struct LightSource {
        std::array<int32_t, 3> direction{};
        std::array<int32_t, 3> half_vector{};
        std::array<uint8_t, 3> color{};
    };

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
    static int32_t sign_extend(uint32_t value, unsigned width) noexcept;
    static Matrix matrix_from_parameters(
        uint8_t command, const uint32_t *parameters) noexcept;
    static Matrix multiply(const Matrix &left, const Matrix &right) noexcept;

    bool fifo_push(uint8_t command, uint32_t parameter) noexcept;
    bool fifo_peek(uint32_t offset, FifoEntry &entry) const noexcept;
    bool fifo_pop(FifoEntry &entry) noexcept;
    void refill_pipe() noexcept;
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
    void execute_vertex_command() noexcept;
    std::array<int32_t, 3> transform_direction(
        const std::array<int32_t, 3> &vector) const noexcept;
    void calculate_lighting() noexcept;
    void transform_texture_coordinates_from_matrix(
        const std::array<int32_t, 3> &source) noexcept;
    void transform_texture_coordinates_from_texcoord() noexcept;
    void submit_vertex() noexcept;
    void calculate_screen_position(Vertex &vertex) const noexcept;
    void begin_vertex_list(uint32_t parameter) noexcept;
    void end_vertex_list() noexcept;
    bool has_incomplete_primitive() const noexcept;
    void append_vertex_to_list(const Vertex &vertex) noexcept;
    void emit_polygon(const Vertex *vertices, uint8_t count) noexcept;
    static int64_t clip_distance(const Vertex &vertex, uint8_t plane) noexcept;
    static Vertex interpolate_vertex(
        const Vertex &from, const Vertex &to,
        int64_t from_distance, int64_t to_distance) noexcept;
    bool clip_polygon(
        const Vertex *vertices, uint8_t count,
        std::array<Vertex, MaxClippedPolygonVertices> &clipped,
        uint8_t &clipped_count,
        bool &far_plane_intersection) const noexcept;
    void reset_build_buffer() noexcept;
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
    std::array<FifoEntry, CommandPipeCapacity> pipe_{};
    uint8_t pipe_head_ = 0u;
    uint8_t pipe_count_ = 0u;

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
    uint8_t viewport_x1_ = 0u;
    uint8_t viewport_y1_ = 0u;
    uint8_t viewport_x2_ = 255u;
    uint8_t viewport_y2_ = 191u;
    std::array<Matrix, 32> position_stack_{};
    std::array<Matrix, 32> directional_stack_{};
    std::array<Matrix, 1> texture_stack_{};

    std::array<int32_t, 3> current_vertex_position_{};
    std::array<int32_t, 3> current_vertex_normal_{};
    uint16_t current_vertex_color_ = UINT16_C(0x7FFF);
    std::array<LightSource, 4> lights_{};
    std::array<uint8_t, 3> material_diffuse_{};
    std::array<uint8_t, 3> material_ambient_{};
    std::array<uint8_t, 3> material_specular_{};
    std::array<uint8_t, 3> material_emission_{};
    std::array<uint8_t, 128> shininess_table_{};
    bool shininess_table_enabled_ = false;
    int16_t raw_texture_s_ = 0;
    int16_t raw_texture_t_ = 0;
    int16_t current_texture_s_ = 0;
    int16_t current_texture_t_ = 0;
    uint32_t current_texture_parameters_ = 0u;
    uint16_t current_texture_palette_base_ = 0u;
    uint8_t texture_transform_mode_ = 0u;
    std::array<GeometryBuffer, 2> buffers_{};
    uint8_t build_buffer_index_ = 0u;
    uint8_t render_buffer_index_ = 1u;
    bool swap_waiting_vblank_ = false;
    bool swap_lockup_ = false;
    uint16_t swap_buffer_clear_cycles_ = 0u;
    uint8_t swap_parameters_ = 0u;
    uint16_t one_dot_depth_ = 0u;
    uint32_t pending_polygon_attributes_ = 0u;
    uint32_t active_polygon_attributes_ = 0u;
    uint8_t primitive_mode_ = 0u;
    bool vertex_list_open_ = false;
    bool incomplete_polygon_list_ = false;
    uint32_t vertex_list_vertex_count_ = 0u;
    std::array<Vertex, 4> primitive_vertices_{};
    uint8_t primitive_vertex_count_ = 0u;

    uint64_t completed_commands_ = 0u;
    bool installed_ = false;
};

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_GEOMETRY_ENGINE_HPP */

#include "oracis/memory/nds_geometry_engine.hpp"
#include "oracis/memory/nds_interrupt_control.hpp"
#include "oracis/memory/guest_memory.hpp"
#include "oracis/memory/nds_display_control.hpp"
#include "oracis/memory/nds_system_clock.hpp"
#include "oracis/memory/nds_timer_control.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_geometry_engine_test: %s\n", message);
    return 1;
}

bool write_command(
    oracis::memory::NdsGeometryEngine &engine,
    uint8_t command,
    uint32_t parameter = 0u)
{
    const uint32_t address = UINT32_C(0x04000400) +
        static_cast<uint32_t>(command) * 4u;
    return engine.write(NDS_ARM9, address, 4u, parameter) ==
        oracis::memory::Status::Ok;
}

bool write_parameters(
    oracis::memory::NdsGeometryEngine &engine,
    uint8_t command,
    const uint32_t *parameters,
    uint32_t count)
{
    for (uint32_t index = 0u; index < count; ++index) {
        if (!write_command(engine, command, parameters[index])) return false;
    }
    return true;
}

} // namespace

int main()
{
    using namespace oracis::memory;

    GuestMemory memory;
    NdsInterruptControl interrupts(memory);
    NdsGeometryEngine geometry(interrupts);
    if (geometry.install() != Status::Ok || !geometry.installed() ||
        geometry.install() != Status::InvalidRegion) {
        return fail("Geometry Engine install/reset failed");
    }

    /* ARM7 has no access to the Geometry Engine register block. */
    if (geometry.read(NDS_ARM7, NdsGeometryEngine::GxStatAddress, 4u).status !=
            Status::Unmapped ||
        geometry.write(NDS_ARM7, NdsGeometryEngine::GxFifoBase, 4u, 0x10u) !=
            Status::Unmapped ||
        geometry.read(NDS_ARM9, NdsGeometryEngine::GxStatAddress, 2u).status !=
            Status::InvalidWidth ||
        geometry.write(
            NDS_ARM9, NdsGeometryEngine::GxFifoBase, 2u, 0x10u) !=
            Status::InvalidWidth) {
        return fail("CPU visibility or register width checks failed");
    }

    /* An unpacked command occupies a code word followed by its parameters. */
    if (geometry.write(
            NDS_ARM9, NdsGeometryEngine::GxFifoBase, 4u,
            UINT32_C(0x10)) != Status::Ok ||
        geometry.fifo_entries() != 0u ||
        geometry.write(
            NDS_ARM9, NdsGeometryEngine::GxFifoBase + 4u, 4u,
            UINT32_C(1)) != Status::Ok ||
        geometry.fifo_entries() != 0u || geometry.pipe_entries() != 1u ||
        geometry.queued_entries() != 1u) {
        return fail("GXFIFO unpacked command/parameter stream failed");
    }
    uint32_t status = geometry.read(
        NDS_ARM9, NdsGeometryEngine::GxStatAddress, 4u).value;
    if (((status >> 16u) & UINT32_C(0x1FF)) != 0u ||
        (status & (UINT32_C(1) << 26u)) == 0u ||
        (status & (UINT32_C(1) << 27u)) == 0u) {
        return fail("GXSTAT FIFO count, empty or busy bits were incorrect");
    }
    geometry.advance_bus_cycles(1u);
    if (geometry.matrix_mode() != 1u || geometry.busy() ||
        geometry.completed_commands() != 1u) {
        return fail("MTX_MODE did not execute after its documented cycle");
    }

    /*
     * Packed command bytes expand in order, then consume 16 MTX_LOAD_4x4
     * parameters followed by two parameterless MTX_IDENTITY commands.
     */
    const uint32_t packed_commands = UINT32_C(0x00151516);
    if (geometry.write(
            NDS_ARM9, NdsGeometryEngine::GxFifoBase, 4u,
            packed_commands) != Status::Ok ||
        geometry.fifo_entries() != 0u) {
        return fail("packed command decode failed before parameters arrived");
    }
    constexpr std::array<uint32_t, 16> identity = {
        UINT32_C(0x1000), 0u, 0u, 0u,
        0u, UINT32_C(0x1000), 0u, 0u,
        0u, 0u, UINT32_C(0x1000), 0u,
        0u, 0u, 0u, UINT32_C(0x1000)
    };
    for (const uint32_t parameter : identity) {
        if (geometry.write(
                NDS_ARM9, NdsGeometryEngine::GxFifoBase + 4u,
                4u, parameter) != Status::Ok) {
            return fail("packed MTX_LOAD_4x4 parameter delivery failed");
        }
    }
    if (geometry.queued_entries() != 18u || geometry.pipe_entries() != 4u ||
        geometry.fifo_entries() != 14u) {
        return fail("packed commands did not expand across PIPE/FIFO entries");
    }
    geometry.advance_bus_cycles(34u + 19u + 19u);
    if (geometry.busy() || geometry.completed_commands() != 4u ||
        geometry.position_matrix() != NdsGeometryEngine::identity_matrix()) {
        return fail("packed matrix load/identity commands executed incorrectly");
    }

    /* A 4x4 matrix load and translation use signed 20.12 values. */
    if (!write_command(geometry, 0x10u, 1u)) {
        return fail("position matrix mode selection failed");
    }
    geometry.advance_bus_cycles(1u);
    constexpr std::array<uint32_t, 3> translation = {
        UINT32_C(0x00001000), UINT32_C(0xFFFFE000), UINT32_C(0x00003000)
    };
    if (!write_parameters(geometry, 0x1Cu, translation.data(), 3u)) {
        return fail("MTX_TRANS port writes failed");
    }
    geometry.advance_bus_cycles(22u);
    if (geometry.position_matrix()[12] != INT32_C(4096) ||
        geometry.position_matrix()[13] != -INT32_C(8192) ||
        geometry.position_matrix()[14] != INT32_C(12288)) {
        return fail("MTX_TRANS signed 20.12 translation result was incorrect");
    }

    /* Projection x Position is exposed through CLIPMTX_RESULT when idle. */
    if (!write_command(geometry, 0x10u, 0u) ||
        !write_command(geometry, 0x15u)) {
        return fail("projection identity setup failed");
    }
    geometry.advance_bus_cycles(20u);
    const ReadResult clip_x = geometry.read(
        NDS_ARM9, NdsGeometryEngine::ClipMatrixBase + 12u * 4u, 4u);
    const ReadResult clip_y = geometry.read(
        NDS_ARM9, NdsGeometryEngine::ClipMatrixBase + 13u * 4u, 4u);
    if (!clip_x || !clip_y ||
        std::bit_cast<int32_t>(clip_x.value) != INT32_C(4096) ||
        std::bit_cast<int32_t>(clip_y.value) != -INT32_C(8192)) {
        return fail("clip matrix did not combine position and projection matrices");
    }
    if (geometry.read(
            NDS_ARM9, NdsGeometryEngine::RamCountAddress, 4u).value != 0u) {
        return fail("empty polygon/vertex RAM counters were not zero");
    }

    /* Position stack push/pop saves and restores the full matrix. */
    if (!write_command(geometry, 0x10u, 1u) ||
        !write_command(geometry, 0x11u)) {
        return fail("position matrix push enqueue failed");
    }
    geometry.advance_bus_cycles(18u);
    if (geometry.position_stack_pointer() != 1u) {
        return fail("position matrix stack pointer did not advance on PUSH");
    }
    if (!write_parameters(geometry, 0x1Cu, translation.data(), 3u)) {
        return fail("position translation before POP failed");
    }
    geometry.advance_bus_cycles(22u);
    if (!write_command(geometry, 0x12u, 1u)) {
        return fail("position matrix POP enqueue failed");
    }
    geometry.advance_bus_cycles(36u);
    if (geometry.position_stack_pointer() != 0u ||
        geometry.position_matrix()[12] != INT32_C(4096) ||
        geometry.position_matrix()[13] != -INT32_C(8192)) {
        return fail("position matrix POP did not restore its saved value");
    }

    /* STORE/IDENTITY/RESTORE use a direct five-bit stack address. */
    if (!write_command(geometry, 0x13u, 3u) ||
        !write_command(geometry, 0x15u) ||
        !write_command(geometry, 0x14u, 3u)) {
        return fail("matrix STORE/IDENTITY/RESTORE enqueue failed");
    }
    geometry.advance_bus_cycles(17u + 19u + 36u);
    if (geometry.position_matrix()[12] != INT32_C(4096) ||
        geometry.position_matrix()[13] != -INT32_C(8192)) {
        return fail("MTX_RESTORE did not recover the stored matrix");
    }

    /* Mode 2 changes position and directional matrices together, except SCALE. */
    if (!write_command(geometry, 0x10u, 2u) ||
        !write_command(geometry, 0x15u)) {
        return fail("simultaneous position/vector mode setup failed");
    }
    geometry.advance_bus_cycles(20u);
    constexpr std::array<uint32_t, 3> scale = {
        UINT32_C(0x2000), UINT32_C(0x3000), UINT32_C(0x4000)
    };
    if (!write_parameters(geometry, 0x1Bu, scale.data(), 3u)) {
        return fail("MTX_SCALE parameter stream failed");
    }
    geometry.advance_bus_cycles(22u);
    if (geometry.position_matrix()[0] != INT32_C(8192) ||
        geometry.position_matrix()[5] != INT32_C(12288) ||
        geometry.position_matrix()[10] != INT32_C(16384) ||
        geometry.directional_matrix()[0] != INT32_C(4096) ||
        geometry.directional_matrix()[5] != INT32_C(4096) ||
        geometry.directional_matrix()[10] != INT32_C(4096)) {
        return fail("MTX_SCALE incorrectly changed the directional matrix in mode 2");
    }

    /* Vector result exposes the top-left 3x3 directional matrix only. */
    const ReadResult vector_x = geometry.read(
        NDS_ARM9, NdsGeometryEngine::VectorMatrixBase, 4u);
    if (!vector_x || vector_x.value != UINT32_C(0x1000)) {
        return fail("VECMTX_RESULT did not expose the directional matrix");
    }

    /* Projection PUSH overflow is sticky and GXSTAT bit 15 acknowledges it. */
    if (!write_command(geometry, 0x10u, 0u) ||
        !write_command(geometry, 0x11u) ||
        !write_command(geometry, 0x11u)) {
        return fail("projection stack overflow fixture enqueue failed");
    }
    geometry.advance_bus_cycles(35u);
    status = geometry.read(
        NDS_ARM9, NdsGeometryEngine::GxStatAddress, 4u).value;
    if ((status & (UINT32_C(1) << 15u)) == 0u ||
        geometry.projection_stack_pointer() != 1u) {
        return fail("projection stack overflow did not set GXSTAT error");
    }
    if (geometry.write(
            NDS_ARM9, NdsGeometryEngine::GxStatAddress, 4u,
            UINT32_C(1) << 15u) != Status::Ok ||
        geometry.matrix_stack_error() ||
        geometry.projection_stack_pointer() != 0u) {
        return fail("GXSTAT bit 15 did not acknowledge/reset projection stack error");
    }

    /* Underflow is also detected rather than reading outside the stack. */
    if (!write_command(geometry, 0x12u, 1u)) {
        return fail("projection stack underflow fixture enqueue failed");
    }
    geometry.advance_bus_cycles(36u);
    if (!geometry.matrix_stack_error()) {
        return fail("projection matrix stack underflow was not detected");
    }
    if (geometry.write(
            NDS_ARM9, NdsGeometryEngine::GxStatAddress, 4u,
            UINT32_C(1) << 15u) != Status::Ok) {
        return fail("matrix stack underflow error acknowledgement failed");
    }

    /* FIFO IRQ mode 2 is level-sensitive while the FIFO is empty. */
    if (geometry.write(
            NDS_ARM9, NdsGeometryEngine::GxStatAddress, 4u,
            UINT32_C(2) << 30u) != Status::Ok ||
        (interrupts.read(
            NDS_ARM9, NdsInterruptControl::IfAddress, 4u).value &
            NdsGeometryEngine::GxFifoIrqMask) == 0u) {
        return fail("empty FIFO IRQ did not assert IF bit 21");
    }
    if (interrupts.write(
            NDS_ARM9, NdsInterruptControl::IfAddress, 4u,
            NdsGeometryEngine::GxFifoIrqMask) != Status::Ok) {
        return fail("FIFO IRQ IF acknowledge write failed");
    }
    geometry.refresh_fifo_irq();
    if ((interrupts.read(
            NDS_ARM9, NdsInterruptControl::IfAddress, 4u).value &
            NdsGeometryEngine::GxFifoIrqMask) == 0u) {
        return fail("level-sensitive FIFO IRQ was not reasserted after IF acknowledge");
    }
    if (geometry.write(
            NDS_ARM9, NdsGeometryEngine::GxStatAddress, 4u, 0u) != Status::Ok ||
        interrupts.write(
            NDS_ARM9, NdsInterruptControl::IfAddress, 4u,
            NdsGeometryEngine::GxFifoIrqMask) != Status::Ok) {
        return fail("FIFO IRQ disable/clear failed");
    }
    geometry.refresh_fifo_irq();
    if ((interrupts.read(
            NDS_ARM9, NdsInterruptControl::IfAddress, 4u).value &
            NdsGeometryEngine::GxFifoIrqMask) != 0u) {
        return fail("disabled FIFO IRQ was reasserted");
    }

    /* Fill the 4-entry PIPE and all 256 FIFO entries with identity commands. */
    for (uint32_t index = 0u;
         index < NdsGeometryEngine::CommandFifoCapacity +
                     NdsGeometryEngine::CommandPipeCapacity;
         ++index) {
        if (!write_command(geometry, 0x15u)) {
            return fail("FIFO rejected a command before reaching capacity");
        }
    }
    if (geometry.fifo_entries() != 256u ||
        geometry.pipe_entries() != 4u || geometry.queued_entries() != 260u ||
        geometry.write(
            NDS_ARM9, NdsGeometryEngine::CommandPortsBase +
                UINT32_C(0x15) * 4u, 4u, 0u) != Status::PermissionDenied) {
        return fail("FIFO full condition or full-write rejection failed");
    }
    status = geometry.read(
        NDS_ARM9, NdsGeometryEngine::GxStatAddress, 4u).value;
    if (((status >> 16u) & UINT32_C(0x1FF)) != 256u ||
        (status & (UINT32_C(1) << 24u)) == 0u ||
        (status & (UINT32_C(1) << 26u)) != 0u) {
        return fail("GXSTAT did not expose full FIFO count/empty flags");
    }
    /* One PIPE pop leaves three entries; the next pop prefetches two. */
    geometry.advance_bus_cycles(19u);
    if (geometry.pipe_entries() != 3u ||
        geometry.fifo_entries() != 256u) {
        return fail("PIPE drained before its three-entry refill threshold");
    }
    geometry.advance_bus_cycles(19u);
    if (geometry.pipe_entries() != 4u ||
        geometry.fifo_entries() != 254u) {
        return fail("PIPE did not prefetch two FIFO entries below three");
    }
    geometry.advance_bus_cycles(
        UINT64_C(19) * (NdsGeometryEngine::CommandFifoCapacity +
                        NdsGeometryEngine::CommandPipeCapacity - 2u));
    status = geometry.read(
        NDS_ARM9, NdsGeometryEngine::GxStatAddress, 4u).value;
    if (geometry.fifo_entries() != 0u || geometry.pipe_entries() != 0u ||
        geometry.busy() ||
        (status & (UINT32_C(1) << 26u)) == 0u ||
        geometry.completed_commands() < 256u) {
        return fail("command processor did not drain the full FIFO");
    }

    /* Unknown command IDs are ignored and never consume a parameter word. */
    if (geometry.write(
            NDS_ARM9, NdsGeometryEngine::GxFifoBase, 4u,
            UINT32_C(0x7F)) != Status::Ok ||
        geometry.fifo_entries() != 0u) {
        return fail("invalid packed GXFIFO command was not ignored");
    }

    /*
     * VTX_* commands update the retained source coordinates and emit one
     * clip-space vertex. The test matrix is identity so 20.12 inputs remain
     * directly observable in the transformed Vertex RAM entry.
     */
    if (!write_command(geometry, 0x10u, 1u) ||
        !write_command(geometry, 0x15u)) {
        return fail("vertex-test position identity setup enqueue failed");
    }
    geometry.advance_bus_cycles(20u);
    /* VTX commands outside a BEGIN/END list must not append Vertex RAM. */
    const uint32_t outside_count = geometry.vertex_count();
    if (!write_command(
            geometry, 0x23u, (UINT32_C(0x2000) << 16u) | UINT32_C(0x1000)) ||
        !write_command(geometry, 0x23u, UINT32_C(0x3000))) {
        return fail("out-of-list vertex fixture enqueue failed");
    }
    geometry.advance_bus_cycles(9u);
    if (geometry.vertex_count() != outside_count) {
        return fail("VTX command outside BEGIN_VTXS appended Vertex RAM");
    }
    if (!write_command(geometry, 0x29u, UINT32_C(0x2A7F1234)) ||
        !write_command(geometry, 0x40u, UINT32_C(2))) {
        return fail("BEGIN_VTXS triangle-strip setup enqueue failed");
    }
    geometry.advance_bus_cycles(2u);
    if (!write_command(geometry, 0x41u)) {
        return fail("early dummy END_VTXS enqueue failed");
    }
    geometry.advance_bus_cycles(1u);
    if (!geometry.vertex_list_open() || geometry.incomplete_polygon_list()) {
        return fail("END_VTXS unexpectedly closed or damaged an empty active list");
    }
    const uint32_t packed_normal =
        UINT32_C(1) | (UINT32_C(0x3FF) << 10u) |
        (UINT32_C(0x100) << 20u);
    const uint32_t packed_texcoord =
        UINT32_C(0xFFF00010);
    if (!write_command(geometry, 0x20u, UINT32_C(0x4210)) ||
        !write_command(geometry, 0x21u, packed_normal) ||
        !write_command(geometry, 0x22u, packed_texcoord)) {
        return fail("vertex color, normal and UV command enqueue failed");
    }
    geometry.advance_bus_cycles(11u);

    const uint32_t vtx16_xy =
        (UINT32_C(0x2000) << 16u) | UINT32_C(0x1000);
    if (!write_command(geometry, 0x23u, vtx16_xy) ||
        !write_command(geometry, 0x23u, UINT32_C(0x3000))) {
        return fail("VTX_16 parameter enqueue failed");
    }
    geometry.advance_bus_cycles(9u);
    if (geometry.vertex_count() != 1u) {
        return fail("VTX_16 did not append one transformed vertex");
    }
    const NdsGeometryEngine::Vertex *vertex = geometry.vertex_at(0u);
    if (vertex == nullptr ||
        vertex->source_position !=
            std::array<int32_t, 3>{INT32_C(4096), INT32_C(8192), INT32_C(12288)} ||
        vertex->clip_position !=
            std::array<int32_t, 4>{INT32_C(4096), INT32_C(8192),
                INT32_C(12288), INT32_C(4096)} ||
        vertex->normal !=
            std::array<int32_t, 3>{INT32_C(8), -INT32_C(8), INT32_C(2048)} ||
        vertex->color_rgb555 != UINT16_C(0x4210) ||
        vertex->texture_s != INT16_C(16) ||
        vertex->texture_t != INT16_C(-16)) {
        return fail("VTX_16 transform or retained vertex attributes were incorrect");
    }

    const uint32_t vtx10 =
        UINT32_C(0x040) |
        (UINT32_C(0x3FF) << 10u) |
        (UINT32_C(0x020) << 20u);
    if (!write_command(geometry, 0x24u, vtx10)) {
        return fail("VTX_10 command enqueue failed");
    }
    geometry.advance_bus_cycles(8u);
    vertex = geometry.vertex_at(1u);
    if (vertex == nullptr ||
        vertex->source_position !=
            std::array<int32_t, 3>{INT32_C(4096), -INT32_C(64), INT32_C(2048)}) {
        return fail("VTX_10 signed 10-bit and six-fraction decode was incorrect");
    }

    if (!write_command(
            geometry, 0x25u,
            (UINT32_C(0x4000) << 16u) | UINT32_C(0xE000))) {
        return fail("VTX_XY command enqueue failed");
    }
    geometry.advance_bus_cycles(8u);
    vertex = geometry.vertex_at(2u);
    if (vertex == nullptr ||
        vertex->source_position !=
            std::array<int32_t, 3>{-INT32_C(8192), INT32_C(16384), INT32_C(2048)}) {
        return fail("VTX_XY failed to retain the previous Z coordinate");
    }

    if (!write_command(
            geometry, 0x26u,
            (UINT32_C(0xF000) << 16u) | UINT32_C(0x0800))) {
        return fail("VTX_XZ command enqueue failed");
    }
    geometry.advance_bus_cycles(8u);
    vertex = geometry.vertex_at(3u);
    if (vertex == nullptr ||
        vertex->source_position !=
            std::array<int32_t, 3>{INT32_C(2048), INT32_C(16384), -INT32_C(4096)}) {
        return fail("VTX_XZ failed to retain the previous Y coordinate");
    }

    if (!write_command(
            geometry, 0x27u,
            (UINT32_C(0x1000) << 16u) | UINT32_C(0xF000))) {
        return fail("VTX_YZ command enqueue failed");
    }
    geometry.advance_bus_cycles(8u);
    vertex = geometry.vertex_at(4u);
    if (vertex == nullptr ||
        vertex->source_position !=
            std::array<int32_t, 3>{INT32_C(2048), -INT32_C(4096), INT32_C(4096)}) {
        return fail("VTX_YZ failed to retain the previous X coordinate");
    }

    const uint32_t vtx_diff =
        UINT32_C(0x3FF) |
        (UINT32_C(0x002) << 10u) |
        (UINT32_C(0x3FD) << 20u);
    if (!write_command(geometry, 0x28u, vtx_diff)) {
        return fail("VTX_DIFF command enqueue failed");
    }
    geometry.advance_bus_cycles(8u);
    vertex = geometry.vertex_at(5u);
    if (vertex == nullptr ||
        vertex->source_position !=
            std::array<int32_t, 3>{INT32_C(2040), -INT32_C(4080), INT32_C(4072)} ||
        vertex->clip_position !=
            std::array<int32_t, 4>{INT32_C(2040), -INT32_C(4080),
                INT32_C(4072), INT32_C(4096)}) {
        return fail("VTX_DIFF did not add signed 20.12 deltas or transform the result");
    }
    const ReadResult ram_count = geometry.read(
        NDS_ARM9, NdsGeometryEngine::RamCountAddress, 4u);
    if (!ram_count || ((ram_count.value >> 16u) & UINT32_C(0x1FFF)) != 6u ||
        (ram_count.value & UINT32_C(0x0FFF)) != 4u) {
        return fail("RAM_COUNT did not report stored vertex count");
    }

    if (!write_command(geometry, 0x41u)) {
        return fail("END_VTXS command enqueue failed");
    }
    geometry.advance_bus_cycles(1u);
    if (!geometry.vertex_list_open() || geometry.polygon_count() != 4u ||
        geometry.primitive_mode() != 2u || geometry.incomplete_polygon_list()) {
        return fail("END_VTXS changed a complete active triangle-strip list");
    }
    const NdsGeometryEngine::Polygon *strip_first = geometry.polygon_at(0u);
    const NdsGeometryEngine::Polygon *strip_second = geometry.polygon_at(1u);
    if (strip_first == nullptr || strip_second == nullptr ||
        strip_first->vertex_count != 3u ||
        strip_first->vertex_indices !=
            std::array<uint32_t, 4>{0u, 1u, 2u, 0u} ||
        strip_first->attributes != UINT32_C(0x2A7F1234) ||
        strip_second->vertex_indices !=
            std::array<uint32_t, 4>{2u, 1u, 3u, 0u}) {
        return fail("triangle-strip alternating winding or polygon attributes failed");
    }
    const ReadResult ram_counts_after_strip = geometry.read(
        NDS_ARM9, NdsGeometryEngine::RamCountAddress, 4u);
    if (!ram_counts_after_strip ||
        (ram_counts_after_strip.value & UINT32_C(0x0FFF)) != 4u) {
        return fail("RAM_COUNT polygon count did not follow triangle-strip assembly");
    }

    /* POLYGON_ATTR takes effect on the next list, not during the active one. */
    if (!write_command(geometry, 0x29u, UINT32_C(0x12345678)) ||
        !write_command(geometry, 0x40u, UINT32_C(1))) {
        return fail("separate-quad list setup enqueue failed");
    }
    geometry.advance_bus_cycles(2u);
    const uint32_t quad_vertices[4][2] = {
        {UINT32_C(0), UINT32_C(0x1000)},
        {UINT32_C(0x1000), UINT32_C(0x1000)},
        {UINT32_C(0x2000), UINT32_C(0x1000)},
        {UINT32_C(0x3000), UINT32_C(0x1000)}
    };
    for (unsigned index = 0u; index < 4u; ++index) {
        if (!write_command(
                geometry, 0x23u,
                (quad_vertices[index][1] << 16u) | quad_vertices[index][0]) ||
            !write_command(geometry, 0x23u, UINT32_C(0))) {
            return fail("separate-quad vertex parameter enqueue failed");
        }
    }
    geometry.advance_bus_cycles(36u);
    if (!write_command(geometry, 0x41u)) {
        return fail("separate-quad END_VTXS enqueue failed");
    }
    geometry.advance_bus_cycles(1u);
    const NdsGeometryEngine::Polygon *quad = geometry.polygon_at(4u);
    if (geometry.polygon_count() != 5u || quad == nullptr ||
        quad->vertex_count != 4u ||
        quad->vertex_indices != std::array<uint32_t, 4>{6u, 7u, 8u, 9u} ||
        quad->attributes != UINT32_C(0x12345678)) {
        return fail("separate quadrilateral or deferred polygon attributes failed");
    }

    /* Quad strips add one polygon for every pair after their first quad. */
    if (!write_command(geometry, 0x40u, UINT32_C(3))) {
        return fail("quad-strip BEGIN_VTXS enqueue failed");
    }
    geometry.advance_bus_cycles(1u);
    for (unsigned index = 0u; index < 6u; ++index) {
        const uint32_t x = static_cast<uint32_t>(index) * UINT32_C(0x1000);
        if (!write_command(
                geometry, 0x23u,
                (UINT32_C(0x2000) << 16u) | x) ||
            !write_command(geometry, 0x23u, UINT32_C(0))) {
            return fail("quad-strip VTX_16 enqueue failed");
        }
    }
    geometry.advance_bus_cycles(54u);
    if (!write_command(geometry, 0x41u)) {
        return fail("quad-strip END_VTXS enqueue failed");
    }
    geometry.advance_bus_cycles(1u);
    const NdsGeometryEngine::Polygon *quad_strip_first = geometry.polygon_at(5u);
    const NdsGeometryEngine::Polygon *quad_strip_second = geometry.polygon_at(6u);
    if (geometry.polygon_count() != 7u ||
        quad_strip_first == nullptr || quad_strip_second == nullptr ||
        quad_strip_first->vertex_indices !=
            std::array<uint32_t, 4>{10u, 11u, 13u, 12u} ||
        quad_strip_second->vertex_indices !=
            std::array<uint32_t, 4>{12u, 13u, 15u, 14u}) {
        return fail("quad-strip shared-edge assembly failed");
    }

    /*
     * The machine clock advances GX commands only at the shared ARM9/ARM7
     * watermark. ARM9 running ahead alone must not consume a FIFO command.
     */
    NdsDisplayControl display(interrupts);
    if (display.install() != Status::Ok) {
        return fail("display device installation for Geometry Engine clock test failed");
    }
    NdsTimerControl timers(memory, interrupts);
    NdsSystemClock clock(timers, display, nullptr, &geometry);
    const uint64_t completed_before_clock = geometry.completed_commands();
    if (!write_command(geometry, 0x10u, 1u) ||
        !write_command(geometry, 0x15u) ||
        geometry.queued_entries() != 2u || geometry.pipe_entries() != 2u ||
        clock.report_cpu_cycles(NDS_ARM9, 40u) != NdsClockStatus::Ok ||
        clock.global_bus_cycles() != 0u ||
        !geometry.busy()) {
        return fail("ARM9 ahead of ARM7 incorrectly advanced Geometry Engine time");
    }
    if (clock.report_cpu_cycles(NDS_ARM7, 20u) != NdsClockStatus::Ok ||
        clock.global_bus_cycles() != 20u ||
        geometry.busy() ||
        geometry.matrix_mode() != 1u ||
        geometry.completed_commands() != completed_before_clock + 2u) {
        return fail("synchronized dual-core clock did not execute GX matrix commands");
    }

    /*
     * SWAP_BUFFERS waits for line 192, exchanges independent render/build
     * RAM, clears the new build RAM after 10 cycles and completes after a
     * further total of 392 post-VBlank cycles.
     */
    if (!write_command(geometry, 0x50u, UINT32_C(3)) ||
        !geometry.busy()) {
        return fail("SWAP_BUFFERS command enqueue failed");
    }
    if (clock.report_cpu_cycles(NDS_ARM9, UINT64_C(817920)) !=
            NdsClockStatus::Ok ||
        clock.global_bus_cycles() != UINT64_C(20) ||
        geometry.swap_waiting_for_vblank()) {
        return fail("ARM9 ahead of ARM7 incorrectly advanced SWAP_BUFFERS");
    }
    if (clock.report_cpu_cycles(NDS_ARM7, UINT64_C(21)) !=
            NdsClockStatus::Ok ||
        clock.global_bus_cycles() != UINT64_C(21) ||
        !geometry.swap_waiting_for_vblank() ||
        geometry.build_buffer_index() != 0u ||
        geometry.render_buffer_index() != 1u) {
        return fail("SWAP_BUFFERS did not wait after command execution started");
    }
    if (clock.report_cpu_cycles(NDS_ARM7, UINT64_C(408960)) !=
            NdsClockStatus::Ok ||
        clock.global_bus_cycles() != UINT64_C(408960) ||
        geometry.swap_waiting_for_vblank() ||
        !geometry.busy() ||
        geometry.build_buffer_index() != 1u ||
        geometry.render_buffer_index() != 0u ||
        geometry.swap_parameters() != 3u ||
        geometry.vertex_count() != 0u ||
        geometry.polygon_count() != 0u ||
        geometry.render_vertex_count() != 16u ||
        geometry.render_polygon_count() != 7u ||
        geometry.render_vertex_at(0u) == nullptr ||
        geometry.render_polygon_at(0u) == nullptr ||
        geometry.render_vertex_at(15u) == nullptr ||
        geometry.render_polygon_at(6u) == nullptr) {
        return fail("VBlank did not exchange render and build Geometry RAM");
    }

    if (clock.report_cpu_cycles(NDS_ARM9, UINT64_C(817940)) !=
            NdsClockStatus::Ok ||
        clock.report_cpu_cycles(NDS_ARM7, UINT64_C(408970)) !=
            NdsClockStatus::Ok ||
        clock.global_bus_cycles() != UINT64_C(408970) ||
        geometry.vertex_count() != 0u ||
        geometry.polygon_count() != 0u ||
        !geometry.busy()) {
        return fail("post-VBlank 10-cycle build buffer clear failed");
    }

    if (clock.report_cpu_cycles(NDS_ARM9, UINT64_C(818704)) !=
            NdsClockStatus::Ok ||
        clock.report_cpu_cycles(NDS_ARM7, UINT64_C(409352)) !=
            NdsClockStatus::Ok ||
        clock.global_bus_cycles() != UINT64_C(409352) ||
        geometry.busy() ||
        geometry.render_vertex_count() != 16u ||
        geometry.render_polygon_count() != 7u ||
        geometry.render_vertex_at(15u) == nullptr ||
        geometry.render_polygon_at(6u) == nullptr) {
        return fail("SWAP_BUFFERS did not complete after 392 post-VBlank cycles");
    }

    /*
     * A triangle with only two vertices is an incomplete polygon list.
     * END_VTXS is a dummy command and does not cure the incomplete primitive.
     */
    NdsGeometryEngine locked_geometry(interrupts);
    if (locked_geometry.install() != Status::Ok ||
        !write_command(locked_geometry, 0x40u, UINT32_C(0))) {
        return fail("incomplete-triangle swap lockup fixture setup failed");
    }
    locked_geometry.advance_bus_cycles(1u);
    const uint32_t first_vertex_xy = UINT32_C(0x00001000);
    if (!write_command(locked_geometry, 0x23u, first_vertex_xy) ||
        !write_command(locked_geometry, 0x23u, UINT32_C(0)) ||
        !write_command(locked_geometry, 0x23u, UINT32_C(0x10000000)) ||
        !write_command(locked_geometry, 0x23u, UINT32_C(0))) {
        return fail("incomplete-triangle VTX_16 fixture enqueue failed");
    }
    locked_geometry.advance_bus_cycles(18u);
    if (locked_geometry.vertex_count() != 2u ||
        locked_geometry.polygon_count() != 0u ||
        !locked_geometry.incomplete_polygon_list()) {
        return fail("two-vertex triangle was not recorded as an incomplete polygon");
    }
    if (!write_command(locked_geometry, 0x41u)) {
        return fail("incomplete-triangle dummy END_VTXS enqueue failed");
    }
    locked_geometry.advance_bus_cycles(1u);
    if (!locked_geometry.vertex_list_open() ||
        !locked_geometry.incomplete_polygon_list()) {
        return fail("END_VTXS incorrectly closed or completed the partial triangle");
    }
    if (!write_command(locked_geometry, 0x50u, UINT32_C(0))) {
        return fail("incomplete-triangle SWAP_BUFFERS enqueue failed");
    }
    locked_geometry.advance_bus_cycles(1u);
    if (!locked_geometry.swap_lockup() ||
        !locked_geometry.swap_waiting_for_vblank() ||
        !locked_geometry.busy() ||
        locked_geometry.build_buffer_index() != 0u ||
        locked_geometry.render_buffer_index() != 1u) {
        return fail("SWAP_BUFFERS with an incomplete polygon did not lock up safely");
    }
    locked_geometry.notify_vblank();
    if (!locked_geometry.swap_lockup() ||
        locked_geometry.swap_waiting_for_vblank() ||
        locked_geometry.build_buffer_index() != 0u ||
        locked_geometry.render_buffer_index() != 1u) {
        return fail("VBlank incorrectly recovered or swapped a locked Geometry Engine");
    }

    std::puts("NDS Geometry Engine FIFO, GXSTAT and matrix-command tests passed");
    return 0;
}

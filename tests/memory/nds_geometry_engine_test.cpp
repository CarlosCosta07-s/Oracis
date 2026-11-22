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
#include <memory>

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

    /* DISP_1DOT_DEPTH is ARM9-only, write-only, and halfword-sized. */
    if (geometry.write(
            NDS_ARM9, NdsGeometryEngine::OneDotDepthAddress, 4u, 0u) !=
            Status::InvalidWidth ||
        geometry.write(
            NDS_ARM9, NdsGeometryEngine::OneDotDepthAddress, 2u,
            UINT32_C(0xFFFF)) != Status::Ok ||
        geometry.read(
            NDS_ARM9, NdsGeometryEngine::OneDotDepthAddress, 2u).status !=
            Status::Unmapped ||
        geometry.write(
            NDS_ARM7, NdsGeometryEngine::OneDotDepthAddress, 2u,
            UINT32_C(1)) != Status::Unmapped) {
        return fail("DISP_1DOT_DEPTH CPU visibility, width or direction failed");
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
                UINT32_C(0x05) * 4u, 4u, 0u) != Status::PermissionDenied) {
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
    /* VTX commands before the first BEGIN_VTXS must not append Vertex RAM. */
    const uint32_t outside_count = geometry.vertex_count();
    if (!write_command(
            geometry, 0x23u, (UINT32_C(0x2000) << 16u) | UINT32_C(0x1000)) ||
        !write_command(geometry, 0x23u, UINT32_C(0x0C00))) {
        return fail("out-of-list vertex fixture enqueue failed");
    }
    geometry.advance_bus_cycles(9u);
    if (geometry.vertex_count() != outside_count) {
        return fail("VTX command outside BEGIN_VTXS appended Vertex RAM");
    }
    if (!write_command(geometry, 0x29u, UINT32_C(0x2A7F1230)) ||
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
    const uint32_t matching_emission =
        (UINT32_C(16) << 16u) |
        (UINT32_C(16) << 21u) |
        (UINT32_C(16) << 26u);
    if (!write_command(geometry, 0x20u, UINT32_C(0x4210)) ||
        !write_command(geometry, 0x31u, matching_emission) ||
        !write_command(geometry, 0x21u, packed_normal) ||
        !write_command(geometry, 0x22u, packed_texcoord)) {
        return fail("vertex color, normal and UV command enqueue failed");
    }
    geometry.advance_bus_cycles(15u);

    const uint32_t vtx16_xy =
        (UINT32_C(0x0800) << 16u) | UINT32_C(0x0400);
    if (!write_command(geometry, 0x23u, vtx16_xy) ||
        !write_command(geometry, 0x23u, UINT32_C(0x0C00))) {
        return fail("VTX_16 parameter enqueue failed");
    }
    geometry.advance_bus_cycles(9u);
    if (geometry.vertex_count() != 0u ||
        geometry.vertex_list_vertex_count() != 1u) {
        return fail("VTX_16 was stored before a complete polygon existed");
    }

    const uint32_t vtx10 =
        UINT32_C(0x010) |
        (UINT32_C(0x3FF) << 10u) |
        (UINT32_C(0x020) << 20u);
    if (!write_command(geometry, 0x24u, vtx10)) {
        return fail("VTX_10 command enqueue failed");
    }
    geometry.advance_bus_cycles(8u);
    if (geometry.vertex_list_vertex_count() != 2u ||
        geometry.vertex_count() != 0u) {
        return fail("VTX_10 was committed before its triangle was complete");
    }

    if (!write_command(
            geometry, 0x25u,
            (UINT32_C(0x0800) << 16u) | UINT32_C(0xF800))) {
        return fail("VTX_XY command enqueue failed");
    }
    geometry.advance_bus_cycles(8u);
    if (geometry.vertex_count() != 3u || geometry.polygon_count() != 1u) {
        return fail("third VTX command did not commit the first triangle");
    }
    const NdsGeometryEngine::Vertex *vertex = geometry.vertex_at(0u);
    if (vertex == nullptr ||
        vertex->source_position !=
            std::array<int32_t, 3>{INT32_C(1024), INT32_C(2048), INT32_C(3072)} ||
        vertex->clip_position !=
            std::array<int32_t, 4>{INT32_C(1024), INT32_C(2048),
                INT32_C(3072), INT32_C(4096)} ||
        vertex->normal !=
            std::array<int32_t, 3>{INT32_C(8), -INT32_C(8), INT32_C(2048)} ||
        vertex->color_rgb555 != UINT16_C(0x4210) ||
        vertex->texture_s != INT16_C(16) || vertex->texture_t != INT16_C(-16)) {
        return fail("first polygon did not preserve VTX_16 attributes");
    }
    vertex = geometry.vertex_at(1u);
    if (vertex == nullptr ||
        vertex->source_position !=
            std::array<int32_t, 3>{INT32_C(1024), -INT32_C(64), INT32_C(2048)}) {
        return fail("VTX_10 signed 10-bit and six-fraction decode was incorrect");
    }
    vertex = geometry.vertex_at(2u);
    if (vertex == nullptr ||
        vertex->source_position !=
            std::array<int32_t, 3>{-INT32_C(2048), INT32_C(2048), INT32_C(2048)}) {
        return fail("VTX_XY failed to retain the previous Z coordinate");
    }

    if (!write_command(
            geometry, 0x26u,
            (UINT32_C(0xF000) << 16u) | UINT32_C(0x0800))) {
        return fail("VTX_XZ command enqueue failed");
    }
    geometry.advance_bus_cycles(8u);
    vertex = geometry.vertex_at(5u);
    if (vertex == nullptr ||
        vertex->source_position !=
            std::array<int32_t, 3>{INT32_C(2048), INT32_C(2048), -INT32_C(4096)}) {
        return fail("VTX_XZ failed to retain the previous Y coordinate");
    }

    if (!write_command(
            geometry, 0x27u,
            (UINT32_C(0x1000) << 16u) | UINT32_C(0xF000))) {
        return fail("VTX_YZ command enqueue failed");
    }
    geometry.advance_bus_cycles(8u);
    vertex = geometry.vertex_at(8u);
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
    vertex = geometry.vertex_at(11u);
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
    if (!ram_count || ((ram_count.value >> 16u) & UINT32_C(0x1FFF)) != 12u ||
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
            std::array<uint32_t, NdsGeometryEngine::MaxClippedPolygonVertices>{
                0u, 1u, 2u, 0u, 0u, 0u, 0u, 0u, 0u, 0u} ||
        strip_first->attributes != UINT32_C(0x2A7F1230) ||
        strip_second->vertex_indices !=
            std::array<uint32_t, NdsGeometryEngine::MaxClippedPolygonVertices>{
                3u, 4u, 5u, 0u, 0u, 0u, 0u, 0u, 0u, 0u}) {
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
        {UINT32_C(0), UINT32_C(0x0800)},
        {UINT32_C(0x0400), UINT32_C(0x0800)},
        {UINT32_C(0x0800), UINT32_C(0x0800)},
        {UINT32_C(0x0C00), UINT32_C(0x0800)}
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
    if (geometry.polygon_count() != 5u || geometry.vertex_count() != 16u ||
        quad == nullptr || quad->vertex_count != 4u ||
        quad->vertex_indices !=
            std::array<uint32_t, NdsGeometryEngine::MaxClippedPolygonVertices>{
                12u, 13u, 14u, 15u, 0u, 0u, 0u, 0u, 0u, 0u} ||
        quad->attributes != UINT32_C(0x12345678)) {
        return fail("separate quadrilateral or deferred polygon attributes failed");
    }

    /* Quad strips add one polygon for every pair after their first quad. */
    if (!write_command(geometry, 0x40u, UINT32_C(3))) {
        return fail("quad-strip BEGIN_VTXS enqueue failed");
    }
    geometry.advance_bus_cycles(1u);
    for (unsigned index = 0u; index < 6u; ++index) {
        const uint32_t x = static_cast<uint32_t>(index) * UINT32_C(0x0300);
        if (!write_command(
                geometry, 0x23u,
                (UINT32_C(0x0800) << 16u) | x) ||
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
        geometry.vertex_count() != 24u ||
        quad_strip_first->vertex_indices !=
            std::array<uint32_t, NdsGeometryEngine::MaxClippedPolygonVertices>{
                16u, 17u, 18u, 19u, 0u, 0u, 0u, 0u, 0u, 0u} ||
        quad_strip_second->vertex_indices !=
            std::array<uint32_t, NdsGeometryEngine::MaxClippedPolygonVertices>{
                20u, 21u, 22u, 23u, 0u, 0u, 0u, 0u, 0u, 0u}) {
        return fail("quad-strip shared-edge assembly failed");
    }

    /*
     * The clip-space transform is a row-vector multiplication:
     * (x,y,z,1) * (Position * Projection). Translation is deliberately
     * nonzero so a transposed matrix/vector implementation cannot pass.
     */
    NdsGeometryEngine transform_geometry(interrupts);
    if (transform_geometry.install() != Status::Ok ||
        !write_command(transform_geometry, 0x10u, 1u) ||
        !write_command(transform_geometry, 0x15u)) {
        return fail("row-vector transform Geometry Engine setup failed");
    }
    transform_geometry.advance_bus_cycles(20u);
    constexpr std::array<uint32_t, 3> world_translation = {
        UINT32_C(0x00000400), UINT32_C(0xFFFFFE00), UINT32_C(0x00000100)
    };
    if (!write_parameters(
            transform_geometry, 0x1Cu, world_translation.data(), 3u)) {
        return fail("world translation command enqueue failed");
    }
    transform_geometry.advance_bus_cycles(22u);
    if (transform_geometry.position_matrix()[12] != INT32_C(1024) ||
        transform_geometry.position_matrix()[13] != -INT32_C(512) ||
        transform_geometry.position_matrix()[14] != INT32_C(256)) {
        return fail("MTX_TRANS did not place translation in the final matrix row");
    }
    if (!write_command(transform_geometry, 0x40u, UINT32_C(0))) {
        return fail("row-vector triangle BEGIN_VTXS enqueue failed");
    }
    transform_geometry.advance_bus_cycles(1u);
    const std::array<std::array<uint32_t, 3>, 3> triangle_positions = {{
        {{UINT32_C(0x0400), UINT32_C(0x0800), UINT32_C(0x0C00)}},
        {{UINT32_C(0x0000), UINT32_C(0x0000), UINT32_C(0x0000)}},
        {{UINT32_C(0x0400), UINT32_C(0x0000), UINT32_C(0x0400)}}
    }};
    for (const auto &position : triangle_positions) {
        const uint32_t xy = (position[1] << 16u) | position[0];
        if (!write_command(transform_geometry, 0x23u, xy) ||
            !write_command(transform_geometry, 0x23u, position[2])) {
            return fail("translated VTX_16 parameter enqueue failed");
        }
    }
    transform_geometry.advance_bus_cycles(27u);
    const NdsGeometryEngine::Vertex *translated_vertex =
        transform_geometry.vertex_at(0u);
    if (transform_geometry.vertex_count() != 3u ||
        transform_geometry.polygon_count() != 1u ||
        translated_vertex == nullptr ||
        translated_vertex->clip_position !=
            std::array<int32_t, 4>{
                INT32_C(2048), INT32_C(1536), INT32_C(3328), INT32_C(4096)}) {
        return fail("row-vector ClipMatrix transform did not apply translation to XYZ");
    }

    /*
     * VIEWPORT maps valid clip-space coordinates into the configured BG0
     * rectangle. DS viewport origin is bottom-left, so screen Y grows up.
     */
    NdsGeometryEngine viewport_geometry(interrupts);
    if (viewport_geometry.install() != Status::Ok) {
        return fail("viewport Geometry Engine installation failed");
    }
    const uint32_t viewport_parameter =
        UINT32_C(10) |
        (UINT32_C(20) << 8u) |
        (UINT32_C(110) << 16u) |
        (UINT32_C(120) << 24u);
    if (!write_command(viewport_geometry, 0x60u, viewport_parameter)) {
        return fail("VIEWPORT command enqueue failed");
    }
    viewport_geometry.advance_bus_cycles(1u);
    if (viewport_geometry.viewport() !=
            std::array<uint8_t, 4>{10u, 20u, 110u, 120u}) {
        return fail("VIEWPORT fields were not decoded in X1/Y1/X2/Y2 order");
    }
    if (!write_command(viewport_geometry, 0x40u, UINT32_C(0))) {
        return fail("viewport triangle BEGIN_VTXS enqueue failed");
    }
    viewport_geometry.advance_bus_cycles(1u);
    const std::array<std::array<uint32_t, 3>, 3> viewport_positions = {{
        {{UINT32_C(0x0800), UINT32_C(0xFFFFF800), UINT32_C(0x0800)}},
        {{UINT32_C(0x0000), UINT32_C(0x0000), UINT32_C(0x0000)}},
        {{UINT32_C(0x0800), UINT32_C(0x0000), UINT32_C(0x0800)}}
    }};
    for (const auto &position : viewport_positions) {
        const uint32_t xy = (position[1] << 16u) | position[0];
        if (!write_command(viewport_geometry, 0x23u, xy) ||
            !write_command(viewport_geometry, 0x23u, position[2])) {
            return fail("viewport VTX_16 enqueue failed");
        }
    }
    viewport_geometry.advance_bus_cycles(27u);
    const NdsGeometryEngine::Vertex *viewport_vertex =
        viewport_geometry.vertex_at(0u);
    if (viewport_geometry.vertex_count() != 3u ||
        viewport_geometry.polygon_count() != 1u ||
        viewport_vertex == nullptr ||
        viewport_vertex->clip_position !=
            std::array<int32_t, 4>{
                INT32_C(2048), -INT32_C(2048), INT32_C(2048), INT32_C(4096)} ||
        !viewport_vertex->screen_position_valid ||
        viewport_vertex->screen_position !=
            std::array<int32_t, 2>{85, 45}) {
        return fail("clip-space vertex was not projected into the configured viewport");
    }

    /* A partially outside triangle is clipped into a screen-ready polygon. */
    NdsGeometryEngine outside_viewport_geometry(interrupts);
    if (outside_viewport_geometry.install() != Status::Ok ||
        !write_command(outside_viewport_geometry, 0x40u, UINT32_C(0))) {
        return fail("outside-viewport fixture setup failed");
    }
    outside_viewport_geometry.advance_bus_cycles(1u);
    if (!write_command(
            outside_viewport_geometry, 0x23u,
            (UINT32_C(0) << 16u) | UINT32_C(0x3000)) ||
        !write_command(outside_viewport_geometry, 0x23u, UINT32_C(0)) ||
        !write_command(
            outside_viewport_geometry, 0x23u,
            (UINT32_C(0) << 16u) | UINT32_C(0x0000)) ||
        !write_command(outside_viewport_geometry, 0x23u, UINT32_C(0)) ||
        !write_command(
            outside_viewport_geometry, 0x23u,
            (UINT32_C(0) << 16u) | UINT32_C(0x0800)) ||
        !write_command(outside_viewport_geometry, 0x23u, UINT32_C(0))) {
        return fail("outside-viewport VTX_16 enqueue failed");
    }
    outside_viewport_geometry.advance_bus_cycles(27u);
    const NdsGeometryEngine::Polygon *right_clipped =
        outside_viewport_geometry.polygon_at(0u);
    if (outside_viewport_geometry.vertex_count() != 4u ||
        outside_viewport_geometry.polygon_count() != 1u ||
        right_clipped == nullptr || right_clipped->vertex_count != 4u) {
        return fail("right-plane clipping did not turn a triangle into a quad");
    }
    bool found_right_intersection = false;
    for (uint8_t index = 0u; index < right_clipped->vertex_count; ++index) {
        const auto *clipped_vertex =
            outside_viewport_geometry.vertex_at(right_clipped->vertex_indices[index]);
        if (clipped_vertex == nullptr || !clipped_vertex->screen_position_valid ||
            clipped_vertex->clip_position[0] > clipped_vertex->clip_position[3]) {
            return fail("clipped right-plane vertex was outside or lacked screen coordinates");
        }
        if (clipped_vertex->clip_position[0] ==
            clipped_vertex->clip_position[3]) {
            found_right_intersection = true;
        }
    }
    if (!found_right_intersection) {
        return fail("right-plane clipping did not generate an intersection vertex");
    }

    /* Exercise left, right, bottom, top, near and far intersections. */
    const std::array<std::array<int32_t, 3>, 6> outside_points = {{
        {{-INT32_C(8192), 0, 0}},
        {{INT32_C(8192), 0, 0}},
        {{0, -INT32_C(8192), 0}},
        {{0, INT32_C(8192), 0}},
        {{0, 0, -INT32_C(8192)}},
        {{0, 0, INT32_C(8192)}}
    }};
    for (const auto &outside_point : outside_points) {
        auto clip_engine = std::make_unique<NdsGeometryEngine>(interrupts);
        if (clip_engine->install() != Status::Ok ||
            !write_command(*clip_engine, 0x40u, UINT32_C(0))) {
            return fail("six-plane clipping fixture setup failed");
        }
        clip_engine->advance_bus_cycles(1u);
        const std::array<std::array<int32_t, 3>, 3> points = {{
            outside_point,
            {{-INT32_C(2048), -INT32_C(2048), 0}},
            {{INT32_C(2048), INT32_C(2048), 0}}
        }};
        for (const auto &point : points) {
            const uint32_t xy =
                (static_cast<uint32_t>(static_cast<uint16_t>(point[1])) << 16u) |
                static_cast<uint16_t>(point[0]);
            if (!write_command(*clip_engine, 0x23u, xy) ||
                !write_command(
                    *clip_engine, 0x23u,
                    static_cast<uint16_t>(point[2]))) {
                return fail("six-plane clipping VTX_16 enqueue failed");
            }
        }
        clip_engine->advance_bus_cycles(27u);
        const auto *polygon = clip_engine->polygon_at(0u);
        if (clip_engine->polygon_count() != 1u ||
            clip_engine->vertex_count() != 4u ||
            polygon == nullptr || polygon->vertex_count != 4u) {
            return fail("one of the six clip planes failed to emit a clipped quad");
        }
        for (uint8_t index = 0u; index < polygon->vertex_count; ++index) {
            const auto *clipped_vertex =
                clip_engine->vertex_at(polygon->vertex_indices[index]);
            if (clipped_vertex == nullptr ||
                !clipped_vertex->screen_position_valid) {
                return fail("six-plane clipping emitted an invalid screen vertex");
            }
            const int64_t x = clipped_vertex->clip_position[0];
            const int64_t y = clipped_vertex->clip_position[1];
            const int64_t z = clipped_vertex->clip_position[2];
            const int64_t w = clipped_vertex->clip_position[3];
            if (x < -w || x > w || y < -w || y > w ||
                z < -w || z > w || w <= 0) {
                return fail("six-plane clipper left an output vertex outside the volume");
            }
        }
    }

    auto fully_outside_engine =
        std::make_unique<NdsGeometryEngine>(interrupts);
    if (fully_outside_engine->install() != Status::Ok ||
        !write_command(*fully_outside_engine, 0x40u, UINT32_C(0))) {
        return fail("fully-outside clipping fixture setup failed");
    }
    fully_outside_engine->advance_bus_cycles(1u);
    const std::array<uint32_t, 3> outside_x = {
        UINT32_C(0x2000), UINT32_C(0x3000), UINT32_C(0x4000)
    };
    for (const uint32_t x : outside_x) {
        if (!write_command(*fully_outside_engine, 0x23u, x) ||
            !write_command(*fully_outside_engine, 0x23u, UINT32_C(0))) {
            return fail("fully-outside VTX_16 enqueue failed");
        }
    }
    fully_outside_engine->advance_bus_cycles(27u);
    if (fully_outside_engine->polygon_count() != 0u ||
        fully_outside_engine->vertex_count() != 0u) {
        return fail("fully outside polygon was retained in Vertex/Polygon RAM");
    }

    /*
     * POLYGON_ATTR controls front/back visibility after clipping. Anti-
     * clockwise polygons are front-facing; reversing the vertices selects
     * the back surface. Repeated-point line segments bypass those flags.
     */
    const auto make_polygon = [&interrupts](
        NdsGeometryEngine &engine, uint32_t attributes,
        bool reverse_winding, bool cross_far_plane,
        bool duplicate_vertex) noexcept {
        if (engine.install() != Status::Ok ||
            !write_command(engine, 0x29u, attributes) ||
            !write_command(engine, 0x40u, UINT32_C(0))) {
            return false;
        }
        engine.advance_bus_cycles(2u);

        std::array<std::array<int16_t, 3>, 3> points = {{
            {{-INT16_C(2048), -INT16_C(2048), 0}},
            {{INT16_C(2048), -INT16_C(2048), 0}},
            {{0, INT16_C(2048), 0}}
        }};
        if (cross_far_plane) points[0][2] = INT16_C(8192);
        if (duplicate_vertex) points[2] = points[1];
        if (reverse_winding) std::swap(points[1], points[2]);

        for (const auto &point : points) {
            const uint32_t xy =
                (static_cast<uint32_t>(static_cast<uint16_t>(point[1])) << 16u) |
                static_cast<uint16_t>(point[0]);
            if (!write_command(engine, 0x23u, xy) ||
                !write_command(
                    engine, 0x23u, static_cast<uint16_t>(point[2]))) {
                return false;
            }
        }
        engine.advance_bus_cycles(27u);
        return true;
    };

    NdsGeometryEngine front_only_geometry(interrupts);
    if (!make_polygon(
            front_only_geometry, UINT32_C(1) << 7u,
            false, false, false)) {
        return fail("front-surface triangle fixture setup failed");
    }
    const NdsGeometryEngine::Polygon *front_polygon =
        front_only_geometry.polygon_at(0u);
    if (front_polygon == nullptr || !front_polygon->front_facing ||
        front_polygon->line_segment || !front_polygon->render_enabled ||
        front_polygon->far_plane_intersection) {
        return fail("anti-clockwise triangle was not accepted as the front surface");
    }

    NdsGeometryEngine back_hidden_geometry(interrupts);
    if (!make_polygon(
            back_hidden_geometry, UINT32_C(1) << 7u,
            true, false, false)) {
        return fail("back-surface hidden triangle fixture setup failed");
    }
    const NdsGeometryEngine::Polygon *back_hidden =
        back_hidden_geometry.polygon_at(0u);
    if (back_hidden == nullptr || back_hidden->front_facing ||
        back_hidden->line_segment || back_hidden->render_enabled) {
        return fail("back-facing triangle ignored the front-only surface flag");
    }

    NdsGeometryEngine back_visible_geometry(interrupts);
    if (!make_polygon(
            back_visible_geometry, UINT32_C(1) << 6u,
            true, false, false)) {
        return fail("back-surface visible triangle fixture setup failed");
    }
    const NdsGeometryEngine::Polygon *back_visible =
        back_visible_geometry.polygon_at(0u);
    if (back_visible == nullptr || back_visible->front_facing ||
        !back_visible->render_enabled) {
        return fail("back-facing triangle was not accepted by the back-surface flag");
    }

    NdsGeometryEngine line_geometry(interrupts);
    if (!make_polygon(line_geometry, 0u, false, false, true)) {
        return fail("line-segment polygon fixture setup failed");
    }
    const NdsGeometryEngine::Polygon *line_polygon =
        line_geometry.polygon_at(0u);
    if (line_polygon == nullptr || !line_polygon->line_segment ||
        !line_polygon->render_enabled) {
        return fail("repeated-vertex line segment was incorrectly face-culled");
    }

    /* POLYGON_ATTR fields are retained as decoded polygon-side metadata. */
    constexpr uint32_t kDecodedAttributes =
        UINT32_C(0x05) | (UINT32_C(2) << 4u) |
        (UINT32_C(1) << 6u) | (UINT32_C(1) << 7u) |
        (UINT32_C(1) << 11u) | (UINT32_C(1) << 14u) |
        (UINT32_C(1) << 15u) | (UINT32_C(17) << 16u) |
        (UINT32_C(0x2A) << 24u);
    NdsGeometryEngine decoded_attributes_geometry(interrupts);
    if (!make_polygon(
            decoded_attributes_geometry, kDecodedAttributes,
            false, false, false)) {
        return fail("decoded POLYGON_ATTR fixture setup failed");
    }
    const NdsGeometryEngine::Polygon *decoded_polygon =
        decoded_attributes_geometry.polygon_at(0u);
    if (decoded_polygon == nullptr ||
        decoded_polygon->light_enable_mask != UINT8_C(0x05) ||
        decoded_polygon->polygon_mode != 2u ||
        !decoded_polygon->translucent_depth_write ||
        !decoded_polygon->depth_equal || !decoded_polygon->fog_enabled ||
        decoded_polygon->alpha != 17u || decoded_polygon->polygon_id != 0x2Au) {
        return fail("POLYGON_ATTR decoded fields did not match the source word");
    }

    /* DISP_1DOT_DEPTH applies only to sub-pixel polygons smaller than 1x1. */
    const auto make_one_dot_triangle = [&interrupts](
        NdsGeometryEngine &engine, uint16_t depth, uint32_t attributes) noexcept {
        if (engine.install() != Status::Ok ||
            engine.write(
                NDS_ARM9, NdsGeometryEngine::OneDotDepthAddress,
                2u, depth) != Status::Ok ||
            !write_command(engine, 0x29u, attributes) ||
            !write_command(engine, 0x40u, UINT32_C(0))) {
            return false;
        }
        engine.advance_bus_cycles(2u);
        const std::array<std::array<int16_t, 3>, 3> points = {{
            {{-INT16_C(1), -INT16_C(1), 0}},
            {{INT16_C(1), -INT16_C(1), 0}},
            {{0, INT16_C(1), 0}}
        }};
        for (const auto &point : points) {
            const uint32_t xy =
                (static_cast<uint32_t>(static_cast<uint16_t>(point[1])) << 16u) |
                static_cast<uint16_t>(point[0]);
            if (!write_command(engine, 0x23u, xy) ||
                !write_command(
                    engine, 0x23u, static_cast<uint16_t>(point[2]))) {
                return false;
            }
        }
        engine.advance_bus_cycles(27u);
        return true;
    };

    constexpr uint32_t kBothFaces =
        (UINT32_C(1) << 6u) | (UINT32_C(1) << 7u);
    NdsGeometryEngine one_dot_hidden_geometry(interrupts);
    if (!make_one_dot_triangle(
            one_dot_hidden_geometry, UINT16_C(7), kBothFaces)) {
        return fail("1-dot hidden triangle fixture setup failed");
    }
    const auto *one_dot_hidden = one_dot_hidden_geometry.polygon_at(0u);
    if (one_dot_hidden == nullptr || !one_dot_hidden->zero_dot_polygon ||
        one_dot_hidden->one_dot_depth_visible || one_dot_hidden->render_enabled) {
        return fail("1-dot polygon beyond DISP_1DOT_DEPTH was not hidden");
    }

    NdsGeometryEngine one_dot_visible_geometry(interrupts);
    if (!make_one_dot_triangle(
            one_dot_visible_geometry, UINT16_C(8), kBothFaces)) {
        return fail("1-dot visible triangle fixture setup failed");
    }
    const auto *one_dot_visible = one_dot_visible_geometry.polygon_at(0u);
    if (one_dot_visible == nullptr || !one_dot_visible->zero_dot_polygon ||
        !one_dot_visible->one_dot_depth_visible || !one_dot_visible->render_enabled) {
        return fail("1-dot polygon at the DISP_1DOT_DEPTH boundary was hidden");
    }

    NdsGeometryEngine one_dot_bypass_geometry(interrupts);
    if (!make_one_dot_triangle(
            one_dot_bypass_geometry, UINT16_C(7),
            kBothFaces | (UINT32_C(1) << 13u))) {
        return fail("1-dot bypass triangle fixture setup failed");
    }
    const auto *one_dot_bypass = one_dot_bypass_geometry.polygon_at(0u);
    if (one_dot_bypass == nullptr || !one_dot_bypass->zero_dot_polygon ||
        !one_dot_bypass->one_dot_depth_visible || !one_dot_bypass->render_enabled) {
        return fail("POLYGON_ATTR bit 13 did not bypass the 1-dot depth test");
    }

    /* POLYGON_ATTR bit 12 gates polygons which intersect the far clip plane. */
    NdsGeometryEngine far_hidden_geometry(interrupts);
    if (!make_polygon(
            far_hidden_geometry, UINT32_C(1) << 7u,
            false, true, false)) {
        return fail("far-plane hidden polygon fixture setup failed");
    }
    const NdsGeometryEngine::Polygon *far_hidden =
        far_hidden_geometry.polygon_at(0u);
    if (far_hidden == nullptr || !far_hidden->far_plane_intersection ||
        far_hidden->render_enabled || far_hidden->vertex_count != 4u) {
        return fail("far-plane crossing polygon ignored the disabled far-plane flag");
    }

    NdsGeometryEngine far_visible_geometry(interrupts);
    if (!make_polygon(
            far_visible_geometry,
            (UINT32_C(1) << 7u) | (UINT32_C(1) << 12u),
            false, true, false)) {
        return fail("far-plane visible polygon fixture setup failed");
    }
    const NdsGeometryEngine::Polygon *far_visible =
        far_visible_geometry.polygon_at(0u);
    if (far_visible == nullptr || !far_visible->far_plane_intersection ||
        !far_visible->render_enabled || far_visible->vertex_count != 4u) {
        return fail("POLYGON_ATTR bit 12 did not allow a clipped far-plane polygon");
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
        geometry.render_vertex_count() != 24u ||
        geometry.render_polygon_count() != 7u ||
        geometry.render_vertex_at(0u) == nullptr ||
        geometry.render_polygon_at(0u) == nullptr ||
        geometry.render_vertex_at(23u) == nullptr ||
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
        geometry.render_vertex_count() != 24u ||
        geometry.render_polygon_count() != 7u ||
        geometry.render_vertex_at(23u) == nullptr ||
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
    if (locked_geometry.vertex_count() != 0u ||
        locked_geometry.vertex_list_vertex_count() != 2u ||
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

    /*
     * Lighting commands update material/light state, and NORMAL latches the
     * resulting RGB555 vertex color. Near-unit 1.9 vectors avoid overflow.
     */
    auto lighting_geometry =
        std::make_unique<NdsGeometryEngine>(interrupts);
    if (lighting_geometry->install() != Status::Ok) {
        return fail("lighting Geometry Engine installation failed");
    }
    const auto run_lighting_command = [&lighting_geometry](
        uint8_t command, uint32_t parameter, uint32_t cycles) noexcept {
        if (!write_command(*lighting_geometry, command, parameter)) return false;
        lighting_geometry->advance_bus_cycles(cycles);
        return true;
    };
    const uint32_t diffuse_ambient =
        UINT32_C(31) | (UINT32_C(1) << 15u) |
        (UINT32_C(31) << 21u);
    const uint32_t specular_emission = UINT32_C(5) << 26u;
    const uint32_t light_negative_z = UINT32_C(0x201) << 20u;
    const uint32_t white_light =
        UINT32_C(31) | (UINT32_C(31) << 5u) | (UINT32_C(31) << 10u);
    const uint32_t lit_polygon_attributes =
        UINT32_C(1) | (UINT32_C(1) << 6u) | (UINT32_C(1) << 7u);
    const uint32_t normal_positive_z = UINT32_C(0x1FF) << 20u;
    if (!run_lighting_command(0x30u, diffuse_ambient, 4u) ||
        !run_lighting_command(0x31u, specular_emission, 4u) ||
        !run_lighting_command(0x32u, light_negative_z, 6u) ||
        !run_lighting_command(0x33u, white_light, 1u) ||
        !run_lighting_command(0x29u, lit_polygon_attributes, 1u) ||
        !run_lighting_command(0x40u, 0u, 1u) ||
        !run_lighting_command(0x21u, normal_positive_z, 9u)) {
        return fail("lighting commands could not be executed");
    }
    const auto pack_vtx10 = [](
        int32_t x, int32_t y, int32_t z) noexcept {
        return (static_cast<uint32_t>(x) & UINT32_C(0x3FF)) |
            ((static_cast<uint32_t>(y) & UINT32_C(0x3FF)) << 10u) |
            ((static_cast<uint32_t>(z) & UINT32_C(0x3FF)) << 20u);
    };
    const std::array<uint32_t, 3> lit_triangle = {
        pack_vtx10(-16, -16, 0),
        pack_vtx10(16, -16, 0),
        pack_vtx10(0, 16, 0)
    };
    for (const uint32_t vertex : lit_triangle) {
        if (!run_lighting_command(0x24u, vertex, 8u)) {
            return fail("lit VTX_10 command enqueue failed");
        }
    }
    const NdsGeometryEngine::Vertex *lit_vertex =
        lighting_geometry->vertex_at(0u);
    const uint16_t expected_lit_color = static_cast<uint16_t>(
        UINT16_C(30) | (UINT16_C(31) << 5u) | (UINT16_C(5) << 10u));
    if (lighting_geometry->vertex_count() != 3u ||
        lit_vertex == nullptr || lit_vertex->color_rgb555 != expected_lit_color ||
        lit_vertex->normal != std::array<int32_t, 3>{0, 0, 4088}) {
        return fail("NORMAL did not combine diffuse, ambient and emission colors");
    }

    /* The 128-entry shininess table attenuates specular reflection. */
    auto shininess_geometry =
        std::make_unique<NdsGeometryEngine>(interrupts);
    if (shininess_geometry->install() != Status::Ok) {
        return fail("shininess Geometry Engine installation failed");
    }
    const auto run_shininess_command = [&shininess_geometry](
        uint8_t command, uint32_t parameter, uint32_t cycles) noexcept {
        if (!write_command(*shininess_geometry, command, parameter)) return false;
        shininess_geometry->advance_bus_cycles(cycles);
        return true;
    };
    const uint32_t shininess_word = UINT32_C(0x80808080);
    for (unsigned index = 0u; index < 32u; ++index) {
        if (!write_command(*shininess_geometry, 0x34u, shininess_word)) {
            return fail("SHININESS table word enqueue failed");
        }
    }
    shininess_geometry->advance_bus_cycles(32u);
    const uint32_t specular_material =
        UINT32_C(31) | (UINT32_C(1) << 15u);
    if (!run_shininess_command(0x31u, specular_material, 4u) ||
        !run_shininess_command(0x32u, light_negative_z, 6u) ||
        !run_shininess_command(0x33u, white_light, 1u) ||
        !run_shininess_command(0x29u, lit_polygon_attributes, 1u) ||
        !run_shininess_command(0x40u, 0u, 1u) ||
        !run_shininess_command(0x21u, normal_positive_z, 9u)) {
        return fail("specular lighting setup failed");
    }
    for (const uint32_t vertex : lit_triangle) {
        if (!run_shininess_command(0x24u, vertex, 8u)) {
            return fail("specular VTX_10 enqueue failed");
        }
    }
    const NdsGeometryEngine::Vertex *specular_vertex =
        shininess_geometry->vertex_at(0u);
    if (specular_vertex == nullptr ||
        specular_vertex->color_rgb555 != UINT16_C(15)) {
        return fail("SHININESS table did not scale the specular vertex color");
    }

    /*
     * DS texgen modes transform at different command boundaries:
     * mode 1 at TEXCOORD, mode 2 at NORMAL, and mode 3 at each VTX command.
     */
    const auto prepare_texgen = [](
        NdsGeometryEngine &engine,
        uint8_t mode,
        const NdsGeometryEngine::Matrix &texture_matrix,
        uint16_t source_s,
        uint16_t source_t) noexcept {
        if (engine.install() != Status::Ok ||
            !write_command(engine, 0x10u, 3u)) {
            return false;
        }
        engine.advance_bus_cycles(1u);
        std::array<uint32_t, 16> matrix_words{};
        for (unsigned index = 0u; index < matrix_words.size(); ++index) {
            matrix_words[index] = std::bit_cast<uint32_t>(texture_matrix[index]);
        }
        if (!write_parameters(engine, 0x16u, matrix_words.data(), 16u)) {
            return false;
        }
        engine.advance_bus_cycles(34u);
        const uint32_t teximage_parameters =
            (static_cast<uint32_t>(mode) << 30u) |
            (UINT32_C(3) << 26u) | (UINT32_C(1) << 20u);
        const uint32_t texcoord =
            static_cast<uint32_t>(source_s) |
            (static_cast<uint32_t>(source_t) << 16u);
        if (!write_command(engine, 0x2Au, teximage_parameters)) return false;
        engine.advance_bus_cycles(1u);
        if (!write_command(engine, 0x22u, texcoord)) return false;
        engine.advance_bus_cycles(1u);
        if (!write_command(engine, 0x40u, 0u)) return false;
        engine.advance_bus_cycles(1u);
        return true;
    };
    const auto append_texgen_triangle = [](
        NdsGeometryEngine &engine, bool send_normal) noexcept {
        if (send_normal &&
            !write_command(engine, 0x21u, UINT32_C(0x1FF) << 20u)) {
            return false;
        }
        if (send_normal) engine.advance_bus_cycles(9u);
        const std::array<uint32_t, 3> positions = {
            (UINT32_C(0x3E0) << 10u) | UINT32_C(64),
            (UINT32_C(0x3F0) << 10u) | UINT32_C(0x3F0),
            (UINT32_C(16) << 10u) | UINT32_C(0x3F0)
        };
        for (const uint32_t position : positions) {
            if (!write_command(engine, 0x24u, position)) return false;
            engine.advance_bus_cycles(8u);
        }
        return true;
    };

    auto texcoord_matrix = NdsGeometryEngine::identity_matrix();
    texcoord_matrix[0] = INT32_C(8192);
    texcoord_matrix[5] = INT32_C(12288);
    texcoord_matrix[8] = INT32_C(4096);
    texcoord_matrix[13] = INT32_C(8192);
    auto texgen_mode1 = std::make_unique<NdsGeometryEngine>(interrupts);
    if (!prepare_texgen(
            *texgen_mode1, 1u, texcoord_matrix,
            UINT16_C(16), UINT16_C(32)) ||
        !append_texgen_triangle(*texgen_mode1, false)) {
        return fail("mode-1 texture-coordinate transform fixture failed");
    }
    const auto *mode1_vertex = texgen_mode1->vertex_at(0u);
    const auto *mode1_polygon = texgen_mode1->polygon_at(0u);
    if (mode1_vertex == nullptr || mode1_polygon == nullptr ||
        mode1_vertex->texture_s != 33 || mode1_vertex->texture_t != 98 ||
        mode1_polygon->texture_parameters !=
            ((UINT32_C(1) << 30u) | (UINT32_C(3) << 26u) |
             (UINT32_C(1) << 20u))) {
        return fail("mode-1 TEXCOORD source did not use the texture matrix");
    }

    auto normal_matrix = NdsGeometryEngine::identity_matrix();
    normal_matrix[8] = INT32_C(4096);
    normal_matrix[9] = INT32_C(8192);
    auto texgen_mode2 = std::make_unique<NdsGeometryEngine>(interrupts);
    if (!prepare_texgen(
            *texgen_mode2, 2u, normal_matrix,
            UINT16_C(16), UINT16_C(32)) ||
        !append_texgen_triangle(*texgen_mode2, true)) {
        return fail("mode-2 normal-source texture transform fixture failed");
    }
    const auto *mode2_vertex = texgen_mode2->vertex_at(0u);
    const auto *mode2_polygon = texgen_mode2->polygon_at(0u);
    if (mode2_vertex == nullptr || mode2_polygon == nullptr ||
        mode2_vertex->texture_s != 31 || mode2_vertex->texture_t != 63 ||
        mode2_polygon->texture_parameters !=
            ((UINT32_C(2) << 30u) | (UINT32_C(3) << 26u) |
             (UINT32_C(1) << 20u))) {
        return fail("mode-2 NORMAL source did not use the texture matrix");
    }

    auto vertex_matrix = NdsGeometryEngine::identity_matrix();
    auto texgen_mode3 = std::make_unique<NdsGeometryEngine>(interrupts);
    if (!prepare_texgen(
            *texgen_mode3, 3u, vertex_matrix,
            UINT16_C(16), UINT16_C(32)) ||
        !append_texgen_triangle(*texgen_mode3, false)) {
        return fail("mode-3 vertex-source texture transform fixture failed");
    }
    const auto *mode3_vertex = texgen_mode3->vertex_at(0u);
    const auto *mode3_polygon = texgen_mode3->polygon_at(0u);
    if (mode3_vertex == nullptr || mode3_polygon == nullptr ||
        mode3_vertex->texture_s != 32 || mode3_vertex->texture_t != 24 ||
        mode3_polygon->texture_parameters !=
            ((UINT32_C(3) << 30u) | (UINT32_C(3) << 26u) |
             (UINT32_C(1) << 20u))) {
        return fail("mode-3 VTX source did not use the texture matrix");
    }

    std::puts("NDS Geometry Engine FIFO, texgen, lighting and matrix-command tests passed");
    return 0;
}

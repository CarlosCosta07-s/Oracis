/*
 * Safe extraction of Metrowerks global-destructor registrations for one NDS
 * overlay. The guest chain is detached atomically with respect to the
 * cooperative scheduler; this class does not invoke guest destructors.
 */
#ifndef ORACIS_MACHINE_NDS_OVERLAY_DESTRUCTOR_CHAIN_HPP
#define ORACIS_MACHINE_NDS_OVERLAY_DESTRUCTOR_CHAIN_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

#include "oracis/memory/guest_memory.hpp"

namespace oracis::machine {

enum class NdsOverlayDestructorChainStatus : uint8_t {
    Ok = 0,
    InvalidCpu,
    InvalidMetadata,
    MemoryUnavailable,
    ChainCycle,
    ChainCapacityExceeded,
    DuplicateNode,
    WriteFailed,
    RollbackFailed
};

/*
 * Guest-side MWiDestructorChain node for the 32-bit ARM ABI:
 *   +0x00 next
 *   +0x04 dtor (function pointer; bit 0 carries Thumb state)
 *   +0x08 obj
 *
 * These are guest addresses/values, never host pointers.
 */
struct NdsOverlayDestructorRecord {
    uint32_t chain_node_address = 0u;
    uint32_t destructor_pointer = 0u;
    uint32_t object_address = 0u;
};

class NdsOverlayDestructorChain final {
public:
    static constexpr std::size_t MaxNodes = 16384u;
    static constexpr uint32_t NodeSize = 12u;

    explicit NdsOverlayDestructorChain(
        memory::GuestMemory &memory) noexcept
        : memory_(memory) {}

    NdsOverlayDestructorChain(const NdsOverlayDestructorChain &) = delete;
    NdsOverlayDestructorChain &operator=(
        const NdsOverlayDestructorChain &) = delete;

    /*
     * Detach registrations owned by [overlay_ram_address,
     * overlay_ram_address + ram_size + bss_size) from the guest global chain.
     *
     * The head_pointer_address is the guest address of the 32-bit
     * __global_destructor_chain head variable. Nodes selected by either
     * object-in-overlay or (obj == 0 && dtor-in-overlay) are returned in
     * guest chain order and linked together as a detached chain. Unselected
     * nodes remain linked in their original relative order.
     *
     * The caller must invoke this only when the selected CPU is at a safe
     * scheduler boundary. On any validation/write failure, the output vector
     * remains unchanged and writes already performed are rolled back.
     * This method never calls guest code.
     */
    NdsOverlayDestructorChainStatus detach_for_overlay(
        NdsCpu cpu,
        uint32_t head_pointer_address,
        uint32_t overlay_ram_address,
        uint32_t ram_size,
        uint32_t bss_size,
        std::vector<NdsOverlayDestructorRecord> &detached) noexcept;

    /*
     * Metrowerks __register_global_object ABI helper. The caller supplies
     * guest arguments obj, dtor and chain, plus the guest address of
     * __global_destructor_chain. The new 12-byte node is inserted at the
     * global head. Existing nodes are checked for cycles/duplicate storage
     * before mutation; node fields are written before the head is published.
     */
    NdsOverlayDestructorChainStatus register_global_object(
        NdsCpu cpu,
        uint32_t head_pointer_address,
        uint32_t object_address,
        uint32_t destructor_pointer,
        uint32_t chain_node_address) noexcept;

private:
    memory::GuestMemory &memory_;
};

const char *nds_overlay_destructor_chain_status_name(
    NdsOverlayDestructorChainStatus status) noexcept;

} // namespace oracis::machine

#endif /* ORACIS_MACHINE_NDS_OVERLAY_DESTRUCTOR_CHAIN_HPP */

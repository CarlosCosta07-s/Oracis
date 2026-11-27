#include "oracis/machine/nds_overlay_destructor_chain.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <new>
#include <set>
#include <unordered_set>
#include <utility>

namespace oracis::machine {
namespace {

struct ChainNodeSnapshot {
    uint32_t address = 0u;
    uint32_t next = 0u;
    uint32_t destructor = 0u;
    uint32_t object = 0u;
    bool selected = false;
};

struct LinkWrite {
    uint32_t address = 0u;
    uint32_t old_value = 0u;
    uint32_t new_value = 0u;
};

bool range_contains(
    uint64_t begin, uint64_t end, uint32_t address) noexcept
{
    return static_cast<uint64_t>(address) >= begin &&
        static_cast<uint64_t>(address) < end;
}

NdsOverlayDestructorChainStatus read_word(
    const memory::GuestMemory &memory,
    NdsCpu cpu,
    uint32_t address,
    uint32_t &value) noexcept
{
    const auto result = memory.read(cpu, address, 4u);
    if (!result) return NdsOverlayDestructorChainStatus::MemoryUnavailable;
    value = result.value;
    return NdsOverlayDestructorChainStatus::Ok;
}

bool write_word(
    memory::GuestMemory &memory,
    NdsCpu cpu,
    uint32_t address,
    uint32_t value) noexcept
{
    return memory.write(cpu, address, 4u, value) ==
        memory::Status::Ok;
}

} // namespace

NdsOverlayDestructorChainStatus
NdsOverlayDestructorChain::detach_for_overlay(
    NdsCpu cpu,
    uint32_t head_pointer_address,
    uint32_t overlay_ram_address,
    uint32_t ram_size,
    uint32_t bss_size,
    std::vector<NdsOverlayDestructorRecord> &detached) noexcept
{
    if (cpu != NDS_ARM9 && cpu != NDS_ARM7) {
        return NdsOverlayDestructorChainStatus::InvalidCpu;
    }
    if (head_pointer_address == 0u ||
        (head_pointer_address & 3u) != 0u ||
        ram_size == 0u) {
        return NdsOverlayDestructorChainStatus::InvalidMetadata;
    }

    const uint64_t overlay_begin = overlay_ram_address;
    const uint64_t code_end =
        overlay_begin + static_cast<uint64_t>(ram_size);
    const uint64_t overlay_end =
        code_end + static_cast<uint64_t>(bss_size);
    if (code_end > UINT64_C(0x1'0000'0000) ||
        overlay_end > UINT64_C(0x1'0000'0000)) {
        return NdsOverlayDestructorChainStatus::InvalidMetadata;
    }

    if (memory_.validate_read(cpu, head_pointer_address, 4u) !=
            memory::Status::Ok ||
        memory_.validate_write(cpu, head_pointer_address, 4u) !=
            memory::Status::Ok) {
        return NdsOverlayDestructorChainStatus::MemoryUnavailable;
    }

    try {
        uint32_t global_head = 0u;
        const auto head_status = read_word(
            memory_, cpu, head_pointer_address, global_head);
        if (head_status != NdsOverlayDestructorChainStatus::Ok) {
            return head_status;
        }
        if (global_head == 0u) {
            detached.clear();
            return NdsOverlayDestructorChainStatus::Ok;
        }

        std::vector<ChainNodeSnapshot> nodes;
        nodes.reserve(64u);
        std::unordered_set<uint32_t> visited;
        visited.reserve(64u);
        std::set<uint32_t> node_addresses;

        const uint64_t head_begin = head_pointer_address;
        const uint64_t head_end = head_begin + 4u;
        uint32_t current = global_head;
        while (current != 0u) {
            if ((current & 3u) != 0u ||
                static_cast<uint64_t>(current) + NodeSize >
                    UINT64_C(0x1'0000'0000)) {
                return NdsOverlayDestructorChainStatus::InvalidMetadata;
            }
            if (!visited.insert(current).second) {
                return NdsOverlayDestructorChainStatus::ChainCycle;
            }
            if (nodes.size() >= MaxNodes) {
                return NdsOverlayDestructorChainStatus::ChainCapacityExceeded;
            }

            const uint64_t node_begin = current;
            const uint64_t node_end = node_begin + NodeSize;
            if (node_begin < head_end && head_begin < node_end) {
                return NdsOverlayDestructorChainStatus::InvalidMetadata;
            }
            const auto next_node = node_addresses.lower_bound(current);
            if (next_node != node_addresses.end() &&
                node_end > static_cast<uint64_t>(*next_node)) {
                return NdsOverlayDestructorChainStatus::DuplicateNode;
            }
            if (next_node != node_addresses.begin()) {
                const uint32_t previous = *std::prev(next_node);
                if (static_cast<uint64_t>(previous) + NodeSize > node_begin) {
                    return NdsOverlayDestructorChainStatus::DuplicateNode;
                }
            }
            node_addresses.insert(current);
            if (memory_.validate_read(cpu, current, NodeSize) !=
                    memory::Status::Ok ||
                memory_.validate_write(cpu, current, NodeSize) !=
                    memory::Status::Ok) {
                return NdsOverlayDestructorChainStatus::MemoryUnavailable;
            }

            ChainNodeSnapshot node;
            node.address = current;
            auto status = read_word(memory_, cpu, current, node.next);
            if (status != NdsOverlayDestructorChainStatus::Ok) return status;
            status = read_word(
                memory_, cpu, current + 4u, node.destructor);
            if (status != NdsOverlayDestructorChainStatus::Ok) return status;
            status = read_word(
                memory_, cpu, current + 8u, node.object);
            if (status != NdsOverlayDestructorChainStatus::Ok) return status;

            if (node.next != 0u && (node.next & 3u) != 0u) {
                return NdsOverlayDestructorChainStatus::InvalidMetadata;
            }

            const bool object_in_overlay =
                node.object != 0u &&
                range_contains(overlay_begin, overlay_end, node.object);
            const bool null_object_destructor_in_overlay =
                node.object == 0u && node.destructor != 0u &&
                range_contains(overlay_begin, overlay_end, node.destructor);
            node.selected =
                object_in_overlay || null_object_destructor_in_overlay;
            nodes.push_back(node);
            current = node.next;
        }

        std::vector<NdsOverlayDestructorRecord> selected;
        std::vector<uint32_t> retained_addresses;
        selected.reserve(nodes.size());
        retained_addresses.reserve(nodes.size());

        for (const ChainNodeSnapshot &node : nodes) {
            if (node.selected) {
                selected.push_back({
                    node.address, node.destructor, node.object
                });
            } else {
                retained_addresses.push_back(node.address);
            }
        }

        if (selected.empty()) {
            detached.clear();
            return NdsOverlayDestructorChainStatus::Ok;
        }

        std::vector<LinkWrite> writes;
        writes.reserve(nodes.size() + 1u);

        /* Preserve the relative order of nodes that stay on the global chain. */
        for (std::size_t index = 0u;
             index < retained_addresses.size(); ++index) {
            const uint32_t address = retained_addresses[index];
            const uint32_t new_next =
                index + 1u < retained_addresses.size()
                    ? retained_addresses[index + 1u]
                    : 0u;
            const auto found = std::find_if(
                nodes.begin(), nodes.end(),
                [address](const ChainNodeSnapshot &node) {
                    return node.address == address;
                });
            if (found == nodes.end()) {
                return NdsOverlayDestructorChainStatus::InvalidMetadata;
            }
            if (found->next != new_next) {
                writes.push_back({address, found->next, new_next});
            }
        }

        /* Match NitroSDK's detached list: selected nodes remain linked in
           the original guest chain order while their destructors run. */
        for (std::size_t index = 0u; index < selected.size(); ++index) {
            const uint32_t address = selected[index].chain_node_address;
            const uint32_t new_next =
                index + 1u < selected.size()
                    ? selected[index + 1u].chain_node_address
                    : 0u;
            const auto found = std::find_if(
                nodes.begin(), nodes.end(),
                [address](const ChainNodeSnapshot &node) {
                    return node.address == address;
                });
            if (found == nodes.end()) {
                return NdsOverlayDestructorChainStatus::InvalidMetadata;
            }
            if (found->next != new_next) {
                writes.push_back({address, found->next, new_next});
            }
        }

        const uint32_t new_global_head =
            retained_addresses.empty() ? 0u : retained_addresses.front();
        if (new_global_head != global_head) {
            writes.push_back({
                head_pointer_address, global_head, new_global_head
            });
        }

        std::size_t applied = 0u;
        for (; applied < writes.size(); ++applied) {
            const LinkWrite &write = writes[applied];
            if (!write_word(memory_, cpu, write.address, write.new_value)) {
                bool rollback_ok = true;
                while (applied != 0u) {
                    --applied;
                    const LinkWrite &rollback = writes[applied];
                    if (!write_word(
                            memory_, cpu, rollback.address,
                            rollback.old_value)) {
                        rollback_ok = false;
                    }
                }
                return rollback_ok
                    ? NdsOverlayDestructorChainStatus::WriteFailed
                    : NdsOverlayDestructorChainStatus::RollbackFailed;
            }
        }

        detached.swap(selected);
        return NdsOverlayDestructorChainStatus::Ok;
    } catch (const std::bad_alloc &) {
        return NdsOverlayDestructorChainStatus::AllocationFailed;
    } catch (...) {
        return NdsOverlayDestructorChainStatus::MemoryUnavailable;
    }
}

NdsOverlayDestructorChainStatus
NdsOverlayDestructorChain::register_global_object(
    NdsCpu cpu,
    uint32_t head_pointer_address,
    uint32_t object_address,
    uint32_t destructor_pointer,
    uint32_t chain_node_address) noexcept
{
    if (cpu != NDS_ARM9 && cpu != NDS_ARM7) {
        return NdsOverlayDestructorChainStatus::InvalidCpu;
    }
    if (head_pointer_address == 0u ||
        (head_pointer_address & 3u) != 0u ||
        chain_node_address == 0u ||
        (chain_node_address & 3u) != 0u ||
        static_cast<uint64_t>(head_pointer_address) + 4u >
            UINT64_C(0x1'0000'0000) ||
        static_cast<uint64_t>(chain_node_address) + NodeSize >
            UINT64_C(0x1'0000'0000)) {
        return NdsOverlayDestructorChainStatus::InvalidMetadata;
    }

    const uint64_t head_begin = head_pointer_address;
    const uint64_t head_end = head_begin + 4u;
    const uint64_t node_begin = chain_node_address;
    const uint64_t node_end = node_begin + NodeSize;
    if (head_begin < node_end && node_begin < head_end) {
        return NdsOverlayDestructorChainStatus::InvalidMetadata;
    }

    if (memory_.validate_read(cpu, head_pointer_address, 4u) !=
            memory::Status::Ok ||
        memory_.validate_write(cpu, head_pointer_address, 4u) !=
            memory::Status::Ok ||
        memory_.validate_read(cpu, chain_node_address, NodeSize) !=
            memory::Status::Ok ||
        memory_.validate_write(cpu, chain_node_address, NodeSize) !=
            memory::Status::Ok) {
        return NdsOverlayDestructorChainStatus::MemoryUnavailable;
    }

    try {
        uint32_t global_head = 0u;
        const auto head_status = read_word(
            memory_, cpu, head_pointer_address, global_head);
        if (head_status != NdsOverlayDestructorChainStatus::Ok) {
            return head_status;
        }

        /*
         * The SDK helper inserts at the head. Walk the existing next links
         * first so a node cannot be linked twice and corrupt the global chain.
         */
        std::unordered_set<uint32_t> visited;
        visited.reserve(64u);
        std::set<uint32_t> existing_node_addresses;
        uint32_t current = global_head;
        std::size_t count = 0u;
        while (current != 0u) {
            if ((current & 3u) != 0u ||
                static_cast<uint64_t>(current) + NodeSize >
                    UINT64_C(0x1'0000'0000)) {
                return NdsOverlayDestructorChainStatus::InvalidMetadata;
            }
            if (!visited.insert(current).second) {
                return NdsOverlayDestructorChainStatus::ChainCycle;
            }
            const uint64_t current_begin = current;
            const uint64_t current_end =
                current_begin + NodeSize;
            if (current_begin < head_end && head_begin < current_end) {
                return NdsOverlayDestructorChainStatus::InvalidMetadata;
            }
            if (current_begin < node_end && node_begin < current_end) {
                return NdsOverlayDestructorChainStatus::DuplicateNode;
            }
            const auto next_existing =
                existing_node_addresses.lower_bound(current);
            if (next_existing != existing_node_addresses.end() &&
                current_end > static_cast<uint64_t>(*next_existing)) {
                return NdsOverlayDestructorChainStatus::DuplicateNode;
            }
            if (next_existing != existing_node_addresses.begin()) {
                const uint32_t previous = *std::prev(next_existing);
                if (static_cast<uint64_t>(previous) + NodeSize >
                    current_begin) {
                    return NdsOverlayDestructorChainStatus::DuplicateNode;
                }
            }
            existing_node_addresses.insert(current);
            if (count >= MaxNodes) {
                return NdsOverlayDestructorChainStatus::ChainCapacityExceeded;
            }
            ++count;

            if (memory_.validate_read(cpu, current, NodeSize) !=
                memory::Status::Ok) {
                return NdsOverlayDestructorChainStatus::MemoryUnavailable;
            }
            uint32_t next = 0u;
            const auto next_status = read_word(
                memory_, cpu, current, next);
            if (next_status != NdsOverlayDestructorChainStatus::Ok) {
                return next_status;
            }
            if (next != 0u && (next & 3u) != 0u) {
                return NdsOverlayDestructorChainStatus::InvalidMetadata;
            }
            current = next;
        }

        std::array<uint8_t, NodeSize> old_bytes{};
        for (uint32_t offset = 0u; offset < NodeSize; offset += 4u) {
            uint32_t value = 0u;
            const auto status = read_word(
                memory_, cpu, chain_node_address + offset, value);
            if (status != NdsOverlayDestructorChainStatus::Ok) {
                return status;
            }
            old_bytes[offset] = static_cast<uint8_t>(value);
            old_bytes[offset + 1u] = static_cast<uint8_t>(value >> 8u);
            old_bytes[offset + 2u] = static_cast<uint8_t>(value >> 16u);
            old_bytes[offset + 3u] = static_cast<uint8_t>(value >> 24u);
        }

        std::array<uint8_t, NodeSize> new_bytes{};
        const uint32_t values[3]{
            global_head, destructor_pointer, object_address
        };
        for (uint32_t word = 0u; word < 3u; ++word) {
            const uint32_t value = values[word];
            const uint32_t offset = word * 4u;
            new_bytes[offset] = static_cast<uint8_t>(value);
            new_bytes[offset + 1u] = static_cast<uint8_t>(value >> 8u);
            new_bytes[offset + 2u] = static_cast<uint8_t>(value >> 16u);
            new_bytes[offset + 3u] = static_cast<uint8_t>(value >> 24u);
        }

        if (memory_.write_bytes(
                cpu, chain_node_address,
                new_bytes.data(), NodeSize) != memory::Status::Ok) {
            return NdsOverlayDestructorChainStatus::WriteFailed;
        }

        if (!write_word(
                memory_, cpu, head_pointer_address, chain_node_address)) {
            const bool rollback_ok = memory_.write_bytes(
                cpu, chain_node_address, old_bytes.data(), NodeSize) ==
                memory::Status::Ok;
            return rollback_ok
                ? NdsOverlayDestructorChainStatus::WriteFailed
                : NdsOverlayDestructorChainStatus::RollbackFailed;
        }

        return NdsOverlayDestructorChainStatus::Ok;
    } catch (const std::bad_alloc &) {
        return NdsOverlayDestructorChainStatus::AllocationFailed;
    } catch (...) {
        return NdsOverlayDestructorChainStatus::MemoryUnavailable;
    }
}

const char *nds_overlay_destructor_chain_status_name(
    NdsOverlayDestructorChainStatus status) noexcept
{
    switch (status) {
    case NdsOverlayDestructorChainStatus::Ok: return "ok";
    case NdsOverlayDestructorChainStatus::InvalidCpu: return "invalid-cpu";
    case NdsOverlayDestructorChainStatus::InvalidMetadata: return "invalid-metadata";
    case NdsOverlayDestructorChainStatus::MemoryUnavailable: return "memory-unavailable";
    case NdsOverlayDestructorChainStatus::ChainCycle: return "chain-cycle";
    case NdsOverlayDestructorChainStatus::ChainCapacityExceeded: return "chain-capacity-exceeded";
    case NdsOverlayDestructorChainStatus::DuplicateNode: return "duplicate-node";
    case NdsOverlayDestructorChainStatus::AllocationFailed: return "allocation-failed";
    case NdsOverlayDestructorChainStatus::WriteFailed: return "write-failed";
    case NdsOverlayDestructorChainStatus::RollbackFailed: return "rollback-failed";
    }
    return "unknown";
}

} // namespace oracis::machine

import importlib.util
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
PATCHER_PATH = ROOT / "tools" / "ndsrecomp" / "patch_memory_access_direction.py"
SPEC = importlib.util.spec_from_file_location(
    "oracis_memory_direction_patcher", PATCHER_PATH
)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Cannot load memory direction patcher: {PATCHER_PATH}")
patcher = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(patcher)


class MemoryAccessDirectionPatchTests(unittest.TestCase):
    def test_load_and_store_are_tagged_from_the_emitted_bus_operation(self):
        source = """
void bank(void) {
    uint32_t value;
    runtime_mem_cycles(address, 4u, 0u);
    value = bus_read_u32(address);
    runtime_mem_cycles(address + 4u, 4u, 0u);
    bus_write_u32(address + 4u, value);
}
"""
        patched, reads, writes = patcher.patch_generated_source(source)

        self.assertEqual((reads, writes), (1, 1))
        self.assertIn(
            "runtime_mem_cycles_ex(address, 4u, 0u, "
            "ORACIS_MEMORY_ACCESS_READ)",
            patched,
        )
        self.assertIn(
            "runtime_mem_cycles_ex(address + 4u, 4u, 0u, "
            "ORACIS_MEMORY_ACCESS_WRITE)",
            patched,
        )
        self.assertNotIn("runtime_mem_cycles(", patched)

    def test_adjacent_word_timings_share_the_following_ldrd_or_strd_direction(self):
        source = """
void load_pair(void) {
    runtime_mem_cycles(address, 4u, 0u);
    runtime_mem_cycles(address + 4u, 4u, 1u);
    first = bus_read_u32(address);
    second = bus_read_u32(address + 4u);
}
void store_pair(void) {
    runtime_mem_cycles(address, 4u, 0u);
    runtime_mem_cycles(address + 4u, 4u, 1u);
    bus_write_u32(address, first);
    bus_write_u32(address + 4u, second);
}
"""
        patched, reads, writes = patcher.patch_generated_source(source)

        self.assertEqual((reads, writes), (2, 2))
        self.assertEqual(
            patched.count("ORACIS_MEMORY_ACCESS_READ)"), 2
        )
        self.assertEqual(
            patched.count("ORACIS_MEMORY_ACCESS_WRITE)"), 2
        )

    def test_swap_read_and_write_are_classified_independently(self):
        source = """
void swap_word(void) {
    runtime_mem_cycles(address, 4u, 0u);
    old_value = bus_read_u32(address);
    runtime_mem_cycles(address, 4u, 0u);
    bus_write_u32(address, new_value);
}
"""
        patched, reads, writes = patcher.patch_generated_source(source)

        self.assertEqual((reads, writes), (1, 1))
        self.assertLess(
            patched.index("ORACIS_MEMORY_ACCESS_READ"),
            patched.index("ORACIS_MEMORY_ACCESS_WRITE"),
        )

    def test_comments_and_string_literals_are_not_treated_as_bus_accesses(self):
        source = r'''
/* runtime_mem_cycles(fake, 4u, 0u); bus_read_u32(fake); */
const char *description = "bus_write_u32(fake, value)";
void bank(void) {
    runtime_mem_cycles(real_address, 2u, 0u);
    value = bus_read_u16(real_address);
}
'''
        patched, reads, writes = patcher.patch_generated_source(source)

        self.assertEqual((reads, writes), (1, 0))
        self.assertIn(
            "runtime_mem_cycles_ex(real_address, 2u, 0u, "
            "ORACIS_MEMORY_ACCESS_READ)",
            patched,
        )
        self.assertIn("runtime_mem_cycles(fake, 4u, 0u)", patched)

    def test_unmatched_timing_call_fails_closed(self):
        with self.assertRaisesRegex(ValueError, "no subsequent bus_read/write"):
            patcher.patch_generated_source(
                "void bank(void) { runtime_mem_cycles(address, 4u, 0u); }"
            )

    def test_already_adapted_source_is_idempotent(self):
        source = """
void bank(void) {
    runtime_mem_cycles_ex(address, 4u, 0u, ORACIS_MEMORY_ACCESS_READ);
    value = bus_read_u32(address);
}
"""
        patched, reads, writes = patcher.patch_generated_source(source)

        self.assertEqual((patched, reads, writes), (source, 0, 0))


    def test_timing_call_cannot_borrow_bus_access_from_the_next_function(self):
        source = """
void function_without_access(void) {
    runtime_mem_cycles(address, 4u, 0u);
}
void unrelated_reader(void) {
    value = bus_read_u32(address);
}
"""
        with self.assertRaisesRegex(ValueError, "no subsequent bus_read/write"):
            patcher.patch_generated_source(source)

    def test_nested_blocks_and_extern_c_wrapper_keep_function_scope(self):
        source = r'''
extern "C" {
void bank(void) {
    if (enabled) {
        runtime_mem_cycles(address, 4u, 0u);
        value = bus_read_u32(address);
    }
}
}
'''
        patched, reads, writes = patcher.patch_generated_source(source)

        self.assertEqual((reads, writes), (1, 0))
        self.assertIn(
            "runtime_mem_cycles_ex(address, 4u, 0u, "
            "ORACIS_MEMORY_ACCESS_READ)",
            patched,
        )

    def test_each_function_uses_its_own_bus_operation_sequence(self):
        source = """
void load_word(void) {
    runtime_mem_cycles(load_address, 4u, 0u);
    loaded = bus_read_u32(load_address);
}
void store_word(void) {
    runtime_mem_cycles(store_address, 4u, 0u);
    bus_write_u32(store_address, stored);
}
"""
        patched, reads, writes = patcher.patch_generated_source(source)

        self.assertEqual((reads, writes), (1, 1))
        self.assertEqual(patched.count("ORACIS_MEMORY_ACCESS_READ)"), 1)
        self.assertEqual(patched.count("ORACIS_MEMORY_ACCESS_WRITE)"), 1)


    def test_large_generated_function_is_rewritten_in_one_pass(self):
        instruction_count = 2500
        body = "".join(
            f"    runtime_mem_cycles(address_{index}, 4u, 0u);\\n"
            f"    value_{index} = bus_read_u32(address_{index});\\n"
            for index in range(instruction_count)
        )
        source = "void bank(void) {\\n" + body + "}\\n"

        patched, reads, writes = patcher.patch_generated_source(source)

        self.assertEqual((reads, writes), (instruction_count, 0))
        self.assertEqual(
            patched.count("runtime_mem_cycles_ex("), instruction_count
        )
        self.assertEqual(
            patched.count("ORACIS_MEMORY_ACCESS_READ)"), instruction_count
        )


if __name__ == "__main__":
    unittest.main()

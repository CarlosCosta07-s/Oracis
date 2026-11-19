#include "oracis/memory/guest_memory_abi.h"
#include "oracis/memory/nds_interrupt_control.hpp"
#include "oracis/memory/nds_keypad_control.hpp"
#include "oracis/recomp/runtime_arm.h"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_keypad_control_test: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    using oracis::memory::GuestMemory;
    using oracis::memory::NdsInterruptControl;
    using oracis::memory::NdsKeypadControl;
    using oracis::memory::Status;

    GuestMemory memory;
    NdsInterruptControl interrupts(memory);
    NdsKeypadControl keypad(memory, interrupts);

    constexpr uint16_t key_a = UINT16_C(1) << 0u;
    constexpr uint16_t key_b = UINT16_C(1) << 1u;
    constexpr uint16_t key_start = UINT16_C(1) << 3u;
    constexpr uint16_t select_a_b =
        static_cast<uint16_t>(key_a | key_b);
    constexpr uint32_t key_irq = NdsKeypadControl::KeypadIrqMask;

    if (keypad.read(
            NDS_ARM9, NdsKeypadControl::KeyInputAddress, 2u).value !=
            NdsKeypadControl::KeyMask ||
        keypad.read(
            NDS_ARM7, NdsKeypadControl::KeyInputAddress, 2u).value !=
            NdsKeypadControl::KeyMask) {
        return fail("KEYINPUT did not initialize with all ten buttons released");
    }

    if (keypad.read(
            NDS_ARM9, NdsKeypadControl::KeyControlAddress, 2u).value != 0u ||
        keypad.read(
            NDS_ARM7, NdsKeypadControl::KeyControlAddress, 2u).value != 0u) {
        return fail("independent KEYCNT registers did not initialize to zero");
    }

    if (keypad.read(
            NDS_ARM9, NdsKeypadControl::KeyInputAddress, 1u).status !=
            Status::InvalidWidth ||
        keypad.write(
            NDS_ARM9, NdsKeypadControl::KeyControlAddress, 4u, 0u) !=
            Status::InvalidWidth ||
        keypad.write(
            NDS_ARM9, NdsKeypadControl::KeyInputAddress, 2u, 0u) !=
            Status::PermissionDenied ||
        keypad.read(
            NDS_ARM9, NdsKeypadControl::KeyInputAddress + 4u, 2u).status !=
            Status::Unmapped ||
        keypad.read(
            static_cast<NdsCpu>(77),
            NdsKeypadControl::KeyInputAddress, 2u).status !=
            Status::InvalidCpu) {
        return fail("invalid KEYINPUT/KEYCNT access was accepted");
    }

    ArmCpuState arm9{};
    ArmCpuState arm7{};
    arm9.cpsr = ORACIS_ARM_MODE_SYSTEM;
    arm7.cpsr = ORACIS_ARM_MODE_SYSTEM;
    if (!oracis_arm_runtime_bind_cpus(&arm9, &arm7) ||
        !oracis_bus_bind_memory(&memory) ||
        !oracis_bus_bind_interrupt_control(&interrupts) ||
        !oracis_bus_bind_keypad_control(&keypad)) {
        return fail("keypad bus binding failed");
    }
    if (oracis_bus_bind_keypad_control(&keypad)) {
        return fail("duplicate keypad bus binding was accepted");
    }
    if (oracis_bus_unbind_memory(&memory) ||
        oracis_bus_unbind_interrupt_control(&interrupts)) {
        return fail("a backing device was unbound while keypad remained bound");
    }

    /* KEYINPUT is the same physical ten-button state as seen by both CPUs. */
    keypad.set_pressed_keys(static_cast<uint16_t>(key_a | key_start));
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9) ||
        bus_read_u16(NdsKeypadControl::KeyInputAddress) != 0x03F6u ||
        !oracis_arm_runtime_select_cpu(NDS_ARM7) ||
        bus_read_u16(NdsKeypadControl::KeyInputAddress) != 0x03F6u) {
        return fail("KEYINPUT active-low state was not shared across CPUs");
    }
    keypad.set_pressed_keys(0u);

    /* ARM9: OR mode requests IRQ when any selected key is pressed. */
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9))
        return fail("ARM9 selection failed for OR keypad test");
    bus_write_u32(NdsInterruptControl::ImeAddress, 1u);
    bus_write_u32(NdsInterruptControl::IeAddress, key_irq);
    bus_write_u16(
        NdsKeypadControl::KeyControlAddress,
        static_cast<uint16_t>(NdsKeypadControl::IrqEnableMask | select_a_b));

    if (bus_read_u16(NdsKeypadControl::KeyControlAddress) != 0x4003u) {
        return fail("ARM9 KEYCNT control bits did not read back correctly");
    }

    keypad.set_pressed_keys(key_b);
    if (bus_read_u32(NdsInterruptControl::IfAddress) != key_irq ||
        !interrupts.irq_asserted(NDS_ARM9)) {
        return fail("KEYCNT OR condition did not request ARM9 keypad IRQ");
    }

    /* A held matching combination does not relatch IRQ after IF acknowledgement. */
    bus_write_u32(NdsInterruptControl::IfAddress, key_irq);
    keypad.set_pressed_keys(key_b);
    if (bus_read_u32(NdsInterruptControl::IfAddress) != 0u) {
        return fail("held key combination incorrectly repeated keypad IRQ");
    }
    keypad.set_pressed_keys(0u);
    keypad.set_pressed_keys(key_b);
    if (bus_read_u32(NdsInterruptControl::IfAddress) != key_irq) {
        return fail("a fresh OR-condition keypress did not relatch keypad IRQ");
    }
    bus_write_u32(NdsInterruptControl::IfAddress, key_irq);

    /* Reserved KEYCNT bits are ignored on write. */
    bus_write_u16(NdsKeypadControl::KeyControlAddress, UINT16_C(0xFFFF));
    if (bus_read_u16(NdsKeypadControl::KeyControlAddress) !=
            NdsKeypadControl::KeyControlMask) {
        return fail("KEYCNT reserved bits were not masked");
    }
    keypad.set_pressed_keys(0u);
    bus_write_u16(NdsKeypadControl::KeyControlAddress, 0u);
    if (bus_read_u32(NdsInterruptControl::IfAddress) != 0u) {
        return fail("ARM9 keypad IF did not remain acknowledged after release");
    }

    /* ARM7 has its own KEYCNT and IF state; AND mode requires all selected keys. */
    if (!oracis_arm_runtime_select_cpu(NDS_ARM7))
        return fail("ARM7 selection failed for AND keypad test");
    bus_write_u32(NdsInterruptControl::ImeAddress, 1u);
    bus_write_u32(NdsInterruptControl::IeAddress, key_irq);
    bus_write_u16(
        NdsKeypadControl::KeyControlAddress,
        static_cast<uint16_t>(
            NdsKeypadControl::IrqEnableMask |
            NdsKeypadControl::IrqAndMask |
            select_a_b));

    if (bus_read_u16(NdsKeypadControl::KeyControlAddress) != 0xC003u ||
        keypad.key_control(NDS_ARM9) != 0u) {
        return fail("ARM7 and ARM9 KEYCNT state was not independent");
    }

    keypad.set_pressed_keys(key_a);
    if (bus_read_u32(NdsInterruptControl::IfAddress) != 0u) {
        return fail("KEYCNT AND condition fired with only one selected key");
    }

    keypad.set_pressed_keys(select_a_b);
    if (bus_read_u32(NdsInterruptControl::IfAddress) != key_irq ||
        !interrupts.irq_asserted(NDS_ARM7)) {
        return fail("KEYCNT AND condition did not request ARM7 keypad IRQ");
    }

    /* The physical key state remains common even though IRQ controls differ. */
    if (!oracis_arm_runtime_select_cpu(NDS_ARM9) ||
        bus_read_u16(NdsKeypadControl::KeyInputAddress) != 0x03FCu ||
        bus_read_u16(NdsKeypadControl::KeyControlAddress) != 0u ||
        bus_read_u32(NdsInterruptControl::IfAddress) != 0u) {
        return fail("shared KEYINPUT or isolated CPU register state failed");
    }

    if (!oracis_arm_runtime_select_cpu(NDS_ARM7))
        return fail("ARM7 final selection failed");
    bus_write_u32(NdsInterruptControl::IfAddress, key_irq);
    if (bus_read_u32(NdsInterruptControl::IfAddress) != 0u ||
        interrupts.irq_asserted(NDS_ARM7)) {
        return fail("ARM7 keypad IF acknowledgement failed");
    }

    if (!oracis_bus_unbind_keypad_control(&keypad) ||
        !oracis_bus_unbind_interrupt_control(&interrupts) ||
        !oracis_bus_unbind_memory(&memory)) {
        return fail("keypad bus teardown failed");
    }

    std::puts("NDS KEYINPUT, per-CPU KEYCNT, OR/AND IRQ and bus routing passed");
    return 0;
}

#include "oracis/recomp/runtime_arm.h"

#include <bit>
#include <cstdio>
#include <cstdlib>

namespace {

[[noreturn]] void abi_failure(const char *operation)
{
    std::fprintf(stderr,
        "[Oracis runtime ABI] %s failed (CPU=%u, PC=0x%08X, CPSR=0x%08X)\n",
        operation,
        static_cast<unsigned>(g_nds_active),
        oracis_active_cpu_state ? oracis_active_cpu_state->R[15] : 0u,
        oracis_active_cpu_state ? oracis_active_cpu_state->cpsr : 0u);
    std::abort();
}

ArmCpuState *g_arm9_state = nullptr;
ArmCpuState *g_arm7_state = nullptr;
void *g_power_context = nullptr;
OracisArmRuntimePowerHandler g_power_handler = nullptr;
void *g_irq_context = nullptr;
OracisArmRuntimeIrqHandler g_irq_handler = nullptr;
void *g_interrupt_wait_context = nullptr;
OracisArmRuntimeInterruptWaitHandler g_interrupt_wait_handler = nullptr;
void *g_coprocessor_context = nullptr;
OracisArmRuntimeCoprocessorHandler g_coprocessor_handler = nullptr;
void *g_bios_memory_context = nullptr;
OracisArmRuntimeBiosMemoryHandler g_bios_memory_handler = nullptr;
void *g_bios_crc16_context = nullptr;
OracisArmRuntimeBiosCrc16Handler g_bios_crc16_handler = nullptr;
void *g_bios_bit_unpack_context = nullptr;
OracisArmRuntimeBiosBitUnpackHandler g_bios_bit_unpack_handler = nullptr;
void *g_bios_lz77_context = nullptr;
OracisArmRuntimeBiosLz77Handler g_bios_lz77_handler = nullptr;
void *g_bios_rle_context = nullptr;
OracisArmRuntimeBiosRleHandler g_bios_rle_handler = nullptr;
void *g_bios_huffman_context = nullptr;
OracisArmRuntimeBiosHuffmanHandler g_bios_huffman_handler = nullptr;
void *g_bios_diff_context = nullptr;
OracisArmRuntimeBiosDiffHandler g_bios_diff_handler = nullptr;

} // namespace

extern "C" {
ArmCpuState *oracis_active_cpu_state = nullptr;
NdsCpu g_nds_active = NDS_ARM9;

int oracis_arm_runtime_bind_cpus(ArmCpuState *arm9, ArmCpuState *arm7)
{
    if (arm9 == nullptr || arm7 == nullptr) return 0;

    /* The generated-bank ABI is process-global: never replace live states. */
    if (g_arm9_state != nullptr || g_arm7_state != nullptr) {
        return g_arm9_state == arm9 && g_arm7_state == arm7 ? 1 : 0;
    }

    g_arm9_state = arm9;
    g_arm7_state = arm7;
    g_nds_active = NDS_ARM9;
    oracis_active_cpu_state = g_arm9_state;
    return 1;
}

int oracis_arm_runtime_unbind_cpus(ArmCpuState *arm9, ArmCpuState *arm7)
{
    if (arm9 == nullptr || arm7 == nullptr ||
        g_arm9_state != arm9 || g_arm7_state != arm7 ||
        (oracis_active_cpu_state != arm9 &&
         oracis_active_cpu_state != arm7)) {
        return 0;
    }

    g_arm9_state = nullptr;
    g_arm7_state = nullptr;
    oracis_active_cpu_state = nullptr;
    g_nds_active = NDS_ARM9;
    return 1;
}

int oracis_arm_runtime_bind_power_handler(
    void *context, OracisArmRuntimePowerHandler handler)
{
    if (context == nullptr || handler == nullptr) return 0;
    if (g_power_handler != nullptr || g_power_context != nullptr) {
        return g_power_context == context && g_power_handler == handler ? 1 : 0;
    }
    g_power_context = context;
    g_power_handler = handler;
    return 1;
}

int oracis_arm_runtime_unbind_power_handler(
    void *context, OracisArmRuntimePowerHandler handler)
{
    if (context == nullptr || handler == nullptr ||
        g_power_context != context || g_power_handler != handler) {
        return 0;
    }
    g_power_context = nullptr;
    g_power_handler = nullptr;
    return 1;
}

int oracis_arm_runtime_bind_irq_handler(
    void *context, OracisArmRuntimeIrqHandler handler)
{
    if (context == nullptr || handler == nullptr) return 0;
    if (g_irq_handler != nullptr || g_irq_context != nullptr) {
        return g_irq_context == context && g_irq_handler == handler ? 1 : 0;
    }
    g_irq_context = context;
    g_irq_handler = handler;
    return 1;
}

int oracis_arm_runtime_unbind_irq_handler(
    void *context, OracisArmRuntimeIrqHandler handler)
{
    if (context == nullptr || handler == nullptr ||
        g_irq_context != context || g_irq_handler != handler) {
        return 0;
    }
    g_irq_context = nullptr;
    g_irq_handler = nullptr;
    return 1;
}

int oracis_arm_runtime_request_irq(uint32_t return_pc)
{
    if (g_irq_handler == nullptr || g_irq_context == nullptr) return 0;
    if (oracis_active_cpu_state == nullptr ||
        (g_nds_active != NDS_ARM9 && g_nds_active != NDS_ARM7)) {
        return -1;
    }
    return g_irq_handler(g_irq_context, g_nds_active, return_pc);
}

int oracis_arm_runtime_bind_interrupt_wait_handler(
    void *context, OracisArmRuntimeInterruptWaitHandler handler)
{
    if (context == nullptr || handler == nullptr) return 0;
    if (g_interrupt_wait_handler != nullptr ||
        g_interrupt_wait_context != nullptr) {
        return g_interrupt_wait_context == context &&
            g_interrupt_wait_handler == handler ? 1 : 0;
    }
    g_interrupt_wait_context = context;
    g_interrupt_wait_handler = handler;
    return 1;
}

int oracis_arm_runtime_unbind_interrupt_wait_handler(
    void *context, OracisArmRuntimeInterruptWaitHandler handler)
{
    if (context == nullptr || handler == nullptr ||
        g_interrupt_wait_context != context ||
        g_interrupt_wait_handler != handler) {
        return 0;
    }
    g_interrupt_wait_context = nullptr;
    g_interrupt_wait_handler = nullptr;
    return 1;
}

int oracis_arm_runtime_request_interrupt_wait(
    uint32_t clear_old_flags, uint32_t wait_mask)
{
    if (g_interrupt_wait_handler == nullptr ||
        g_interrupt_wait_context == nullptr) {
        return 0;
    }
    if (oracis_active_cpu_state == nullptr ||
        (g_nds_active != NDS_ARM9 && g_nds_active != NDS_ARM7)) {
        return -1;
    }
    return g_interrupt_wait_handler(
        g_interrupt_wait_context, g_nds_active,
        ORACIS_ARM_RUNTIME_INTERRUPT_WAIT_BEGIN,
        clear_old_flags, wait_mask);
}

int oracis_arm_runtime_resume_interrupt_wait(uint32_t return_pc)
{
    if (g_interrupt_wait_handler == nullptr ||
        g_interrupt_wait_context == nullptr) {
        return 1;
    }
    if (oracis_active_cpu_state == nullptr ||
        (g_nds_active != NDS_ARM9 && g_nds_active != NDS_ARM7)) {
        return -1;
    }
    return g_interrupt_wait_handler(
        g_interrupt_wait_context, g_nds_active,
        ORACIS_ARM_RUNTIME_INTERRUPT_WAIT_RESUME,
        return_pc, 0u);
}


int oracis_arm_runtime_bind_coprocessor_handler(
    void *context, OracisArmRuntimeCoprocessorHandler handler)
{
    if (context == nullptr || handler == nullptr) return 0;
    if (g_coprocessor_handler != nullptr || g_coprocessor_context != nullptr) {
        return g_coprocessor_context == context &&
            g_coprocessor_handler == handler ? 1 : 0;
    }
    g_coprocessor_context = context;
    g_coprocessor_handler = handler;
    return 1;
}

int oracis_arm_runtime_unbind_coprocessor_handler(
    void *context, OracisArmRuntimeCoprocessorHandler handler)
{
    if (context == nullptr || handler == nullptr ||
        g_coprocessor_context != context ||
        g_coprocessor_handler != handler) {
        return 0;
    }
    g_coprocessor_context = nullptr;
    g_coprocessor_handler = nullptr;
    return 1;
}

int oracis_arm_runtime_bind_bios_memory_handler(
    void *context, OracisArmRuntimeBiosMemoryHandler handler)
{
    if (context == nullptr || handler == nullptr) return 0;
    if (g_bios_memory_handler != nullptr || g_bios_memory_context != nullptr) {
        return g_bios_memory_context == context &&
            g_bios_memory_handler == handler ? 1 : 0;
    }
    g_bios_memory_context = context;
    g_bios_memory_handler = handler;
    return 1;
}

int oracis_arm_runtime_unbind_bios_memory_handler(
    void *context, OracisArmRuntimeBiosMemoryHandler handler)
{
    if (context == nullptr || handler == nullptr ||
        g_bios_memory_context != context ||
        g_bios_memory_handler != handler) {
        return 0;
    }
    g_bios_memory_context = nullptr;
    g_bios_memory_handler = nullptr;
    return 1;
}

int oracis_arm_runtime_bios_memory_transfer(
    uint32_t source, uint32_t destination, uint32_t count,
    uint32_t width, uint32_t fixed_source)
{
    if (count == 0u) return 1;
    if ((width != 2u && width != 4u) || fixed_source > 1u ||
        g_bios_memory_handler == nullptr ||
        g_bios_memory_context == nullptr ||
        oracis_active_cpu_state == nullptr ||
        (g_nds_active != NDS_ARM9 && g_nds_active != NDS_ARM7)) {
        return 0;
    }
    return g_bios_memory_handler(
        g_bios_memory_context, g_nds_active,
        source, destination, count, width, fixed_source);
}

int oracis_arm_runtime_bind_bios_crc16_handler(
    void *context, OracisArmRuntimeBiosCrc16Handler handler)
{
    if (context == nullptr || handler == nullptr) return 0;
    if (g_bios_crc16_handler != nullptr || g_bios_crc16_context != nullptr) {
        return g_bios_crc16_context == context &&
            g_bios_crc16_handler == handler ? 1 : 0;
    }
    g_bios_crc16_context = context;
    g_bios_crc16_handler = handler;
    return 1;
}

int oracis_arm_runtime_unbind_bios_crc16_handler(
    void *context, OracisArmRuntimeBiosCrc16Handler handler)
{
    if (context == nullptr || handler == nullptr ||
        g_bios_crc16_context != context ||
        g_bios_crc16_handler != handler) {
        return 0;
    }
    g_bios_crc16_context = nullptr;
    g_bios_crc16_handler = nullptr;
    return 1;
}

int oracis_arm_runtime_bios_crc16(
    uint32_t initial_crc, uint32_t source, uint32_t length,
    uint32_t *result, uint32_t *last_halfword)
{
    if (result == nullptr || last_halfword == nullptr ||
        (source & 1u) != 0u || (length & 1u) != 0u) {
        return 0;
    }
    *result = initial_crc & UINT32_C(0xFFFF);
    *last_halfword = 0u;
    if (length == 0u) return 1;

    if (g_bios_crc16_handler == nullptr ||
        g_bios_crc16_context == nullptr ||
        oracis_active_cpu_state == nullptr ||
        (g_nds_active != NDS_ARM9 && g_nds_active != NDS_ARM7)) {
        return 0;
    }
    return g_bios_crc16_handler(
        g_bios_crc16_context, g_nds_active,
        initial_crc & UINT32_C(0xFFFF), source, length,
        result, last_halfword);
}

int oracis_arm_runtime_bind_bios_bit_unpack_handler(
    void *context, OracisArmRuntimeBiosBitUnpackHandler handler)
{
    if (context == nullptr || handler == nullptr) return 0;
    if (g_bios_bit_unpack_handler != nullptr ||
        g_bios_bit_unpack_context != nullptr) {
        return g_bios_bit_unpack_context == context &&
            g_bios_bit_unpack_handler == handler ? 1 : 0;
    }
    g_bios_bit_unpack_context = context;
    g_bios_bit_unpack_handler = handler;
    return 1;
}

int oracis_arm_runtime_unbind_bios_bit_unpack_handler(
    void *context, OracisArmRuntimeBiosBitUnpackHandler handler)
{
    if (context == nullptr || handler == nullptr ||
        g_bios_bit_unpack_context != context ||
        g_bios_bit_unpack_handler != handler) {
        return 0;
    }
    g_bios_bit_unpack_context = nullptr;
    g_bios_bit_unpack_handler = nullptr;
    return 1;
}

int oracis_arm_runtime_bios_bit_unpack(
    uint32_t source, uint32_t destination, uint32_t parameters)
{
    if (g_bios_bit_unpack_handler == nullptr ||
        g_bios_bit_unpack_context == nullptr ||
        oracis_active_cpu_state == nullptr ||
        (g_nds_active != NDS_ARM9 && g_nds_active != NDS_ARM7)) {
        return 0;
    }
    return g_bios_bit_unpack_handler(
        g_bios_bit_unpack_context, g_nds_active,
        source, destination, parameters);
}

int oracis_arm_runtime_bind_bios_lz77_handler(
    void *context, OracisArmRuntimeBiosLz77Handler handler)
{
    if (context == nullptr || handler == nullptr) return 0;
    if (g_bios_lz77_handler != nullptr || g_bios_lz77_context != nullptr) {
        return g_bios_lz77_context == context &&
            g_bios_lz77_handler == handler ? 1 : 0;
    }
    g_bios_lz77_context = context;
    g_bios_lz77_handler = handler;
    return 1;
}

int oracis_arm_runtime_unbind_bios_lz77_handler(
    void *context, OracisArmRuntimeBiosLz77Handler handler)
{
    if (context == nullptr || handler == nullptr ||
        g_bios_lz77_context != context ||
        g_bios_lz77_handler != handler) {
        return 0;
    }
    g_bios_lz77_context = nullptr;
    g_bios_lz77_handler = nullptr;
    return 1;
}

int oracis_arm_runtime_bios_lz77(
    uint32_t source, uint32_t destination, uint32_t write_halfwords)
{
    if (write_halfwords > 1u ||
        g_bios_lz77_handler == nullptr ||
        g_bios_lz77_context == nullptr ||
        oracis_active_cpu_state == nullptr ||
        (g_nds_active != NDS_ARM9 && g_nds_active != NDS_ARM7)) {
        return 0;
    }
    return g_bios_lz77_handler(
        g_bios_lz77_context, g_nds_active,
        source, destination, write_halfwords);
}

int oracis_arm_runtime_bind_bios_rle_handler(
    void *context, OracisArmRuntimeBiosRleHandler handler)
{
    if (context == nullptr || handler == nullptr) return 0;
    if (g_bios_rle_handler != nullptr || g_bios_rle_context != nullptr) {
        return g_bios_rle_context == context &&
            g_bios_rle_handler == handler ? 1 : 0;
    }
    g_bios_rle_context = context;
    g_bios_rle_handler = handler;
    return 1;
}

int oracis_arm_runtime_unbind_bios_rle_handler(
    void *context, OracisArmRuntimeBiosRleHandler handler)
{
    if (context == nullptr || handler == nullptr ||
        g_bios_rle_context != context ||
        g_bios_rle_handler != handler) {
        return 0;
    }
    g_bios_rle_context = nullptr;
    g_bios_rle_handler = nullptr;
    return 1;
}

int oracis_arm_runtime_bios_rle(
    uint32_t source, uint32_t destination, uint32_t write_halfwords)
{
    if (write_halfwords > 1u ||
        g_bios_rle_handler == nullptr ||
        g_bios_rle_context == nullptr ||
        oracis_active_cpu_state == nullptr ||
        (g_nds_active != NDS_ARM9 && g_nds_active != NDS_ARM7)) {
        return 0;
    }
    return g_bios_rle_handler(
        g_bios_rle_context, g_nds_active,
        source, destination, write_halfwords);
}

int oracis_arm_runtime_bind_bios_huffman_handler(
    void *context, OracisArmRuntimeBiosHuffmanHandler handler)
{
    if (context == nullptr || handler == nullptr) return 0;
    if (g_bios_huffman_handler != nullptr ||
        g_bios_huffman_context != nullptr) {
        return g_bios_huffman_context == context &&
            g_bios_huffman_handler == handler ? 1 : 0;
    }
    g_bios_huffman_context = context;
    g_bios_huffman_handler = handler;
    return 1;
}

int oracis_arm_runtime_unbind_bios_huffman_handler(
    void *context, OracisArmRuntimeBiosHuffmanHandler handler)
{
    if (context == nullptr || handler == nullptr ||
        g_bios_huffman_context != context ||
        g_bios_huffman_handler != handler) {
        return 0;
    }
    g_bios_huffman_context = nullptr;
    g_bios_huffman_handler = nullptr;
    return 1;
}

int oracis_arm_runtime_bios_huffman(
    uint32_t source, uint32_t destination, uint32_t callback_structure)
{
    if (g_bios_huffman_handler == nullptr ||
        g_bios_huffman_context == nullptr ||
        oracis_active_cpu_state == nullptr ||
        (g_nds_active != NDS_ARM9 && g_nds_active != NDS_ARM7)) {
        return 0;
    }
    return g_bios_huffman_handler(
        g_bios_huffman_context, g_nds_active,
        source, destination, callback_structure);
}

int oracis_arm_runtime_bind_bios_diff_handler(
    void *context, OracisArmRuntimeBiosDiffHandler handler)
{
    if (context == nullptr || handler == nullptr) return 0;
    if (g_bios_diff_handler != nullptr || g_bios_diff_context != nullptr) {
        return g_bios_diff_context == context &&
            g_bios_diff_handler == handler ? 1 : 0;
    }
    g_bios_diff_context = context;
    g_bios_diff_handler = handler;
    return 1;
}

int oracis_arm_runtime_unbind_bios_diff_handler(
    void *context, OracisArmRuntimeBiosDiffHandler handler)
{
    if (context == nullptr || handler == nullptr ||
        g_bios_diff_context != context ||
        g_bios_diff_handler != handler) {
        return 0;
    }
    g_bios_diff_context = nullptr;
    g_bios_diff_handler = nullptr;
    return 1;
}

int oracis_arm_runtime_bios_diff(
    uint32_t source, uint32_t destination, uint32_t unit_width)
{
    if ((unit_width != 1u && unit_width != 2u) ||
        g_bios_diff_handler == nullptr ||
        g_bios_diff_context == nullptr ||
        oracis_active_cpu_state == nullptr ||
        g_nds_active != NDS_ARM9) {
        return 0;
    }
    return g_bios_diff_handler(
        g_bios_diff_context, g_nds_active,
        source, destination, unit_width);
}

int oracis_arm_runtime_coprocessor_transfer(
    uint32_t is_write, uint32_t cp_num, uint32_t op1,
    uint32_t crn, uint32_t crm, uint32_t op2, uint32_t *value)
{
    if (is_write > 1u || value == nullptr ||
        g_coprocessor_handler == nullptr ||
        g_coprocessor_context == nullptr ||
        oracis_active_cpu_state == nullptr ||
        (g_nds_active != NDS_ARM9 && g_nds_active != NDS_ARM7)) {
        return 0;
    }
    return g_coprocessor_handler(
        g_coprocessor_context, g_nds_active, is_write,
        cp_num, op1, crn, crm, op2, value);
}

int oracis_arm_runtime_request_halt(NdsCpu cpu)
{
    if (g_power_handler == nullptr || g_power_context == nullptr ||
        oracis_active_cpu_state == nullptr || g_nds_active != cpu ||
        (cpu != NDS_ARM9 && cpu != NDS_ARM7)) {
        return 0;
    }
    return g_power_handler(
        g_power_context, cpu, ORACIS_ARM_RUNTIME_POWER_HALT, 0u);
}

int oracis_arm_runtime_write_arm7_halt_control(uint8_t value)
{
    if (g_power_handler == nullptr || g_power_context == nullptr ||
        oracis_active_cpu_state == nullptr || g_nds_active != NDS_ARM7) {
        return 0;
    }
    return g_power_handler(
        g_power_context, NDS_ARM7,
        ORACIS_ARM_RUNTIME_POWER_ARM7_HALTCNT, value);
}

int oracis_arm_runtime_write_arm9_post_flag(uint32_t value)
{
    if (g_power_handler == nullptr || g_power_context == nullptr ||
        oracis_active_cpu_state == nullptr || g_nds_active != NDS_ARM9) {
        return 0;
    }
    return g_power_handler(
        g_power_context, NDS_ARM9,
        ORACIS_ARM_RUNTIME_POWER_ARM9_POSTFLG, value);
}

int oracis_arm_runtime_select_cpu(NdsCpu cpu)
{
    if (g_arm9_state == nullptr || g_arm7_state == nullptr) return 0;

    switch (cpu) {
    case NDS_ARM9:
        g_nds_active = NDS_ARM9;
        oracis_active_cpu_state = g_arm9_state;
        return 1;
    case NDS_ARM7:
        g_nds_active = NDS_ARM7;
        oracis_active_cpu_state = g_arm7_state;
        return 1;
    default:
        return 0;
    }
}

ArmCpuState *oracis_arm_runtime_current_state(void)
{
    return oracis_active_cpu_state;
}

int arm_cond_passes(unsigned cond)
{
    if (oracis_active_cpu_state == nullptr) abi_failure("arm_cond_passes");
    return oracis_arm_condition_passes(g_cpu.cpsr, cond);
}

void arm_set_nz(uint32_t result)
{
    if (oracis_active_cpu_state == nullptr) abi_failure("arm_set_nz");
    oracis_arm_set_nz(&g_cpu, result);
}

void arm_set_nzc_logic(uint32_t result, uint32_t shifter_carry)
{
    if (oracis_active_cpu_state == nullptr) abi_failure("arm_set_nzc_logic");
    oracis_arm_set_nzc_logic(&g_cpu, result, shifter_carry);
}

void arm_set_nzcv_add(uint32_t a, uint32_t b, uint32_t result)
{
    if (oracis_active_cpu_state == nullptr) abi_failure("arm_set_nzcv_add");
    oracis_arm_set_nzcv_add(&g_cpu, a, b, result);
}

void arm_set_nzcv_adc(uint32_t a, uint32_t b, uint32_t carry_in,
                      uint32_t result)
{
    if (oracis_active_cpu_state == nullptr) abi_failure("arm_set_nzcv_adc");
    oracis_arm_set_nzcv_adc(&g_cpu, a, b, carry_in, result);
}

void arm_set_nzcv_sub(uint32_t a, uint32_t b, uint32_t result)
{
    if (oracis_active_cpu_state == nullptr) abi_failure("arm_set_nzcv_sub");
    oracis_arm_set_nzcv_sub(&g_cpu, a, b, result);
}

void arm_set_nzcv_sbc(uint32_t a, uint32_t b, uint32_t carry_in,
                      uint32_t result)
{
    if (oracis_active_cpu_state == nullptr) abi_failure("arm_set_nzcv_sbc");
    oracis_arm_set_nzcv_sbc(&g_cpu, a, b, carry_in, result);
}

uint32_t runtime_mrs_cpsr(void)
{
    if (oracis_active_cpu_state == nullptr) abi_failure("runtime_mrs_cpsr");
    return g_cpu.cpsr;
}

uint32_t runtime_mrs_spsr(void)
{
    if (oracis_active_cpu_state == nullptr) abi_failure("runtime_mrs_spsr");
    uint32_t value = 0u;
    if (!oracis_arm_cpu_read_spsr(&g_cpu, &value))
        abi_failure("runtime_mrs_spsr (current mode has no SPSR)");
    return value;
}

void runtime_msr_cpsr(uint32_t value, uint32_t field_mask)
{
    if (oracis_active_cpu_state == nullptr) abi_failure("runtime_msr_cpsr");
    if (!oracis_arm_cpu_write_cpsr(&g_cpu, value, field_mask))
        abi_failure("runtime_msr_cpsr (invalid mode or field mask)");
}

void runtime_msr_spsr(uint32_t value, uint32_t field_mask)
{
    if (oracis_active_cpu_state == nullptr) abi_failure("runtime_msr_spsr");
    if (!oracis_arm_cpu_write_spsr(&g_cpu, value, field_mask))
        abi_failure("runtime_msr_spsr (current mode has no SPSR or invalid mask)");
}

void runtime_exception_return(uint32_t new_pc)
{
    if (oracis_active_cpu_state == nullptr) abi_failure("runtime_exception_return");
    if (!oracis_arm_cpu_return_from_exception(&g_cpu, new_pc))
        abi_failure("runtime_exception_return (invalid SPSR or return PC)");
    if (oracis_arm_runtime_resume_interrupt_wait(new_pc) <= 0) {
        abi_failure("runtime_exception_return (interrupt wait resume failed)");
    }
}

void runtime_restore_cpsr_from_spsr(void)
{
    if (oracis_active_cpu_state == nullptr) abi_failure("runtime_restore_cpsr_from_spsr");
    if (!oracis_arm_cpu_restore_cpsr_from_spsr(&g_cpu))
        abi_failure("runtime_restore_cpsr_from_spsr (current mode has no SPSR)");
}

uint32_t runtime_read_user_reg(uint32_t reg)
{
    if (oracis_active_cpu_state == nullptr)
        abi_failure("runtime_read_user_reg");
    if (reg > 15u)
        abi_failure("runtime_read_user_reg (register index out of range)");

    const uint32_t mode = g_cpu.cpsr & ORACIS_ARM_MODE_MASK;
    uint32_t current_bank = 0u;
    if (!oracis_arm_cpu_mode_bank(mode, &current_bank))
        abi_failure("runtime_read_user_reg (invalid CPSR mode)");

    if (reg <= 7u || reg == 15u) return g_cpu.R[reg];

    if (reg >= 8u && reg <= 12u && mode == ORACIS_ARM_MODE_FIQ) {
        return g_cpu.r8_12_user[reg - 8u];
    }

    if (reg == 13u || reg == 14u) {
        if (current_bank == ARM_BANK_USER) return g_cpu.R[reg];
        return reg == 13u
            ? g_cpu.banked_sp[ARM_BANK_USER]
            : g_cpu.banked_lr[ARM_BANK_USER];
    }

    /* R8-R12 are shared in every mode except FIQ. */
    return g_cpu.R[reg];
}

void runtime_write_user_reg(uint32_t reg, uint32_t value)
{
    if (oracis_active_cpu_state == nullptr)
        abi_failure("runtime_write_user_reg");
    if (reg > 15u)
        abi_failure("runtime_write_user_reg (register index out of range)");

    const uint32_t mode = g_cpu.cpsr & ORACIS_ARM_MODE_MASK;
    uint32_t current_bank = 0u;
    if (!oracis_arm_cpu_mode_bank(mode, &current_bank))
        abi_failure("runtime_write_user_reg (invalid CPSR mode)");

    if (reg <= 7u || reg == 15u) {
        g_cpu.R[reg] = value;
        return;
    }

    if (reg >= 8u && reg <= 12u && mode == ORACIS_ARM_MODE_FIQ) {
        g_cpu.r8_12_user[reg - 8u] = value;
        return;
    }

    if (reg == 13u || reg == 14u) {
        if (current_bank == ARM_BANK_USER) {
            g_cpu.R[reg] = value;
        } else if (reg == 13u) {
            g_cpu.banked_sp[ARM_BANK_USER] = value;
        } else {
            g_cpu.banked_lr[ARM_BANK_USER] = value;
        }
        return;
    }

    /* R8-R12 are shared in every mode except FIQ. */
    g_cpu.R[reg] = value;
}

uint32_t runtime_clz(uint32_t value)
{
    return static_cast<uint32_t>(std::countl_zero(value));
}

namespace {

uint32_t nds_bios_integer_sqrt(uint32_t value) noexcept
{
    /*
     * Restoring base-4 integer square root. The result is exact for the
     * unsigned 32-bit SWI input and does not depend on host floating-point.
     */
    uint32_t result = 0u;
    uint32_t bit = UINT32_C(1) << 30u;
    while (bit > value) {
        bit >>= 2u;
    }

    while (bit != 0u) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1u) + bit;
        } else {
            result >>= 1u;
        }
        bit >>= 2u;
    }
    return result;
}

} // namespace

void runtime_irq(uint32_t return_address)
{
    if (oracis_active_cpu_state == nullptr ||
        (g_nds_active != NDS_ARM9 && g_nds_active != NDS_ARM7)) {
        abi_failure("runtime_irq (no active CPU)");
    }
    if (oracis_arm_runtime_request_irq(return_address) < 0) {
        abi_failure("runtime_irq (machine rejected architectural IRQ boundary)");
    }
}

void runtime_swi(uint32_t swi_imm)
{
    if (oracis_active_cpu_state == nullptr) {
        abi_failure("runtime_swi (no active CPU)");
    }

    /* ARM SWI immediates are 24 bits; NDS BIOS service numbers fit in 8. */
    switch (swi_imm & UINT32_C(0x00FFFFFF)) {
    case 0x09u: { /* NDS7/NDS9 signed division: r0/r1 -> r0/r1/r3. */
        const int64_t dividend = std::bit_cast<int32_t>(g_cpu.R[0]);
        const int64_t divisor = std::bit_cast<int32_t>(g_cpu.R[1]);
        if (divisor == 0) {
            /* The retail BIOS does not return from a zero-divisor call. */
            abi_failure("runtime_swi Div (zero divisor would not return)");
        }

        /*
         * Widen before dividing to avoid signed overflow for INT_MIN/-1.
         * The 32-bit quotient write wraps architecturally; r3 receives the
         * unsigned magnitude of the quotient.
         */
        const int64_t quotient = dividend / divisor;
        const int64_t remainder = dividend % divisor;
        const uint64_t magnitude = quotient < 0
            ? static_cast<uint64_t>(-quotient)
            : static_cast<uint64_t>(quotient);
        g_cpu.R[0] = static_cast<uint32_t>(quotient);
        g_cpu.R[1] = static_cast<uint32_t>(remainder);
        g_cpu.R[3] = static_cast<uint32_t>(magnitude);
        return;
    }

    case 0x0Bu: { /* NDS7/NDS9 CpuSet: halfword or word copy/fill. */
        const uint32_t control = g_cpu.R[2];
        const uint32_t width =
            (control & (UINT32_C(1) << 26u)) != 0u ? 4u : 2u;
        const uint32_t count = control & UINT32_C(0x001FFFFF);
        const uint32_t fixed_source =
            (control >> 24u) & UINT32_C(1);
        if (!oracis_arm_runtime_bios_memory_transfer(
                g_cpu.R[0], g_cpu.R[1], count, width, fixed_source)) {
            abi_failure("runtime_swi CpuSet (invalid or unsupported memory span)");
        }
        return;
    }

    case 0x0Cu: { /* NDS7/NDS9 CpuFastSet: 32-bit word copy/fill. */
        const uint32_t control = g_cpu.R[2];
        const uint32_t count = control & UINT32_C(0x001FFFFF);
        const uint32_t fixed_source =
            (control >> 24u) & UINT32_C(1);
        if (!oracis_arm_runtime_bios_memory_transfer(
                g_cpu.R[0], g_cpu.R[1], count, 4u, fixed_source)) {
            abi_failure("runtime_swi CpuFastSet (invalid or unsupported memory span)");
        }
        return;
    }

    case 0x0Du: /* NDS7/NDS9 unsigned integer square root. */
        g_cpu.R[0] = nds_bios_integer_sqrt(g_cpu.R[0]);
        return;

    case 0x0Eu: { /* NDS7/NDS9 GetCRC16: R0=seed, R1=address, R2=length. */
        uint32_t result = 0u;
        uint32_t last_halfword = g_cpu.R[3];
        const uint32_t length = g_cpu.R[2];
        if (!oracis_arm_runtime_bios_crc16(
                g_cpu.R[0], g_cpu.R[1], length,
                &result, &last_halfword)) {
            abi_failure("runtime_swi GetCRC16 (invalid or unsupported memory span)");
        }
        g_cpu.R[0] = result & UINT32_C(0xFFFF);
        if (length != 0u) {
            g_cpu.R[3] = last_halfword & UINT32_C(0xFFFF);
        }
        return;
    }

    case 0x0Fu: /* NDS7/NDS9 IsDebugger: Oracis models retail 4MB Main RAM. */
        g_cpu.R[0] = 0u;
        return;

    case 0x10u: { /* NDS7/NDS9 BitUnPack: R0=source, R1=destination, R2=info. */
        if (!oracis_arm_runtime_bios_bit_unpack(
                g_cpu.R[0], g_cpu.R[1], g_cpu.R[2])) {
            abi_failure("runtime_swi BitUnPack (invalid or unsupported memory span)");
        }
        return;
    }

    case 0x11u: /* NDS7/NDS9 LZ77UnCompWram: byte writes. */
    case 0x12u: { /* NDS7/NDS9 LZ77UnCompVram: halfword writes. */
        const uint32_t write_halfwords =
            (swi_imm & UINT32_C(0x00FFFFFF)) == 0x12u ? 1u : 0u;
        if (!oracis_arm_runtime_bios_lz77(
                g_cpu.R[0], g_cpu.R[1], write_halfwords)) {
            abi_failure(write_halfwords != 0u
                ? "runtime_swi LZ77UnCompVram (invalid or unsupported stream/span)"
                : "runtime_swi LZ77UnCompWram (invalid or unsupported stream/span)");
        }
        return;
    }

    case 0x13u: { /* NDS7/NDS9 HuffUnComp: packed symbols, 32-bit writes. */
        if (!oracis_arm_runtime_bios_huffman(
                g_cpu.R[0], g_cpu.R[1], g_cpu.R[3])) {
            abi_failure("runtime_swi HuffUnComp (invalid/unsupported stream or callback mode)");
        }
        return;
    }

    case 0x16u: /* NDS9 Diff8bitUnFilterWram: byte units. */
    case 0x18u: { /* NDS9 Diff16bitUnFilter: halfword units. */
        const uint32_t unit_width =
            (swi_imm & UINT32_C(0x00FFFFFF)) == 0x18u ? 2u : 1u;
        if (!oracis_arm_runtime_bios_diff(
                g_cpu.R[0], g_cpu.R[1], unit_width)) {
            abi_failure(unit_width == 2u
                ? "runtime_swi Diff16bitUnFilter (invalid/unsupported stream/span)"
                : "runtime_swi Diff8bitUnFilterWram (invalid/unsupported stream/span)");
        }
        return;
    }

    case 0x14u: /* NDS7/NDS9 RLUnCompWram: byte writes. */
    case 0x15u: { /* NDS7/NDS9 RLUnCompVram: halfword writes. */
        const uint32_t write_halfwords =
            (swi_imm & UINT32_C(0x00FFFFFF)) == 0x15u ? 1u : 0u;
        if (!oracis_arm_runtime_bios_rle(
                g_cpu.R[0], g_cpu.R[1], write_halfwords)) {
            abi_failure(write_halfwords != 0u
                ? "runtime_swi RLUnCompVram (invalid or unsupported stream/span)"
                : "runtime_swi RLUnCompWram (invalid or unsupported stream/span)");
        }
        return;
    }

    case 0x04u: /* NDS7/NDS9 IntrWait: R0=clear old flags, R1=IRQ mask. */
        if (oracis_arm_runtime_request_interrupt_wait(
                g_cpu.R[0], g_cpu.R[1]) <= 0) {
            abi_failure("runtime_swi IntrWait (machine rejected wait request)");
        }
        return;

    case 0x05u: /* NDS7/NDS9 VBlankIntrWait: clear and wait for IRQ_VBLANK. */
        if (oracis_arm_runtime_request_interrupt_wait(
                1u, UINT32_C(1)) <= 0) {
            abi_failure("runtime_swi VBlankIntrWait (machine rejected wait request)");
        }
        return;

    case 0x06u: /* NDS Halt: ARM7 HALTCNT or ARM9 CP15 WFI. */
        if (!oracis_arm_runtime_request_halt(g_nds_active)) {
            abi_failure("runtime_swi Halt (machine rejected power request)");
        }
        return;

    case 0x1Fu: /* NDS7 CustomHalt / NDS9 CustomPost. */
        if (g_nds_active == NDS_ARM7) {
            if (!oracis_arm_runtime_write_arm7_halt_control(
                    static_cast<uint8_t>(g_cpu.R[2]))) {
                abi_failure("runtime_swi CustomHalt (unsupported CPU/mode)");
            }
            return;
        }
        if (g_nds_active == NDS_ARM9) {
            if (!oracis_arm_runtime_write_arm9_post_flag(g_cpu.R[0])) {
                abi_failure("runtime_swi CustomPost (POSTFLG update failed)");
            }
            return;
        }
        abi_failure("runtime_swi CustomPost/CustomHalt (unsupported CPU)");

    case 0x07u: /* Sleep pauses additional hardware domains not modeled yet. */
        abi_failure("runtime_swi Sleep (power-domain suspension unsupported)");

    default:
        abi_failure("runtime_swi (unimplemented BIOS service)");
    }
}

uint32_t runtime_coproc_read(
    uint32_t cp_num, uint32_t op1, uint32_t crn,
    uint32_t crm, uint32_t op2)
{
    if (oracis_active_cpu_state == nullptr || g_nds_active != NDS_ARM9) {
        abi_failure("runtime_coproc_read (no active ARM9 CPU)");
    }

    uint32_t value = 0u;
    if (!oracis_arm_runtime_coprocessor_transfer(
            0u, cp_num, op1, crn, crm, op2, &value)) {
        abi_failure("runtime_coproc_read (unsupported coprocessor register)");
    }
    return value;
}

void runtime_coproc_write(
    uint32_t cp_num, uint32_t op1, uint32_t crn,
    uint32_t crm, uint32_t op2, uint32_t value)
{
    if (oracis_active_cpu_state == nullptr || g_nds_active != NDS_ARM9) {
        abi_failure("runtime_coproc_write (no active ARM9 CPU)");
    }

    /* ARM9 Wait-For-Interrupt: MCR p15,0,Rd,c7,c0,4 (or c7,c8,2). */
    const bool wait_for_interrupt = cp_num == 15u && op1 == 0u &&
        crn == 7u &&
        ((crm == 0u && op2 == 4u) || (crm == 8u && op2 == 2u));
    if (wait_for_interrupt) {
        if (!oracis_arm_runtime_request_halt(NDS_ARM9)) {
            abi_failure("runtime_coproc_write WFI (machine rejected power request)");
        }
        return;
    }

    uint32_t transfer_value = value;
    if (!oracis_arm_runtime_coprocessor_transfer(
            1u, cp_num, op1, crn, crm, op2, &transfer_value)) {
        abi_failure("runtime_coproc_write (unsupported coprocessor register/value)");
    }
}

void runtime_coproc_cdp(
    uint32_t cp_num, uint32_t op1, uint32_t crn,
    uint32_t crm, uint32_t op2)
{
    if (oracis_active_cpu_state == nullptr) {
        abi_failure("runtime_coproc_cdp (no active CPU)");
    }

    /*
     * The Nintendo DS ARM cores do not provide a general-purpose external
     * coprocessor. ARM946E-S CP14/CP15 expose register transfers through
     * MRC/MCR; CDP is not a supported CP15 operation. Until the machine
     * delivers architectural Undefined exceptions from this dispatch point,
     * fail closed instead of silently discarding the operation.
     */
    char operation[96] = {};
    const int written = std::snprintf(
        operation, sizeof(operation),
        "unsupported CDP p%u,op1=%u,crn=%u,crm=%u,op2=%u",
        static_cast<unsigned>(cp_num),
        static_cast<unsigned>(op1),
        static_cast<unsigned>(crn),
        static_cast<unsigned>(crm),
        static_cast<unsigned>(op2));
    if (written < 0 || static_cast<size_t>(written) >= sizeof(operation)) {
        abi_failure("runtime_coproc_cdp (diagnostic formatting failed)");
    }
    runtime_unimplemented_op(operation, g_cpu.R[15]);
}

void runtime_unimplemented_op(const char *op_name, uint32_t pc)
{
    std::fprintf(stderr,
        "[Oracis] unimplemented generated ARM operation: %s at 0x%08X (CPU=%u)\n",
        op_name ? op_name : "(unknown)",
        pc,
        static_cast<unsigned>(g_nds_active));
    std::abort();
}
} // extern "C"

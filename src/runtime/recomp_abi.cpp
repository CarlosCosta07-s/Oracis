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
void *g_coprocessor_context = nullptr;
OracisArmRuntimeCoprocessorHandler g_coprocessor_handler = nullptr;
void *g_bios_memory_context = nullptr;
OracisArmRuntimeBiosMemoryHandler g_bios_memory_handler = nullptr;

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

    case 0x06u: /* NDS Halt: ARM7 HALTCNT or ARM9 CP15 WFI. */
        if (!oracis_arm_runtime_request_halt(g_nds_active)) {
            abi_failure("runtime_swi Halt (machine rejected power request)");
        }
        return;

    case 0x1Fu: /* NDS7 CustomHalt writes the byte argument from R2 to HALTCNT. */
        if (g_nds_active != NDS_ARM7 ||
            !oracis_arm_runtime_write_arm7_halt_control(
                static_cast<uint8_t>(g_cpu.R[2]))) {
            abi_failure("runtime_swi CustomHalt (unsupported CPU/mode)");
        }
        return;

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

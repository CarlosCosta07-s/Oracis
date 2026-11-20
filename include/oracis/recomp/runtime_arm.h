/*
 * Oracis compatibility header for standard NDSRecomp-generated C banks.
 *
 * This is an Oracis-owned ABI surface. It does not include or copy the
 * NDSRecomp runtime. g_cpu is a view of the CPU selected by the Oracis
 * scheduler; the scheduler must select the core before invoking a bank.
 */
#ifndef ORACIS_RECOMP_RUNTIME_ARM_H
#define ORACIS_RECOMP_RUNTIME_ARM_H

#include <stdbool.h>
#include <stdint.h>

#include "arm_cpu_state.h"
#include "arm_cpu_state_ops.h"
#include "arm_flags.h"
#include "runtime_event_trace.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The two architectural states are owned by the Oracis machine/runtime. */
extern ArmCpuState *oracis_active_cpu_state;
#ifdef NDS_STATIC_CPU
#define g_nds_active ((NdsCpu)(NDS_STATIC_CPU))
#else
extern NdsCpu g_nds_active;
#endif

enum {
    ORACIS_ARM_RUNTIME_POWER_HALT = 0,
    ORACIS_ARM_RUNTIME_POWER_ARM7_HALTCNT = 1
};

typedef int (*OracisArmRuntimePowerHandler)(
    void *context, NdsCpu cpu, uint32_t operation, uint32_t value);

/* Dispatches implemented ARM9 CP15 transfers to the owning NdsMachine. */
typedef int (*OracisArmRuntimeCoprocessorHandler)(
    void *context, NdsCpu cpu, uint32_t is_write,
    uint32_t cp_num, uint32_t op1, uint32_t crn,
    uint32_t crm, uint32_t op2, uint32_t *value);

/* Checked backing-store service used by NDS CpuSet/CpuFastSet SWIs. */
typedef int (*OracisArmRuntimeBiosMemoryHandler)(
    void *context, NdsCpu cpu, uint32_t source, uint32_t destination,
    uint32_t count, uint32_t width, uint32_t fixed_source);

/* Read-only NDS BIOS GetCRC16 service; result and final halfword are out params. */
typedef int (*OracisArmRuntimeBiosCrc16Handler)(
    void *context, NdsCpu cpu, uint32_t initial_crc,
    uint32_t source, uint32_t length,
    uint32_t *result, uint32_t *last_halfword);

/* Checked NDS BIOS BitUnPack service (SWI 0x10). */
typedef int (*OracisArmRuntimeBiosBitUnpackHandler)(
    void *context, NdsCpu cpu, uint32_t source,
    uint32_t destination, uint32_t parameters);

/* Checked NDS BIOS LZ77UnCompWram/Vram services (SWI 0x11/0x12). */
typedef int (*OracisArmRuntimeBiosLz77Handler)(
    void *context, NdsCpu cpu, uint32_t source,
    uint32_t destination, uint32_t write_halfwords);

/* Checked NDS BIOS RLUnCompWram/Vram services (SWI 0x14/0x15). */
typedef int (*OracisArmRuntimeBiosRleHandler)(
    void *context, NdsCpu cpu, uint32_t source,
    uint32_t destination, uint32_t write_halfwords);

int oracis_arm_runtime_bind_cpus(ArmCpuState *arm9, ArmCpuState *arm7);
int oracis_arm_runtime_unbind_cpus(ArmCpuState *arm9, ArmCpuState *arm7);
int oracis_arm_runtime_bind_power_handler(
    void *context, OracisArmRuntimePowerHandler handler);
int oracis_arm_runtime_unbind_power_handler(
    void *context, OracisArmRuntimePowerHandler handler);
int oracis_arm_runtime_bind_coprocessor_handler(
    void *context, OracisArmRuntimeCoprocessorHandler handler);
int oracis_arm_runtime_unbind_coprocessor_handler(
    void *context, OracisArmRuntimeCoprocessorHandler handler);
int oracis_arm_runtime_coprocessor_transfer(
    uint32_t is_write, uint32_t cp_num, uint32_t op1,
    uint32_t crn, uint32_t crm, uint32_t op2, uint32_t *value);
int oracis_arm_runtime_bind_bios_memory_handler(
    void *context, OracisArmRuntimeBiosMemoryHandler handler);
int oracis_arm_runtime_unbind_bios_memory_handler(
    void *context, OracisArmRuntimeBiosMemoryHandler handler);
int oracis_arm_runtime_bios_memory_transfer(
    uint32_t source, uint32_t destination, uint32_t count,
    uint32_t width, uint32_t fixed_source);
int oracis_arm_runtime_bind_bios_crc16_handler(
    void *context, OracisArmRuntimeBiosCrc16Handler handler);
int oracis_arm_runtime_unbind_bios_crc16_handler(
    void *context, OracisArmRuntimeBiosCrc16Handler handler);
int oracis_arm_runtime_bios_crc16(
    uint32_t initial_crc, uint32_t source, uint32_t length,
    uint32_t *result, uint32_t *last_halfword);
int oracis_arm_runtime_bind_bios_bit_unpack_handler(
    void *context, OracisArmRuntimeBiosBitUnpackHandler handler);
int oracis_arm_runtime_unbind_bios_bit_unpack_handler(
    void *context, OracisArmRuntimeBiosBitUnpackHandler handler);
int oracis_arm_runtime_bios_bit_unpack(
    uint32_t source, uint32_t destination, uint32_t parameters);
int oracis_arm_runtime_bind_bios_lz77_handler(
    void *context, OracisArmRuntimeBiosLz77Handler handler);
int oracis_arm_runtime_unbind_bios_lz77_handler(
    void *context, OracisArmRuntimeBiosLz77Handler handler);
int oracis_arm_runtime_bios_lz77(
    uint32_t source, uint32_t destination, uint32_t write_halfwords);
int oracis_arm_runtime_bind_bios_rle_handler(
    void *context, OracisArmRuntimeBiosRleHandler handler);
int oracis_arm_runtime_unbind_bios_rle_handler(
    void *context, OracisArmRuntimeBiosRleHandler handler);
int oracis_arm_runtime_bios_rle(
    uint32_t source, uint32_t destination, uint32_t write_halfwords);
int oracis_arm_runtime_request_halt(NdsCpu cpu);
int oracis_arm_runtime_write_arm7_halt_control(uint8_t value);
int oracis_arm_runtime_select_cpu(NdsCpu cpu);
ArmCpuState *oracis_arm_runtime_current_state(void);

/*
 * Generated bank bodies directly read/write g_cpu.R[] and g_cpu.cpsr.
 * This macro redirects those accesses to the scheduler-selected CPU state.
 * No copied shadow register file is introduced.
 */
#define g_cpu (*oracis_active_cpu_state)

static inline uint32_t cpsr_n(void) { return (g_cpu.cpsr & CPSR_N_BIT) ? 1u : 0u; }
static inline uint32_t cpsr_z(void) { return (g_cpu.cpsr & CPSR_Z_BIT) ? 1u : 0u; }
static inline uint32_t cpsr_c(void) { return (g_cpu.cpsr & CPSR_C_BIT) ? 1u : 0u; }
static inline uint32_t cpsr_v(void) { return (g_cpu.cpsr & CPSR_V_BIT) ? 1u : 0u; }

static inline int arm_cond_passes_i(unsigned cond)
{
    const uint32_t n = cpsr_n();
    const uint32_t z = cpsr_z();
    const uint32_t c = cpsr_c();
    const uint32_t v = cpsr_v();
    switch (cond & 0xFu) {
    case 0x0: return z != 0u;
    case 0x1: return z == 0u;
    case 0x2: return c != 0u;
    case 0x3: return c == 0u;
    case 0x4: return n != 0u;
    case 0x5: return n == 0u;
    case 0x6: return v != 0u;
    case 0x7: return v == 0u;
    case 0x8: return (c != 0u) && (z == 0u);
    case 0x9: return (c == 0u) || (z != 0u);
    case 0xA: return n == v;
    case 0xB: return n != v;
    case 0xC: return (z == 0u) && (n == v);
    case 0xD: return (z != 0u) || (n != v);
    case 0xE: return 1;
    default: return 0;
    }
}

/* Implemented state/PSR/condition helpers. */
int arm_cond_passes(unsigned cond);
void arm_set_nz(uint32_t result);
void arm_set_nzc_logic(uint32_t result, uint32_t shifter_carry);
void arm_set_nzcv_add(uint32_t a, uint32_t b, uint32_t result);
void arm_set_nzcv_adc(uint32_t a, uint32_t b, uint32_t carry_in, uint32_t result);
void arm_set_nzcv_sub(uint32_t a, uint32_t b, uint32_t result);
void arm_set_nzcv_sbc(uint32_t a, uint32_t b, uint32_t carry_in, uint32_t result);

uint32_t runtime_mrs_cpsr(void);
uint32_t runtime_mrs_spsr(void);
void runtime_msr_cpsr(uint32_t value, uint32_t field_mask);
void runtime_msr_spsr(uint32_t value, uint32_t field_mask);
void runtime_exception_return(uint32_t new_pc);
void runtime_restore_cpsr_from_spsr(void);
void runtime_unimplemented_op(const char *op_name, uint32_t pc);

/*
 * Remaining standard-bank ABI declarations. They allow generated objects to
 * compile while the corresponding Oracis subsystems are implemented. Linking
 * a game bank is intentionally blocked until every referenced symbol has a
 * real implementation; no silent no-op stubs are provided here.
 */
typedef struct NdsStaticValidationRange {
    uint32_t addr;
    uint32_t size;
    const uint8_t *expected;
} NdsStaticValidationRange;

typedef struct NdsStaticValidation {
    uint32_t addr;
    uint32_t size;
    const uint8_t *expected;
    const NdsStaticValidationRange *dependencies;
    uint32_t dependency_count;
} NdsStaticValidation;

typedef struct NdsDispatchEntry {
    uint32_t addr;
    uint8_t thumb;
    void (*fn)(void);
    const NdsStaticValidation *validation;
} NdsDispatchEntry;

typedef struct NdsLinkSlot {
    void (*fn)(void);
    uint32_t key;
    uint32_t target_pc;
    const void *guard;
} NdsLinkSlot;

/*
 * The project targets 64-bit desktop and Android ARM64. These assertions
 * lock the generated dispatch-table ABI to the pointer width used by those
 * platforms; a future 32-bit target must define and test a separate layout.
 */
#define ORACIS_BANK_ABI_ASSERT(name, expression) typedef char name[(expression) ? 1 : -1]
ORACIS_BANK_ABI_ASSERT(oracis_validation_range_size, sizeof(NdsStaticValidationRange) == 16u);
ORACIS_BANK_ABI_ASSERT(oracis_validation_size, sizeof(NdsStaticValidation) == 32u);
ORACIS_BANK_ABI_ASSERT(oracis_validation_dependencies_offset, offsetof(NdsStaticValidation, dependencies) == 16u);
ORACIS_BANK_ABI_ASSERT(oracis_validation_dependency_count_offset, offsetof(NdsStaticValidation, dependency_count) == 24u);
ORACIS_BANK_ABI_ASSERT(oracis_dispatch_entry_size, sizeof(NdsDispatchEntry) == 24u);
ORACIS_BANK_ABI_ASSERT(oracis_dispatch_entry_fn_offset, offsetof(NdsDispatchEntry, fn) == 8u);
ORACIS_BANK_ABI_ASSERT(oracis_dispatch_entry_validation_offset, offsetof(NdsDispatchEntry, validation) == 16u);
ORACIS_BANK_ABI_ASSERT(oracis_link_slot_size, sizeof(NdsLinkSlot) == 24u);
ORACIS_BANK_ABI_ASSERT(oracis_link_slot_key_offset, offsetof(NdsLinkSlot, key) == 8u);
ORACIS_BANK_ABI_ASSERT(oracis_link_slot_guard_offset, offsetof(NdsLinkSlot, guard) == 16u);
#undef ORACIS_BANK_ABI_ASSERT

#define RUNTIME_TRACE_DISPATCH  1u
#define RUNTIME_TRACE_EXCHANGE  2u
#define RUNTIME_TRACE_SWI       3u
#define RUNTIME_TRACE_MEM_WRITE 4u
#define RUNTIME_TRACE_BRANCH    5u
#define RUNTIME_TRACE_IRQ       6u
#define RUNTIME_TRACE_CALL      7u
#define RUNTIME_TRACE_MEM_READ  8u
#define RUNTIME_TRACE_LIVE_TRANSFER 9u

#define NDS_LIVE_TRANSFER_B         1u
#define NDS_LIVE_TRANSFER_BL        2u
#define NDS_LIVE_TRANSFER_BX        3u
#define NDS_LIVE_TRANSFER_BLX_REG   4u
#define NDS_LIVE_TRANSFER_BLX_IMM   5u
#define NDS_LIVE_TRANSFER_PC_WRITE  6u
#define NDS_LIVE_TRANSFER_LDR_PC    7u
#define NDS_LIVE_TRANSFER_LDM_PC    8u

uint8_t bus_read_u8(uint32_t addr);
uint16_t bus_read_u16(uint32_t addr);
uint32_t bus_read_u32(uint32_t addr);
void bus_write_u8(uint32_t addr, uint8_t value);
void bus_write_u16(uint32_t addr, uint16_t value);
void bus_write_u32(uint32_t addr, uint32_t value);

void runtime_tick(uint32_t cycles);
bool runtime_should_yield(void);
bool runtime_unwinding(void);
bool runtime_slice_yield(void);
uint32_t runtime_code_cycles(uint32_t pc);
/* NDS/DATA bus timing; width=1/2/4, sequential=0/1. */
uint32_t runtime_mem_cycles(uint32_t addr, uint32_t width, uint32_t sequential);
/*
 * ARM7TDMI operand-dependent multiply wait. signed_variant=1 permits signed
 * sign-extension termination; extra is the generated 0/1/2 internal-cycle
 * term. ARM9 returns zero because its multiply cost is static in numI.
 */
uint32_t runtime_mul_cycles(uint32_t rs_value, uint32_t signed_variant, uint32_t extra);
uint32_t runtime_clz(uint32_t value);

/*
 * NDSRecomp-generated instruction prologues use these retired-instruction
 * counters and the optional slow hook. Counters are independent per CPU.
 * The hook remains disarmed until Oracis provides a concrete tracing client.
 */
extern uint64_t g_insn_count[2];
extern uint32_t g_insn_hook_armed;
void runtime_insn_slow(void);

/*
 * Static-bank timing ABI.
 *
 * The fast packed ARM9 code-fetch table is disabled. nds_code_numc always
 * calls the Oracis provider, which models a limited set of uncached ARM9
 * code-fetch regions. ARM9/ARM7 branch refill providers also cover documented
 * memory classes. ARM7 instruction/data-cycle combination and ARM7TDMI
 * multiply are implemented for documented regions; cache and dynamic device
 * wait-state timing remain fail-closed.
 */
#define NDS_ARM9_CODE_K(pc, thumb) (UINT32_C(0))

/*
 * Publish the active ARM9 ITCM instruction window to the code-timing helper.
 * Zero means ITCM is disabled. Only NdsMachine's CP15/TCM controller updates
 * this process-wide value.
 */
void oracis_runtime_set_arm9_tcm_timing_state(
    uint32_t itcm_window_size,
    uint32_t dtcm_base,
    uint32_t dtcm_window_size,
    uint32_t itcm_load_mode,
    uint32_t dtcm_load_mode);

static inline uint32_t nds_code_numc(uint32_t pc, uint32_t packed_cost)
{
    (void)packed_cost;
    return runtime_code_cycles(pc);
}

/*
 * Branch/PC-write pipeline refill. target bit 0 marks Thumb. The helpers
 * account for sequential destination fetches (including the shared 32-bit
 * fetch for an aligned ARM9 Thumb pair); unsupported target regions fail closed.
 */
uint32_t arm7_refill_cycles(uint32_t target);
uint32_t arm9_refill_cycles(uint32_t target);

/*
 * ARM9 instruction-cycle combination is a pure overlap calculation:
 * memory cycles overlap up to six fetch cycles; non-memory internal cycles
 * are added to the instruction's own fetch cost.
 */
static inline uint32_t arm9_cycle_combine(
    uint32_t num_code, uint32_t num_data,
    uint32_t num_internal, uint32_t has_data)
{
    if (has_data != 0u) {
        const uint32_t floor_cost =
            num_code > num_data ? num_code : num_data;
        const uint32_t combined = num_code + num_data;
        const uint32_t overlap = combined > 6u ? combined - 6u : 0u;
        return overlap > floor_cost ? overlap : floor_cost;
    }
    return num_internal != 0u
        ? num_code + num_internal
        : num_code;
}

/*
 * ARM7 retirement timing. flat_cycles contains the generated 1S base and
 * num_data is the sum of runtime_mem_cycles calls for this instruction.
 * runtime_code_cycles publishes the instruction PC/Thumb state and each
 * runtime_mem_cycles call publishes the last data address before this call.
 */
uint32_t arm7_cycle_combine(
    uint32_t flat_cycles, uint32_t num_data,
    uint32_t is_load, uint32_t has_internal);

void runtime_dispatch(uint32_t target_pc);
void runtime_dispatch_literal_branch(uint32_t target_pc);
void runtime_dispatch_literal_call(uint32_t target_pc);
void runtime_dispatch_literal_fallthrough(uint32_t target_pc);
void runtime_dispatch_with_exchange(uint32_t target_pc);
void runtime_dispatch_miss(uint32_t target_pc);
void runtime_dispatch_bad_entry(uint32_t target_pc);
void runtime_link_call(NdsLinkSlot *slot);
void runtime_link_branch(NdsLinkSlot *slot);
void runtime_link_fallthrough(NdsLinkSlot *slot);
int runtime_inline_leaf_admit(const NdsLinkSlot *slot, void (*expected)(void));
void runtime_call_push_return(uint32_t return_pc);
int runtime_call_should_return(uint32_t target_pc);
void runtime_call_cancel_return(uint32_t return_pc);
uint32_t runtime_call_stack_depth(void);
const uint32_t *runtime_call_stack_data(void);
void runtime_call_stack_restore(const uint32_t *entries, uint32_t depth);
void runtime_live_transfer(uint32_t source_pc, uint32_t target_pc, uint32_t transfer_type);
void runtime_insn_slow(void);
void runtime_trace_event(uint32_t kind, uint32_t pc, uint32_t addr,
                         uint32_t value, uint32_t aux);
void runtime_swi(uint32_t swi_imm);
void runtime_irq(uint32_t return_address);
uint32_t runtime_read_user_reg(uint32_t reg);
void runtime_write_user_reg(uint32_t reg, uint32_t value);
void runtime_coproc_write(uint32_t cp_num, uint32_t op1, uint32_t crn,
                          uint32_t crm, uint32_t op2, uint32_t value);
uint32_t runtime_coproc_read(uint32_t cp_num, uint32_t op1, uint32_t crn,
                             uint32_t crm, uint32_t op2);
void runtime_coproc_cdp(uint32_t cp_num, uint32_t op1, uint32_t crn,
                        uint32_t crm, uint32_t op2);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ORACIS_RECOMP_RUNTIME_ARM_H */

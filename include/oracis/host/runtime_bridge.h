/*
 * C bridge between the SDL host and the C++ NdsMachine runtime.
 *
 * The host owns the lifecycle calls. Guest-memory, device and CPU state remain
 * owned by the single NdsMachine instance in the bridge implementation.
 */
#ifndef ORACIS_HOST_RUNTIME_BRIDGE_H
#define ORACIS_HOST_RUNTIME_BRIDGE_H

#include <stdint.h>

#include "oracis/recomp/arm_cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum OracisHostRuntimeStatus {
    ORACIS_HOST_RUNTIME_OK = 0,
    ORACIS_HOST_RUNTIME_NOT_INITIALIZED = 1,
    ORACIS_HOST_RUNTIME_ALREADY_INITIALIZED = 2,
    ORACIS_HOST_RUNTIME_NO_PROGRAM_BANKS = 3,
    ORACIS_HOST_RUNTIME_MACHINE_ERROR = 4,
    ORACIS_HOST_RUNTIME_NO_PROGRAM_LOADED = 5
} OracisHostRuntimeStatus;

/* Initializes the process-wide NDS machine and binds its base devices. */
int oracis_host_runtime_initialize(void);

/* Unbinds the machine and releases its process-wide runtime bindings. */
int oracis_host_runtime_shutdown(void);

/*
 * Host-provided architectural context for a pending IRQ. The callback must
 * compute the exact exception LR and active vector base for this CPU boundary.
 */
typedef int (*OracisHostIrqContextProvider)(
    void *context, NdsCpu cpu, const ArmCpuState *state,
    uint32_t *return_address, uint32_t *vector_base);

/*
 * Host-provided, already-extracted program segment. entry_pc is aligned and
 * has the Thumb marker removed; entry_thumb carries the execution mode.
 */
typedef struct OracisHostProgramSegment {
    uint32_t load_address;
    uint32_t size;
    uint32_t entry_pc;
    uint32_t entry_thumb;
    const uint8_t *contents;
} OracisHostProgramSegment;

/*
 * Preflight and load one ARM9 and one ARM7 image into the retail RAM map.
 * This loads bytes and seeds PC/CPSR.T; it does not emulate BIOS boot setup.
 */
int oracis_host_runtime_load_program_segments(
    const OracisHostProgramSegment *arm9,
    const OracisHostProgramSegment *arm7);
int oracis_host_runtime_program_loaded(void);

/*
 * Execute one synchronized ARM9/ARM7 round through the registered static
 * banks. Returns NO_PROGRAM_BANKS until both CPUs have at least one bank.
 */
int oracis_host_runtime_run_round(
    uint64_t bus_cycle_budget,
    OracisHostIrqContextProvider irq_context,
    void *context);

/* Read-only host diagnostics. */
int oracis_host_runtime_is_initialized(void);
int oracis_host_runtime_has_program_banks(void);
uint64_t oracis_host_runtime_global_bus_cycles(void);

/* Raw NdsMachineStatus from the most recent machine operation. */
int oracis_host_runtime_last_machine_status(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ORACIS_HOST_RUNTIME_BRIDGE_H */

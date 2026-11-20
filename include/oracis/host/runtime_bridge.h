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
    ORACIS_HOST_RUNTIME_NO_PROGRAM_LOADED = 5,
    ORACIS_HOST_RUNTIME_NO_COMPILED_BANKS = 6,
    ORACIS_HOST_RUNTIME_BANK_REGISTRATION_FAILED = 7,
    ORACIS_HOST_RUNTIME_BANKS_ALREADY_REGISTERED = 8,
    ORACIS_HOST_RUNTIME_LOCAL_IMAGE_MISSING = 9,
    ORACIS_HOST_RUNTIME_LOCAL_IMAGE_IO_ERROR = 10,
    ORACIS_HOST_RUNTIME_LOCAL_IMAGE_INVALID = 11,
    ORACIS_HOST_RUNTIME_INVALID_ARGUMENT = 12,
    ORACIS_HOST_RUNTIME_VIDEO_ERROR = 13,
    ORACIS_HOST_RUNTIME_ENTRYPOINT_UNRESOLVED = 14
} OracisHostRuntimeStatus;

/* Result of checking the current ARM9/ARM7 start PCs against registered banks. */
typedef struct OracisHostRuntimeEntryPointReport {
    uint32_t pc;
    uint32_t cpsr;
    uint32_t lookup_status;
    uint32_t candidate_count;
    uint32_t has_inactive_candidate;
    uint32_t resolved;
} OracisHostRuntimeEntryPointReport;

/*
 * Scanline composition diagnostics. status is an Nds2dComposeStatus value;
 * subordinate fields preserve the first failing BG/OBJ/memory operation.
 */
typedef struct OracisHostVideoScanlineReport {
    uint32_t status;
    uint32_t background_index;
    uint32_t fetch_status;
    uint32_t memory_status;
    uint32_t object_fetch_status;
    uint32_t object_index;
} OracisHostVideoScanlineReport;

/* Initializes the process-wide NDS machine and binds its base devices. */
int oracis_host_runtime_initialize(void);

/* Unbinds the machine and releases its process-wide runtime bindings. */
int oracis_host_runtime_shutdown(void);

/*
 * Host-provided architectural context for a pending IRQ. The callback must
 * compute the exact exception LR for this CPU boundary. The vector_base output
 * remains for ABI compatibility; NdsMachine selects vectors from CP15/platform.
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
 * Compose one visible scanline from the live NDS display/VRAM state.
 * engine_index 0 is Main Engine A (upper LCD), 1 is Engine B (lower LCD).
 * rgb555 receives exactly 256 pixels. Composition is fail-closed: on error
 * the output buffer remains unchanged and report contains device diagnostics.
 */
int oracis_host_runtime_render_scanline(
    uint32_t engine_index,
    uint32_t scanline,
    uint16_t *rgb555,
    uint32_t pixel_capacity,
    OracisHostVideoScanlineReport *report);

/*
 * Preflight both loaded start PCs using the live-byte validation callback.
 * A successful call may still report resolved=0 for either CPU; lookup_status
 * is an OracisDispatchLookupStatus value and no guest code is executed.
 */
int oracis_host_runtime_validate_entrypoints(
    OracisHostRuntimeEntryPointReport *arm9,
    OracisHostRuntimeEntryPointReport *arm7);

/*
 * Load the locally extracted Platinum USA Rev 1 ARM9/ARM7 images.
 * directory may be NULL to use .oracis/local/pokemon-platinum-usa-rev1.
 * Both files are read and checked against fixed sizes/SHA-1 values before
 * any guest memory is modified. This does not perform BIOS boot setup.
 */
int oracis_host_runtime_load_local_platinum_segments(const char *directory);

/*
 * Register the locally linked Platinum ARM9/ARM7 dispatch tables. Returns
 * NO_COMPILED_BANKS when the executable was built without ORACIS_LOCAL_BANK_DIR.
 */
int oracis_host_runtime_register_compiled_banks(void);

/*
 * Execute one synchronized ARM9/ARM7 round through the registered static
 * banks. Both loaded start PCs must resolve to live-byte-validated rows before
 * execution begins; otherwise ENTRYPOINT_UNRESOLVED is returned without
 * advancing either CPU or the shared bus clock. Later dynamic dispatch misses
 * remain fail-closed runtime errors.
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

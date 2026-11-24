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
    ORACIS_HOST_RUNTIME_GAME_CARD_ERROR = 14,
    ORACIS_HOST_RUNTIME_ENTRYPOINT_UNRESOLVED = 15,
    ORACIS_HOST_RUNTIME_OVERLAY_ALREADY_LOADED = 16,
    ORACIS_HOST_RUNTIME_OVERLAY_NOT_LOADED = 17,
    ORACIS_HOST_RUNTIME_OVERLAY_INVALID = 18,
    ORACIS_HOST_RUNTIME_OVERLAY_HASH_MISMATCH = 19,
    ORACIS_HOST_RUNTIME_OVERLAY_MEMORY_UNAVAILABLE = 20,
    ORACIS_HOST_RUNTIME_OVERLAY_OVERLAP = 21,
    ORACIS_HOST_RUNTIME_OVERLAY_BUSY = 22,
    ORACIS_HOST_RUNTIME_OVERLAY_WRITE_FAILED = 23,
    ORACIS_HOST_RUNTIME_OVERLAY_IDENTITY_CONFLICT = 24,
    ORACIS_HOST_RUNTIME_OVERLAY_INITIALIZER_DISPATCH_MISSING = 25,
    ORACIS_HOST_RUNTIME_OVERLAY_INITIALIZER_BUDGET_EXCEEDED = 26,
    ORACIS_HOST_RUNTIME_OVERLAY_INITIALIZER_EXECUTION_FAILED = 27,
    ORACIS_HOST_RUNTIME_OVERLAY_INITIALIZER_INVALID_STACK = 28,
    ORACIS_HOST_RUNTIME_OVERLAY_INITIALIZERS_ALREADY_RUN = 29,
    ORACIS_HOST_RUNTIME_OVERLAY_INITIALIZER_PENDING = 30,
    ORACIS_HOST_RUNTIME_OVERLAY_REGION_CAPACITY_EXCEEDED = 31,
    ORACIS_HOST_RUNTIME_OVERLAY_FINALIZERS_UNAVAILABLE = 32,
    ORACIS_HOST_RUNTIME_FIRMWARE_ERROR = 33,
    ORACIS_HOST_RUNTIME_FIRMWARE_MEMORY_UNAVAILABLE = 34,
    ORACIS_HOST_RUNTIME_BIOS_BOOT_UNAVAILABLE = 35,
    ORACIS_HOST_RUNTIME_BIOS_BOOT_NOT_ACTIVE = 36,
    ORACIS_HOST_RUNTIME_CPU_RESET_HELD = 37,
    ORACIS_HOST_RUNTIME_CPU_NOT_IN_RESET = 38
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

/* Sizes, destinations and CRC results for the retained user firmware IPL. */
typedef struct OracisHostFirmwareBootReport {
    uint32_t arm9_ipl1_load_address;
    uint32_t arm9_ipl1_bytes;
    uint32_t arm7_ipl2_load_address;
    uint32_t arm7_ipl2_bytes;
    uint32_t arm9_ipl3_staged_bytes;
    uint32_t arm7_ipl4_staged_bytes;
    uint32_t ipl5_language_staged_bytes;
    uint32_t expected_ipl12_crc16;
    uint32_t computed_ipl12_crc16;
    uint32_t expected_ipl34_crc16;
    uint32_t computed_ipl34_crc16;
    uint32_t expected_ipl5_crc16;
    uint32_t computed_ipl5_crc16;
    /* NdsFirmwareRevisionHint numeric value; 0 means unknown/modified. */
    uint32_t firmware_revision_hint;
} OracisHostFirmwareBootReport;

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
    uint32_t three_d_raster_status;
} OracisHostVideoScanlineReport;

/* Initializes the process-wide NDS machine and binds its base devices. */
int oracis_host_runtime_initialize(void);

/*
 * Stops both CPU slices, unbinds the machine and clears all static dispatch
 * registrations (including caller-owned C tables with non-owning pointers).
 */
int oracis_host_runtime_shutdown(void);

/*
 * Copy a local raw NDS image (at least the 0x200-byte NDS header) into the
 * Game Card device. The caller may release its input buffer after this call.
 * Images are limited to 512 MiB, the DS Game Card ROM address-space maximum.
 * The Game Card path supports raw commands plus KEY1/KEY2 secure reads B7/B8.
 */
int oracis_host_runtime_mount_game_card_image(
    const uint8_t *bytes, uint32_t size);
/* Load a user-owned 4 KiB NDS ARM9 BIOS into its CPU-local ROM window. */
int oracis_host_runtime_load_arm9_bios(
    const uint8_t *bytes, uint32_t size);
/* Load a user-owned 16 KiB NDS ARM7 BIOS and initialize its KEY1 table. */
int oracis_host_runtime_load_game_card_arm7_bios(
    const uint8_t *bytes, uint32_t size);
/* Copy a user-supplied raw NDS SPI firmware-flash dump (1..16 MiB). */
int oracis_host_runtime_set_spi_firmware_image(
    const uint8_t *bytes, uint32_t size);
/* Decrypt/validate IPL1/2 and P3/P4/P5 with the supplied ARM7 BIOS KEY1 table. */
int oracis_host_runtime_load_spi_firmware_boot_code(void);
/* Read retained firmware component sizes, IPL destinations and CRC checks. */
int oracis_host_runtime_get_firmware_boot_report(
    OracisHostFirmwareBootReport *report);
int oracis_host_runtime_unmount_game_card_image(void);
/* Supply the physical 32-bit cartridge chip ID; the raw .nds dump does not contain it. */
int oracis_host_runtime_set_game_card_chip_id(uint32_t chip_id);
/* Copy/decrypt the 0x800-byte Secure Area prefix; identifier_valid is 1 only for encryObj. */
int oracis_host_runtime_decrypt_game_card_secure_area_prefix(
    uint8_t *output, uint32_t output_size, uint32_t *identifier_valid);

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
/*
 * Validate two raw segments against the mounted NDS header and image, decrypt
 * the ARM9 0x800-byte Secure Area prefix using the user-supplied ARM7 BIOS
 * KEY1 table, then load both segments. This seeds PC/CPSR.T but does not
 * reproduce BIOS boot setup or guarantee that the game can execute.
 */
int oracis_host_runtime_load_game_card_program_segments(
    const OracisHostProgramSegment *arm9,
    const OracisHostProgramSegment *arm7);
/* Parse the two program segments directly from the mounted NDS header and
 * load them through the validated Game Card/Secure Area path. */
int oracis_host_runtime_load_mounted_game_card_program(void);
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
 * Preflight both user BIOS reset vectors against native static banks, then
 * initialize ARM9 at 0xFFFF0000 while holding ARM7 in reset at 0x00000000.
 * No CPU cycles are consumed unless both reset-vector entries resolve.
 */
int oracis_host_runtime_start_retail_bios_boot(
    OracisHostRuntimeEntryPointReport *arm9,
    OracisHostRuntimeEntryPointReport *arm7);
/*
 * Release ARM7 only after the caller has verified the hardware hand-off.
 * This explicit API is not connected to an assumed IPCSYNC value/edge.
 */
int oracis_host_runtime_release_arm7_from_reset(void);
/*
 * Advance a retail-BIOS-mode scheduler round. ARM7's time advances while
 * ResetHeld but its generated code is not called until explicit release.
 */
int oracis_host_runtime_run_retail_bios_round(
    uint64_t bus_cycle_budget,
    OracisHostIrqContextProvider irq_context,
    void *context);

/*
 * Load the locally extracted Platinum USA Rev 1 ARM9/ARM7 images.
 * directory may be NULL to use .oracis/local/pokemon-platinum-usa-rev1.
 * Both files are read and checked against fixed sizes/SHA-1 values before
 * any guest memory is modified. This does not perform BIOS boot setup.
 */
int oracis_host_runtime_load_local_platinum_segments(const char *directory);

/*
 * One locally extracted, already-decompressed overlay image. The caller
 * obtains these fields from the Oracis overlays/manifest.json and supplies
 * the matching image file. The runtime verifies image_sha1 before writing.
 */
typedef struct OracisHostOverlayImage {
    uint32_t cpu;
    uint32_t overlay_id;
    uint32_t ram_address;
    uint32_t ram_size;
    uint32_t bss_size;
    uint32_t static_init_start;
    uint32_t static_init_end;
    const uint8_t *contents;
    const char *image_sha1;
} OracisHostOverlayImage;

/* Load one overlay after the two main program segments have been loaded. */
int oracis_host_runtime_load_overlay(
    const OracisHostOverlayImage *overlay);
/*
 * Execute the overlay's validated static-init function pointers in table
 * order. Each constructor receives an isolated bounded guest call; guest
 * RAM/MMIO side effects persist, while CPU registers and guest return stack
 * are restored. The caller must have established a valid guest stack.
 */
int oracis_host_runtime_run_overlay_static_initializers(
    uint32_t cpu, uint32_t overlay_id,
    uint64_t cycle_budget_per_initializer);
/*
 * Request constructors from inside a guest/HLE callback. When a CPU slice is
 * active, returns OVERLAY_INITIALIZER_PENDING after requesting a cooperative
 * unwind; execution occurs at the next host-owned slice boundary.
 */
int oracis_host_runtime_request_overlay_static_initializers(
    uint32_t cpu, uint32_t overlay_id,
    uint64_t cycle_budget_per_initializer);
/*
 * Unload only at a safe overlay-manager boundary. no_live_guest_references
 * must be 1 only after the host has established that no saved guest return
 * address or other live reference points into this module.
 */
int oracis_host_runtime_unload_overlay(
    uint32_t cpu, uint32_t overlay_id,
    uint32_t no_live_guest_references);

/*
 * Register the locally linked Platinum ARM9/ARM7 dispatch tables. Returns
 * NO_COMPILED_BANKS when the executable was built without ORACIS_LOCAL_BANK_DIR.
 */
int oracis_host_runtime_register_compiled_banks(void);

/*
 * Execute one synchronized ARM9/ARM7 round through the registered static
 * banks. Before entering either CPU, validates both current entry PCs against
 * their registered dispatch rows and live guest bytes. Returns
 * NO_PROGRAM_BANKS until both CPUs have a bank, NO_PROGRAM_LOADED until both
 * program segments are loaded, and ENTRYPOINT_UNRESOLVED if either entry
 * cannot be resolved/validated. A failed preflight executes no guest code and
 * consumes no bus-cycle budget.
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

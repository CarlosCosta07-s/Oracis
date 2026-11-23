/*
 * C ABI for the Oracis static dispatch registry.
 *
 * Generated bank tables are plain C data. The C++ machine/runtime registers
 * those immutable tables here and may look up a row before invoking it.
 * Validated rows are fail-closed unless an identity callback is installed.
 */
#ifndef ORACIS_RECOMP_STATIC_DISPATCH_ABI_H
#define ORACIS_RECOMP_STATIC_DISPATCH_ABI_H

#include "oracis/recomp/runtime_arm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*OracisDispatchValidationFn)(
    NdsCpu cpu,
    const NdsStaticValidation *validation,
    uint32_t pc,
    uint32_t thumb,
    void *user);

typedef uint64_t (*OracisDispatchMemoryGenerationFn)(void *user);

typedef enum OracisDispatchRegisterStatus {
    ORACIS_DISPATCH_REGISTER_OK = 0,
    ORACIS_DISPATCH_REGISTER_INVALID_CPU = 1,
    ORACIS_DISPATCH_REGISTER_INVALID_TABLE = 2,
    ORACIS_DISPATCH_REGISTER_INVALID_ENTRY = 3,
    ORACIS_DISPATCH_REGISTER_INVALID_VALIDATION = 4,
    ORACIS_DISPATCH_REGISTER_UNSORTED_TABLE = 5,
    ORACIS_DISPATCH_REGISTER_DUPLICATE_ENTRY_KEY = 6,
    ORACIS_DISPATCH_REGISTER_DUPLICATE_BANK_NAME = 7,
    ORACIS_DISPATCH_REGISTER_DUPLICATE_BANK_TABLE = 8
} OracisDispatchRegisterStatus;

typedef enum OracisDispatchLookupStatus {
    ORACIS_DISPATCH_LOOKUP_FOUND = 0,
    ORACIS_DISPATCH_LOOKUP_INVALID_CPU = 1,
    ORACIS_DISPATCH_LOOKUP_CPU_NOT_REGISTERED = 2,
    ORACIS_DISPATCH_LOOKUP_MISS = 3,
    ORACIS_DISPATCH_LOOKUP_MISALIGNED_TARGET = 4
} OracisDispatchLookupStatus;

int oracis_static_dispatch_register_bank(
    NdsCpu cpu,
    const NdsDispatchEntry *table,
    uint32_t length,
    const char *bank_name);

int oracis_static_dispatch_unregister_bank(
    NdsCpu cpu,
    const NdsDispatchEntry *table,
    uint32_t length);

void oracis_static_dispatch_clear(void);

/*
 * A cached validated row may reuse this callback's previous proof only while
 * callback/context identity and the bound memory-generation key stay stable.
 * Callers with additional mutable validation state must invalidate the
 * StaticDispatchRegistry link epoch when that state changes.
 */
void oracis_static_dispatch_set_validation_callback(
    OracisDispatchValidationFn callback,
    void *user);

void oracis_static_dispatch_set_memory_generation_provider(
    OracisDispatchMemoryGenerationFn provider,
    void *user);

/* Scoped guest-dispatch miss recovery. Pass nullptr to restore fail-closed mode. */
void oracis_static_dispatch_set_miss_handler(
    OracisDispatchMissHandler handler,
    void *user);


const NdsDispatchEntry *oracis_static_dispatch_lookup(
    NdsCpu cpu,
    uint32_t target_pc,
    uint32_t cpsr,
    uint32_t *status,
    uint32_t *candidate_count,
    const NdsDispatchEntry **inactive_candidate);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ORACIS_RECOMP_STATIC_DISPATCH_ABI_H */

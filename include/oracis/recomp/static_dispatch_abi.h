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
    const NdsStaticValidation *validation,
    uint32_t pc,
    uint32_t thumb,
    void *user);

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

void oracis_static_dispatch_set_validation_callback(
    OracisDispatchValidationFn callback,
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

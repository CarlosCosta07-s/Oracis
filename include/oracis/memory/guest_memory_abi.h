/*
 * C ABI binding for the Oracis guest-memory backing store.
 *
 * The generated bank functions bus_read_* / bus_write_* are declared in
 * oracis/recomp/runtime_arm.h. The machine binds a GuestMemory instance
 * through this opaque pointer before invoking generated code.
 */
#ifndef ORACIS_MEMORY_GUEST_MEMORY_ABI_H
#define ORACIS_MEMORY_GUEST_MEMORY_ABI_H

#include "oracis/recomp/runtime_arm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* memory must point to a live oracis::memory::GuestMemory C++ object. */
int oracis_bus_bind_memory(void *memory);
int oracis_bus_unbind_memory(void *memory);

/* Bind a live NdsWramControl device after binding its GuestMemory object. */
int oracis_bus_bind_wram_control(void *control);
int oracis_bus_unbind_wram_control(void *control);

/*
 * Validation callback suitable for oracis_static_dispatch_set_validation_callback.
 * user is the live GuestMemory object that backs the generated CPU bus.
 */
int oracis_bus_validate_dispatch(
    const NdsStaticValidation *validation,
    uint32_t pc,
    uint32_t thumb,
    void *user);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ORACIS_MEMORY_GUEST_MEMORY_ABI_H */

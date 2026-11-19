/*
 * C ABI binding for the Oracis guest-memory backing store.
 *
 * The generated bank functions bus_read_* / bus_write_* are declared in
 * oracis/recomp/runtime_arm.h. The machine binds a GuestMemory instance
 * through this opaque pointer before invoking generated code.
 */
#ifndef ORACIS_MEMORY_GUEST_MEMORY_ABI_H
#define ORACIS_MEMORY_GUEST_MEMORY_ABI_H

#ifdef __cplusplus
extern "C" {
#endif

/* memory must point to a live oracis::memory::GuestMemory C++ object. */
int oracis_bus_bind_memory(void *memory);
int oracis_bus_unbind_memory(void *memory);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ORACIS_MEMORY_GUEST_MEMORY_ABI_H */

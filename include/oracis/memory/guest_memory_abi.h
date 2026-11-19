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

/* Bind the NDS VRAM C/D bank controller after its GuestMemory backing. */
int oracis_bus_bind_vram_control(void *control);
int oracis_bus_unbind_vram_control(void *control);

/* Bind a live NdsInterruptControl device after binding its GuestMemory object. */
int oracis_bus_bind_interrupt_control(void *control);
int oracis_bus_unbind_interrupt_control(void *control);

/* Bind a live NdsTimerControl after its matching interrupt controller. */
int oracis_bus_bind_timer_control(void *control);
int oracis_bus_unbind_timer_control(void *control);

/* Bind a synchronized NdsSystemClock after its matching timer control. */
int oracis_bus_bind_system_clock(void *clock);
int oracis_bus_unbind_system_clock(void *clock);

/* Report cumulative emulated core cycles; call at CPU slice boundaries. */
int oracis_bus_report_cpu_cycles(NdsCpu cpu, uint64_t cumulative_cycles);
int oracis_bus_report_idle_bus_cycles(NdsCpu cpu, uint64_t elapsed_bus_cycles);
uint64_t oracis_bus_global_bus_cycles(void);

/* Bind a live NdsKeypadControl after its matching memory and interrupt controller. */
int oracis_bus_bind_keypad_control(void *control);
int oracis_bus_unbind_keypad_control(void *control);

/*
 * Host input bridge. The ten low bits use 1=pressed. State is retained while
 * the keypad device is unbound and applied when it is next bound.
 */
void oracis_host_set_keypad_state(uint16_t pressed_keys);

/*
 * Validation callback suitable for oracis_static_dispatch_set_validation_callback.
 * user is the live GuestMemory object that backs the generated CPU bus.
 */
int oracis_bus_validate_dispatch(
    NdsCpu cpu,
    const NdsStaticValidation *validation,
    uint32_t pc,
    uint32_t thumb,
    void *user);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ORACIS_MEMORY_GUEST_MEMORY_ABI_H */

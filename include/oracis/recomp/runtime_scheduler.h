/*
 * Cooperative slice-control API for the Oracis ARM9/ARM7 runtime.
 *
 * This layer provides per-CPU cycle accounting, optional synchronous device
 * clock observers and explicit yield/unwind requests. It does not implement
 * DS event ordering, DMA, IRQ exception delivery, IPC, PPU or memory wait states.
 */
#ifndef ORACIS_RECOMP_RUNTIME_SCHEDULER_H
#define ORACIS_RECOMP_RUNTIME_SCHEDULER_H

#include <stdint.h>

#include "oracis/recomp/arm_cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Optional observer for the active CPU cycle cost. Called synchronously
 * after runtime_tick updates the per-CPU count. The observer must not recurse
 * into runtime_tick and must remain alive until explicitly unbound.
 * Only one observer can be registered at a time.
 */
typedef void (*OracisSchedulerCycleObserver)(
    NdsCpu cpu, uint32_t cycles, void *user);

int oracis_scheduler_bind_cycle_observer(
    OracisSchedulerCycleObserver observer, void *user);
int oracis_scheduler_unbind_cycle_observer(
    OracisSchedulerCycleObserver observer, void *user);

int oracis_scheduler_begin_slice(NdsCpu cpu, uint64_t cycle_budget);
void oracis_scheduler_end_slice(NdsCpu cpu);
int oracis_scheduler_request_yield(NdsCpu cpu);
int oracis_scheduler_set_unwinding(NdsCpu cpu, int unwinding);
uint64_t oracis_scheduler_cycle_count(NdsCpu cpu);
void oracis_scheduler_reset(NdsCpu cpu);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ORACIS_RECOMP_RUNTIME_SCHEDULER_H */

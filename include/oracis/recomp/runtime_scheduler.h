/*
 * Cooperative slice-control API for the Oracis ARM9/ARM7 runtime.
 *
 * This layer provides per-CPU cycle accounting and explicit yield/unwind
 * requests. It does not implement DS event ordering, DMA, IRQ exception
 * delivery, IPC, PPU or memory wait states.
 */
#ifndef ORACIS_RECOMP_RUNTIME_SCHEDULER_H
#define ORACIS_RECOMP_RUNTIME_SCHEDULER_H

#include <stdint.h>

#include "oracis/recomp/arm_cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

int oracis_scheduler_begin_slice(NdsCpu cpu, uint64_t cycle_budget);
void oracis_scheduler_end_slice(NdsCpu cpu);
int oracis_scheduler_request_yield(NdsCpu cpu);
int oracis_scheduler_set_unwinding(NdsCpu cpu, int unwinding);
/* Once enabled for an active slice, a reached deadline immediately unwinds
   generated C frames at their next runtime_unwinding() checkpoint. */
int oracis_scheduler_enable_bounded_unwind(NdsCpu cpu);
uint64_t oracis_scheduler_cycle_count(NdsCpu cpu);
void oracis_scheduler_reset(NdsCpu cpu);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ORACIS_RECOMP_RUNTIME_SCHEDULER_H */

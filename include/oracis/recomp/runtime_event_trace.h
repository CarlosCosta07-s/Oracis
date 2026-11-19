/*
 * Bounded C ABI event history for generated-bank diagnostics.
 *
 * Event queues are isolated by CPU. The runtime appends during cooperative
 * execution; a host reads them at scheduler-safe boundaries. This API is not
 * a cross-thread synchronization primitive.
 */
#ifndef ORACIS_RECOMP_RUNTIME_EVENT_TRACE_H
#define ORACIS_RECOMP_RUNTIME_EVENT_TRACE_H

#include <stdint.h>
#include "arm_cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ORACIS_RUNTIME_EVENT_CAPACITY 512u

typedef struct OracisRuntimeEvent {
    uint64_t sequence;
    uint32_t kind;
    uint32_t pc;
    uint32_t addr;
    uint32_t value;
    uint32_t aux;
    uint32_t reserved;
} OracisRuntimeEvent;

/*
 * Number of events currently retained for one CPU, and the cumulative count
 * overwritten since the last clear. Invalid CPU values return zero.
 */
uint32_t oracis_runtime_event_available(NdsCpu cpu);
uint64_t oracis_runtime_event_dropped(NdsCpu cpu);

/*
 * Read events in sequence order.
 *
 * *next_sequence is an inclusive cursor; zero starts at the oldest retained
 * event. If the cursor was overtaken by ring-buffer overflow, it is advanced
 * to the oldest retained event. On return it points to the next event to read.
 * events may be null only when capacity is zero. The return is the number
 * copied. The dropped counter is cumulative until clear.
 */
uint32_t oracis_runtime_event_read(
    NdsCpu cpu,
    uint64_t *next_sequence,
    OracisRuntimeEvent *events,
    uint32_t capacity);

/* Clear one CPU's event history and its overflow counter; returns 0 if invalid. */
int oracis_runtime_event_clear(NdsCpu cpu);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ORACIS_RECOMP_RUNTIME_EVENT_TRACE_H */

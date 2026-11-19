/*
 * C bridge between the SDL host and the C++ NdsMachine runtime.
 *
 * The host owns the lifecycle calls. Guest-memory, device and CPU state remain
 * owned by the single NdsMachine instance in the bridge implementation.
 */
#ifndef ORACIS_HOST_RUNTIME_BRIDGE_H
#define ORACIS_HOST_RUNTIME_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum OracisHostRuntimeStatus {
    ORACIS_HOST_RUNTIME_OK = 0,
    ORACIS_HOST_RUNTIME_NOT_INITIALIZED = 1,
    ORACIS_HOST_RUNTIME_ALREADY_INITIALIZED = 2,
    ORACIS_HOST_RUNTIME_MACHINE_ERROR = 3
} OracisHostRuntimeStatus;

/* Initializes the process-wide NDS machine and binds its base devices. */
int oracis_host_runtime_initialize(void);

/* Unbinds the machine and releases its process-wide runtime bindings. */
int oracis_host_runtime_shutdown(void);

/* Read-only host diagnostics. */
int oracis_host_runtime_is_initialized(void);
uint64_t oracis_host_runtime_global_bus_cycles(void);

/* Raw NdsMachineStatus from the most recent initialize/shutdown call. */
int oracis_host_runtime_last_machine_status(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ORACIS_HOST_RUNTIME_BRIDGE_H */

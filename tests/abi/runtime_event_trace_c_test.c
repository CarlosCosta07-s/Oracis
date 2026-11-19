#include "oracis/recomp/runtime_event_trace.h"

#include <stddef.h>
#include <stdint.h>

typedef char event_record_must_be_32_bytes[
    sizeof(OracisRuntimeEvent) == 32u ? 1 : -1];
typedef char event_capacity_must_be_bounded[
    ORACIS_RUNTIME_EVENT_CAPACITY == 512u ? 1 : -1];

int main(void)
{
    uint64_t cursor = 0u;
    if (oracis_runtime_event_available(NDS_ARM9) != 0u) return 1;
    if (oracis_runtime_event_dropped(NDS_ARM7) != 0u) return 2;
    if (oracis_runtime_event_read(
            NDS_ARM9, &cursor, NULL, 0u) != 0u) return 3;
    if (!oracis_runtime_event_clear(NDS_ARM9)) return 4;
    if (oracis_runtime_event_clear((NdsCpu)99) != 0) return 5;
    return 0;
}

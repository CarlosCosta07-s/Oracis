#include "oracis/host/runtime_bridge.h"

#include "oracis/machine/nds_machine.hpp"
#include "oracis/recomp/static_dispatch.hpp"

namespace {

oracis::machine::NdsMachine &host_machine() noexcept
{
    static oracis::machine::NdsMachine machine(
        oracis::memory::SharedWramMode::Arm9All32K);
    return machine;
}

int g_last_machine_status =
    static_cast<int>(oracis::machine::NdsMachineStatus::Ok);

struct IrqProviderContext {
    OracisHostIrqContextProvider provider = nullptr;
    void *user = nullptr;
};

bool invoke_irq_provider(
    void *opaque, NdsCpu cpu, const ArmCpuState *state,
    uint32_t *return_address, uint32_t *vector_base)
{
    auto *context = static_cast<IrqProviderContext *>(opaque);
    if (context == nullptr || context->provider == nullptr) return false;
    return context->provider(
        context->user, cpu, state, return_address, vector_base) != 0;
}

bool has_both_program_banks() noexcept
{
    auto &registry = oracis::recomp::global_static_dispatch_registry();
    return registry.bank_count(NDS_ARM9) != 0u &&
           registry.bank_count(NDS_ARM7) != 0u;
}

} // namespace

extern "C" int oracis_host_runtime_initialize(void)
{
    auto &machine = host_machine();
    if (machine.initialized()) {
        return ORACIS_HOST_RUNTIME_ALREADY_INITIALIZED;
    }

    const auto status = machine.initialize();
    g_last_machine_status = static_cast<int>(status);
    return status == oracis::machine::NdsMachineStatus::Ok
        ? ORACIS_HOST_RUNTIME_OK
        : ORACIS_HOST_RUNTIME_MACHINE_ERROR;
}

extern "C" int oracis_host_runtime_shutdown(void)
{
    auto &machine = host_machine();
    const auto status = machine.shutdown();
    g_last_machine_status = static_cast<int>(status);
    return status == oracis::machine::NdsMachineStatus::Ok
        ? ORACIS_HOST_RUNTIME_OK
        : ORACIS_HOST_RUNTIME_MACHINE_ERROR;
}

extern "C" int oracis_host_runtime_run_round(
    uint64_t bus_cycle_budget,
    OracisHostIrqContextProvider irq_context,
    void *context)
{
    auto &machine = host_machine();
    if (!machine.initialized()) return ORACIS_HOST_RUNTIME_NOT_INITIALIZED;
    if (!has_both_program_banks()) return ORACIS_HOST_RUNTIME_NO_PROGRAM_BANKS;

    oracis::machine::NdsMachineStatus status;
    if (irq_context == nullptr) {
        status = machine.run_dual_core_round(
            bus_cycle_budget,
            &oracis::machine::NdsMachine::execute_registered_bank_slice,
            nullptr,
            nullptr);
    } else {
        IrqProviderContext provider_context{irq_context, context};
        status = machine.run_dual_core_round(
            bus_cycle_budget,
            &oracis::machine::NdsMachine::execute_registered_bank_slice,
            &invoke_irq_provider,
            &provider_context);
    }

    g_last_machine_status = static_cast<int>(status);
    return status == oracis::machine::NdsMachineStatus::Ok
        ? ORACIS_HOST_RUNTIME_OK
        : ORACIS_HOST_RUNTIME_MACHINE_ERROR;
}

extern "C" int oracis_host_runtime_is_initialized(void)
{
    return host_machine().initialized() ? 1 : 0;
}

extern "C" int oracis_host_runtime_has_program_banks(void)
{
    return has_both_program_banks() ? 1 : 0;
}

extern "C" uint64_t oracis_host_runtime_global_bus_cycles(void)
{
    return host_machine().global_bus_cycles();
}

extern "C" int oracis_host_runtime_last_machine_status(void)
{
    return g_last_machine_status;
}

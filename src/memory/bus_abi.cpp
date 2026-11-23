#include "oracis/memory/guest_memory_abi.h"

#include "oracis/memory/guest_memory.hpp"
#include "oracis/memory/nds_wram_control.hpp"
#include "oracis/memory/nds_vram_control.hpp"
#include "oracis/memory/nds_video_memory.hpp"
#include "oracis/memory/nds_post_flag.hpp"
#include "oracis/memory/nds_display_control.hpp"
#include "oracis/memory/nds_geometry_engine.hpp"
#include "oracis/memory/nds_interrupt_control.hpp"
#include "oracis/memory/nds_timer_control.hpp"
#include "oracis/memory/nds_keypad_control.hpp"
#include "oracis/memory/nds_rtc.hpp"
#include "oracis/memory/nds_spi_control.hpp"
#include "oracis/memory/nds_ipc_control.hpp"
#include "oracis/memory/nds_game_card_control.hpp"
#include "oracis/memory/nds_dma_control.hpp"
#include "oracis/memory/nds_system_clock.hpp"
#include "oracis/recomp/runtime_arm.h"
#include "oracis/recomp/static_dispatch_abi.h"

#include <cstdio>
#include <cstdlib>

namespace {

oracis::memory::GuestMemory *g_guest_memory = nullptr;
oracis::memory::NdsWramControl *g_wram_control = nullptr;
oracis::memory::NdsVramControl *g_vram_control = nullptr;
oracis::memory::NdsVideoMemory *g_video_memory = nullptr;
oracis::memory::NdsPostFlag *g_post_flag = nullptr;
oracis::memory::NdsDisplayControl *g_display_control = nullptr;
oracis::memory::NdsGeometryEngine *g_geometry_engine = nullptr;
oracis::memory::NdsInterruptControl *g_interrupt_control = nullptr;
oracis::memory::NdsIpcControl *g_ipc_control = nullptr;
oracis::memory::NdsGameCardControl *g_game_card_control = nullptr;
oracis::memory::NdsDmaControl *g_dma_control = nullptr;
oracis::memory::NdsTimerControl *g_timer_control = nullptr;
oracis::memory::NdsKeypadControl *g_keypad_control = nullptr;
oracis::memory::NdsRtc *g_rtc = nullptr;
oracis::memory::NdsSpiControl *g_spi_control = nullptr;
oracis::memory::NdsSystemClock *g_system_clock = nullptr;
void *g_arm9_access_check_context = nullptr;
OracisBusArm9AccessCheck g_arm9_access_check = nullptr;
uint16_t g_host_keypad_pressed_keys = 0u;

uint64_t bus_memory_generation(void *context) noexcept
{
    const auto *memory =
        static_cast<const oracis::memory::GuestMemory *>(context);
    return memory != nullptr ? memory->mutation_generation() : 0u;
}

[[noreturn]] void bus_failure(const char *operation,
                              uint32_t address,
                              uint32_t width,
                              oracis::memory::Status status)
{
    std::fprintf(stderr,
        "[Oracis bus] %s failed: CPU=%u address=0x%08X width=%u "
        "status=%s PC=0x%08X\n",
        operation,
        static_cast<unsigned>(g_nds_active),
        address,
        width,
        oracis::memory::GuestMemory::status_name(status),
        oracis_active_cpu_state ? oracis_active_cpu_state->R[15] : 0u);
    std::abort();
}

oracis::memory::GuestMemory &bound_memory(const char *operation,
                                         uint32_t address,
                                         uint32_t width)
{
    if (g_guest_memory == nullptr) {
        bus_failure(operation, address, width,
                    oracis::memory::Status::Unmapped);
    }
    return *g_guest_memory;
}

bool validate_arm9_access(
    const char *operation, uint32_t address, uint32_t width,
    uint32_t access_kind)
{
    if (g_arm9_access_check == nullptr || g_nds_active != NDS_ARM9) {
        return true;
    }
    if (oracis_active_cpu_state == nullptr) {
        bus_failure(operation, address, width,
                    oracis::memory::Status::PermissionDenied);
    }

    const uint32_t mode =
        oracis_active_cpu_state->cpsr & UINT32_C(0x1F);
    uint32_t privileged = 0u;
    switch (mode) {
    case UINT32_C(0x10): /* User */
        privileged = 0u;
        break;
    case UINT32_C(0x11): /* FIQ */
    case UINT32_C(0x12): /* IRQ */
    case UINT32_C(0x13): /* Supervisor */
    case UINT32_C(0x17): /* Abort */
    case UINT32_C(0x1B): /* Undefined */
    case UINT32_C(0x1F): /* System */
        privileged = 1u;
        break;
    default:
        bus_failure(operation, address, width,
                    oracis::memory::Status::PermissionDenied);
    }

    const int result = g_arm9_access_check(
        g_arm9_access_check_context, g_nds_active,
        address, width, access_kind, privileged);
    if (result > 0) return true;
    if (result == 0) return false;
    bus_failure(operation, address, width,
                oracis::memory::Status::PermissionDenied);
}


uint32_t read_bus(uint32_t address, uint32_t width, const char *operation)
{
    auto &memory = bound_memory(operation, address, width);
    if (!validate_arm9_access(
            operation, address, width, ORACIS_BUS_ACCESS_DATA_READ)) {
        return 0u;
    }
    if (g_post_flag != nullptr) {
        const auto mmio = g_post_flag->read(g_nds_active, address, width);
        if (mmio.status != oracis::memory::Status::Unmapped) {
            if (!mmio) bus_failure(operation, address, width, mmio.status);
            return mmio.value;
        }
    }
    if (g_geometry_engine != nullptr) {
        const auto mmio = g_geometry_engine->read(
            g_nds_active, address, width);
        if (mmio.status != oracis::memory::Status::Unmapped) {
            if (!mmio) bus_failure(operation, address, width, mmio.status);
            return mmio.value;
        }
    }
    if (g_display_control != nullptr) {
        const auto mmio = g_display_control->read(
            g_nds_active, address, width);
        if (mmio.status != oracis::memory::Status::Unmapped) {
            if (!mmio) bus_failure(operation, address, width, mmio.status);
            return mmio.value;
        }
    }
    if (g_keypad_control != nullptr) {
        const auto mmio = g_keypad_control->read(
            g_nds_active, address, width);
        if (mmio.status != oracis::memory::Status::Unmapped) {
            if (!mmio) bus_failure(operation, address, width, mmio.status);
            return mmio.value;
        }
    }
    if (g_rtc != nullptr) {
        const auto mmio = g_rtc->read(g_nds_active, address, width);
        if (mmio.status != oracis::memory::Status::Unmapped) {
            if (!mmio) bus_failure(operation, address, width, mmio.status);
            return mmio.value;
        }
    }
    if (g_spi_control != nullptr) {
        const auto mmio = g_spi_control->read(g_nds_active, address, width);
        if (mmio.status != oracis::memory::Status::Unmapped) {
            if (!mmio) bus_failure(operation, address, width, mmio.status);
            return mmio.value;
        }
    }
    if (g_ipc_control != nullptr) {
        const auto mmio = g_ipc_control->read(
            g_nds_active, address, width);
        if (mmio.status != oracis::memory::Status::Unmapped) {
            if (!mmio) bus_failure(operation, address, width, mmio.status);
            return mmio.value;
        }
    }
    if (g_game_card_control != nullptr) {
        const auto mmio=g_game_card_control->read(g_nds_active,address,width);
        if(mmio.status!=oracis::memory::Status::Unmapped){if(!mmio)bus_failure(operation,address,width,mmio.status);return mmio.value;}
    }
    if (g_dma_control != nullptr) {
        const auto mmio = g_dma_control->read(
            g_nds_active, address, width);
        if (mmio.status != oracis::memory::Status::Unmapped) {
            if (!mmio) bus_failure(operation, address, width, mmio.status);
            return mmio.value;
        }
    }
    if (g_timer_control != nullptr) {
        const auto mmio = g_timer_control->read(
            g_nds_active, address, width);
        if (mmio.status != oracis::memory::Status::Unmapped) {
            if (!mmio) bus_failure(operation, address, width, mmio.status);
            return mmio.value;
        }
    }
    if (g_interrupt_control != nullptr) {
        const auto mmio = g_interrupt_control->read(
            g_nds_active, address, width);
        if (mmio.status != oracis::memory::Status::Unmapped) {
            if (!mmio) bus_failure(operation, address, width, mmio.status);
            return mmio.value;
        }
    }
    if (g_wram_control != nullptr) {
        const auto mmio = g_wram_control->read(
            g_nds_active, address, width);
        if (mmio.status != oracis::memory::Status::Unmapped) {
            if (!mmio) bus_failure(operation, address, width, mmio.status);
            return mmio.value;
        }
    }
    if (g_vram_control != nullptr) {
        const auto mmio = g_vram_control->read(
            g_nds_active, address, width);
        if (mmio.status != oracis::memory::Status::Unmapped) {
            if (!mmio) bus_failure(operation, address, width, mmio.status);
            return mmio.value;
        }
    }
    const auto result = memory.read(g_nds_active, address, width);
    if (!result) bus_failure(operation, address, width, result.status);
    return result.value;
}

void write_bus(uint32_t address,
               uint32_t width,
               uint32_t value,
               const char *operation)
{
    auto &memory = bound_memory(operation, address, width);
    if (!validate_arm9_access(
            operation, address, width, ORACIS_BUS_ACCESS_DATA_WRITE)) {
        return;
    }
    if (g_post_flag != nullptr) {
        const auto mmio = g_post_flag->write(
            g_nds_active, address, width, value);
        if (mmio != oracis::memory::Status::Unmapped) {
            if (mmio != oracis::memory::Status::Ok) {
                bus_failure(operation, address, width, mmio);
            }
            return;
        }
    }
    if (g_geometry_engine != nullptr) {
        const auto mmio = g_geometry_engine->write(
            g_nds_active, address, width, value);
        if (mmio != oracis::memory::Status::Unmapped) {
            if (mmio != oracis::memory::Status::Ok) {
                bus_failure(operation, address, width, mmio);
            }
            return;
        }
    }
    if (g_display_control != nullptr) {
        const auto mmio = g_display_control->write(
            g_nds_active, address, width, value);
        if (mmio != oracis::memory::Status::Unmapped) {
            if (mmio != oracis::memory::Status::Ok) {
                bus_failure(operation, address, width, mmio);
            }
            return;
        }
    }
    if (g_keypad_control != nullptr) {
        const auto mmio = g_keypad_control->write(
            g_nds_active, address, width, value);
        if (mmio != oracis::memory::Status::Unmapped) {
            if (mmio != oracis::memory::Status::Ok) {
                bus_failure(operation, address, width, mmio);
            }
            return;
        }
    }
    if (g_rtc != nullptr) {
        const auto mmio = g_rtc->write(g_nds_active, address, width, value);
        if (mmio != oracis::memory::Status::Unmapped) {
            if (mmio != oracis::memory::Status::Ok) {
                bus_failure(operation, address, width, mmio);
            }
            return;
        }
    }
    if (g_spi_control != nullptr) {
        const auto mmio = g_spi_control->write(
            g_nds_active, address, width, value);
        if (mmio != oracis::memory::Status::Unmapped) {
            if (mmio != oracis::memory::Status::Ok) {
                bus_failure(operation, address, width, mmio);
            }
            return;
        }
    }
    if (g_ipc_control != nullptr) {
        const auto mmio = g_ipc_control->write(
            g_nds_active, address, width, value);
        if (mmio != oracis::memory::Status::Unmapped) {
            if (mmio != oracis::memory::Status::Ok) {
                bus_failure(operation, address, width, mmio);
            }
            return;
        }
    }
    if (g_game_card_control != nullptr) {
        const auto mmio=g_game_card_control->write(g_nds_active,address,width,value);
        if(mmio!=oracis::memory::Status::Unmapped){if(mmio!=oracis::memory::Status::Ok)bus_failure(operation,address,width,mmio);return;}
    }
    if (g_dma_control != nullptr) {
        const auto mmio = g_dma_control->write(
            g_nds_active, address, width, value);
        if (mmio != oracis::memory::Status::Unmapped) {
            if (mmio != oracis::memory::Status::Ok) {
                bus_failure(operation, address, width, mmio);
            }
            return;
        }
    }
    if (g_timer_control != nullptr) {
        const auto mmio = g_timer_control->write(
            g_nds_active, address, width, value);
        if (mmio != oracis::memory::Status::Unmapped) {
            if (mmio != oracis::memory::Status::Ok) {
                bus_failure(operation, address, width, mmio);
            }
            return;
        }
    }
    if (g_interrupt_control != nullptr) {
        const auto mmio = g_interrupt_control->write(
            g_nds_active, address, width, value);
        if (mmio != oracis::memory::Status::Unmapped) {
            if (mmio != oracis::memory::Status::Ok) {
                bus_failure(operation, address, width, mmio);
            }
            if (g_geometry_engine != nullptr) {
                g_geometry_engine->refresh_fifo_irq();
            }
            return;
        }
    }
    if (g_wram_control != nullptr) {
        const auto mmio = g_wram_control->write(
            g_nds_active, address, width, value);
        if (mmio != oracis::memory::Status::Unmapped) {
            if (mmio != oracis::memory::Status::Ok) {
                bus_failure(operation, address, width, mmio);
            }
            return;
        }
    }
    if (g_vram_control != nullptr) {
        const auto mmio = g_vram_control->write(
            g_nds_active, address, width, value);
        if (mmio != oracis::memory::Status::Unmapped) {
            if (mmio != oracis::memory::Status::Ok) {
                bus_failure(operation, address, width, mmio);
            }
            return;
        }
    }
    if (g_video_memory != nullptr) {
        const auto video_status = g_video_memory->write(
            g_nds_active, address, width, value);
        if (video_status != oracis::memory::Status::Unmapped) {
            if (video_status != oracis::memory::Status::Ok) {
                bus_failure(operation, address, width, video_status);
            }
            return;
        }
    }
    const auto status = memory.write(g_nds_active, address, width, value);
    if (status != oracis::memory::Status::Ok) {
        bus_failure(operation, address, width, status);
    }
}

} // namespace

extern "C" int oracis_bus_bind_arm9_access_check(
    void *context, OracisBusArm9AccessCheck callback)
{
    if (context == nullptr || callback == nullptr) return 0;
    if (g_arm9_access_check != nullptr ||
        g_arm9_access_check_context != nullptr) {
        return g_arm9_access_check == callback &&
               g_arm9_access_check_context == context ? 1 : 0;
    }
    g_arm9_access_check_context = context;
    g_arm9_access_check = callback;
    return 1;
}

extern "C" int oracis_bus_unbind_arm9_access_check(
    void *context, OracisBusArm9AccessCheck callback)
{
    if (context == nullptr || callback == nullptr ||
        g_arm9_access_check_context != context ||
        g_arm9_access_check != callback) {
        return 0;
    }
    g_arm9_access_check_context = nullptr;
    g_arm9_access_check = nullptr;
    return 1;
}

extern "C" int oracis_bus_bind_memory(void *memory)
{
    if (memory == nullptr) return 0;
    auto *guest_memory = static_cast<oracis::memory::GuestMemory *>(memory);
    if (g_guest_memory != nullptr && g_guest_memory != guest_memory) return 0;

    g_guest_memory = guest_memory;
    oracis_static_dispatch_set_memory_generation_provider(
        bus_memory_generation, guest_memory);
    oracis_static_dispatch_set_validation_callback(
        oracis_bus_validate_dispatch, guest_memory);
    return 1;
}

extern "C" int oracis_bus_unbind_memory(void *memory)
{
    if (memory == nullptr || g_guest_memory !=
            static_cast<oracis::memory::GuestMemory *>(memory) ||
        (g_wram_control != nullptr &&
         &g_wram_control->backing_memory() == g_guest_memory) ||
        (g_vram_control != nullptr &&
         &g_vram_control->backing_memory() == g_guest_memory) ||
        (g_video_memory != nullptr &&
         &g_video_memory->backing_memory() == g_guest_memory) ||
        g_post_flag != nullptr ||
        g_display_control != nullptr ||
        g_geometry_engine != nullptr ||
        g_game_card_control != nullptr ||
        g_arm9_access_check != nullptr ||
        g_arm9_access_check_context != nullptr ||
        (g_interrupt_control != nullptr &&
         &g_interrupt_control->backing_memory() == g_guest_memory) ||
        (g_timer_control != nullptr &&
         &g_timer_control->backing_memory() == g_guest_memory) ||
        (g_keypad_control != nullptr &&
         &g_keypad_control->backing_memory() == g_guest_memory)) {
        return 0;
    }
    /* Clear the provider before its GuestMemory context can expire. */
    oracis_static_dispatch_set_memory_generation_provider(nullptr, nullptr);
    g_guest_memory = nullptr;
    oracis_static_dispatch_set_validation_callback(nullptr, nullptr);
    return 1;
}

extern "C" int oracis_bus_bind_wram_control(void *control)
{
    if (control == nullptr || g_guest_memory == nullptr) return 0;
    auto *device = static_cast<oracis::memory::NdsWramControl *>(control);
    if (!device->installed() || &device->backing_memory() != g_guest_memory) {
        return 0;
    }
    if (g_wram_control != nullptr) return 0;
    g_wram_control = device;
    return 1;
}

extern "C" int oracis_bus_unbind_wram_control(void *control)
{
    if (control == nullptr || g_wram_control !=
            static_cast<oracis::memory::NdsWramControl *>(control)) {
        return 0;
    }
    g_wram_control = nullptr;
    return 1;
}

extern "C" int oracis_bus_bind_vram_control(void *control)
{
    if (control == nullptr || g_guest_memory == nullptr) return 0;
    auto *device = static_cast<oracis::memory::NdsVramControl *>(control);
    if (!device->installed() || &device->backing_memory() != g_guest_memory) {
        return 0;
    }
    if (g_vram_control != nullptr) return 0;
    g_vram_control = device;
    return 1;
}

extern "C" int oracis_bus_unbind_vram_control(void *control)
{
    if (control == nullptr || g_vram_control !=
            static_cast<oracis::memory::NdsVramControl *>(control)) {
        return 0;
    }
    g_vram_control = nullptr;
    return 1;
}

extern "C" int oracis_bus_bind_video_memory(void *video_memory)
{
    if (video_memory == nullptr || g_guest_memory == nullptr) return 0;
    auto *device =
        static_cast<oracis::memory::NdsVideoMemory *>(video_memory);
    if (!device->installed() || &device->backing_memory() != g_guest_memory) {
        return 0;
    }
    if (g_video_memory != nullptr) return 0;
    g_video_memory = device;
    return 1;
}

extern "C" int oracis_bus_unbind_video_memory(void *video_memory)
{
    if (video_memory == nullptr || g_video_memory !=
            static_cast<oracis::memory::NdsVideoMemory *>(video_memory)) {
        return 0;
    }
    g_video_memory = nullptr;
    return 1;
}

extern "C" int oracis_bus_bind_post_flag(void *post_flag)
{
    if (post_flag == nullptr || g_guest_memory == nullptr) return 0;
    auto *device = static_cast<oracis::memory::NdsPostFlag *>(post_flag);
    if (!device->installed() || g_post_flag != nullptr) return 0;
    g_post_flag = device;
    return 1;
}

extern "C" int oracis_bus_unbind_post_flag(void *post_flag)
{
    if (post_flag == nullptr || g_post_flag !=
            static_cast<oracis::memory::NdsPostFlag *>(post_flag)) {
        return 0;
    }
    g_post_flag = nullptr;
    return 1;
}

extern "C" int oracis_bus_bind_display_control(void *display_control)
{
    if (display_control == nullptr || g_guest_memory == nullptr) return 0;
    auto *device =
        static_cast<oracis::memory::NdsDisplayControl *>(display_control);
    if (!device->installed() ||
        &device->interrupt_control() != g_interrupt_control ||
        g_display_control != nullptr) {
        return 0;
    }
    g_display_control = device;
    return 1;
}

extern "C" int oracis_bus_unbind_display_control(void *display_control)
{
    if (display_control == nullptr || g_display_control !=
            static_cast<oracis::memory::NdsDisplayControl *>(display_control)) {
        return 0;
    }
    g_display_control = nullptr;
    return 1;
}

extern "C" int oracis_bus_bind_geometry_engine(void *geometry_engine)
{
    if (geometry_engine == nullptr || g_guest_memory == nullptr ||
        g_interrupt_control == nullptr) {
        return 0;
    }
    auto *device =
        static_cast<oracis::memory::NdsGeometryEngine *>(geometry_engine);
    if (!device->installed() ||
        &device->interrupt_control() != g_interrupt_control ||
        g_geometry_engine != nullptr) {
        return 0;
    }
    g_geometry_engine = device;
    return 1;
}

extern "C" int oracis_bus_unbind_geometry_engine(void *geometry_engine)
{
    if (geometry_engine == nullptr || g_geometry_engine !=
            static_cast<oracis::memory::NdsGeometryEngine *>(geometry_engine) ||
        g_system_clock != nullptr) {
        return 0;
    }
    g_geometry_engine = nullptr;
    return 1;
}

extern "C" int oracis_bus_bind_interrupt_control(void *control)
{
    if (control == nullptr || g_guest_memory == nullptr) return 0;
    auto *device =
        static_cast<oracis::memory::NdsInterruptControl *>(control);
    if (&device->backing_memory() != g_guest_memory) return 0;
    if (g_interrupt_control != nullptr) return 0;
    g_interrupt_control = device;
    return 1;
}

extern "C" int oracis_bus_unbind_interrupt_control(void *control)
{
    if (control == nullptr || g_interrupt_control !=
            static_cast<oracis::memory::NdsInterruptControl *>(control) ||
        (g_timer_control != nullptr &&
         &g_timer_control->interrupt_control() == g_interrupt_control) ||
        (g_keypad_control != nullptr &&
         &g_keypad_control->interrupt_control() == g_interrupt_control) ||
        (g_spi_control != nullptr &&
         &g_spi_control->interrupt_control() == g_interrupt_control) ||
        (g_ipc_control != nullptr &&
         &g_ipc_control->interrupt_control() == g_interrupt_control) ||
        (g_dma_control != nullptr &&
         &g_dma_control->interrupt_control() == g_interrupt_control) ||
        (g_display_control != nullptr &&
         &g_display_control->interrupt_control() == g_interrupt_control) ||
        (g_geometry_engine != nullptr &&
         &g_geometry_engine->interrupt_control() == g_interrupt_control) ||
        (g_game_card_control != nullptr &&
         &g_game_card_control->interrupt_control() == g_interrupt_control)) {
        return 0;
    }
    g_interrupt_control = nullptr;
    return 1;
}

extern "C" int oracis_bus_bind_game_card_control(void *control)
{
    if(control==nullptr||g_interrupt_control==nullptr)return 0;
    auto *d=static_cast<oracis::memory::NdsGameCardControl*>(control);
    if(!d->installed()||&d->interrupt_control()!=g_interrupt_control||g_game_card_control!=nullptr)return 0;
    g_game_card_control=d;return 1;
}
extern "C" int oracis_bus_unbind_game_card_control(void *control)
{
    if(control==nullptr||g_game_card_control!=static_cast<oracis::memory::NdsGameCardControl*>(control))return 0;
    g_game_card_control=nullptr;return 1;
}

extern "C" int oracis_bus_bind_ipc_control(void *control)
{
    if (control == nullptr || g_interrupt_control == nullptr) return 0;
    auto *device = static_cast<oracis::memory::NdsIpcControl *>(control);
    if (&device->interrupt_control() != g_interrupt_control ||
        g_ipc_control != nullptr) {
        return 0;
    }
    g_ipc_control = device;
    return 1;
}

extern "C" int oracis_bus_unbind_ipc_control(void *control)
{
    if (control == nullptr || g_ipc_control !=
            static_cast<oracis::memory::NdsIpcControl *>(control)) {
        return 0;
    }
    g_ipc_control = nullptr;
    return 1;
}

extern "C" int oracis_bus_bind_dma_control(void *control)
{
    if (control == nullptr || g_guest_memory == nullptr ||
        g_interrupt_control == nullptr) return 0;
    auto *device = static_cast<oracis::memory::NdsDmaControl *>(control);
    if (&device->backing_memory() != g_guest_memory ||
        &device->interrupt_control() != g_interrupt_control ||
        g_dma_control != nullptr) {
        return 0;
    }
    g_dma_control = device;
    return 1;
}

extern "C" int oracis_bus_unbind_dma_control(void *control)
{
    if (control == nullptr || g_dma_control !=
            static_cast<oracis::memory::NdsDmaControl *>(control)) {
        return 0;
    }
    g_dma_control = nullptr;
    return 1;
}

extern "C" int oracis_bus_bind_timer_control(void *control)
{
    if (control == nullptr || g_guest_memory == nullptr ||
        g_interrupt_control == nullptr) {
        return 0;
    }
    auto *device = static_cast<oracis::memory::NdsTimerControl *>(control);
    if (&device->backing_memory() != g_guest_memory ||
        &device->interrupt_control() != g_interrupt_control ||
        g_timer_control != nullptr) {
        return 0;
    }
    g_timer_control = device;
    return 1;
}

extern "C" int oracis_bus_unbind_timer_control(void *control)
{
    if (control == nullptr || g_timer_control !=
            static_cast<oracis::memory::NdsTimerControl *>(control) ||
        (g_system_clock != nullptr &&
         &g_system_clock->timer_control() == g_timer_control)) {
        return 0;
    }
    g_timer_control = nullptr;
    return 1;
}



extern "C" int oracis_bus_bind_system_clock(void *clock)
{
    if (clock == nullptr || g_timer_control == nullptr) return 0;
    auto *device = static_cast<oracis::memory::NdsSystemClock *>(clock);
    if (&device->timer_control() != g_timer_control ||
        g_system_clock != nullptr) {
        return 0;
    }
    g_system_clock = device;
    return 1;
}

extern "C" int oracis_bus_unbind_system_clock(void *clock)
{
    if (clock == nullptr || g_system_clock !=
            static_cast<oracis::memory::NdsSystemClock *>(clock)) {
        return 0;
    }
    g_system_clock = nullptr;
    return 1;
}

extern "C" int oracis_bus_report_cpu_cycles(
    NdsCpu cpu, uint64_t cumulative_cycles)
{
    if (g_system_clock == nullptr) return 0;
    return g_system_clock->report_cpu_cycles(cpu, cumulative_cycles) ==
        oracis::memory::NdsClockStatus::Ok ? 1 : 0;
}

extern "C" int oracis_bus_report_idle_bus_cycles(
    NdsCpu cpu, uint64_t elapsed_bus_cycles)
{
    if (g_system_clock == nullptr) return 0;
    return g_system_clock->report_idle_bus_cycles(cpu, elapsed_bus_cycles) ==
        oracis::memory::NdsClockStatus::Ok ? 1 : 0;
}

extern "C" uint64_t oracis_bus_global_bus_cycles(void)
{
    return g_system_clock != nullptr
        ? g_system_clock->global_bus_cycles() : 0u;
}

extern "C" int oracis_bus_bind_keypad_control(void *control)
{
    if (control == nullptr || g_guest_memory == nullptr ||
        g_interrupt_control == nullptr) {
        return 0;
    }
    auto *device =
        static_cast<oracis::memory::NdsKeypadControl *>(control);
    if (&device->backing_memory() != g_guest_memory ||
        &device->interrupt_control() != g_interrupt_control ||
        g_keypad_control != nullptr) {
        return 0;
    }
    g_keypad_control = device;
    g_keypad_control->set_pressed_keys(g_host_keypad_pressed_keys);
    return 1;
}

extern "C" int oracis_bus_unbind_keypad_control(void *control)
{
    if (control == nullptr || g_keypad_control !=
            static_cast<oracis::memory::NdsKeypadControl *>(control)) {
        return 0;
    }
    g_keypad_control = nullptr;
    return 1;
}

extern "C" int oracis_bus_bind_rtc(void *rtc)
{
    if (rtc == nullptr || g_rtc != nullptr) return 0;
    g_rtc = static_cast<oracis::memory::NdsRtc *>(rtc);
    return 1;
}

extern "C" int oracis_bus_unbind_rtc(void *rtc)
{
    if (rtc == nullptr ||
        g_rtc != static_cast<oracis::memory::NdsRtc *>(rtc)) {
        return 0;
    }
    g_rtc = nullptr;
    return 1;
}

extern "C" int oracis_bus_bind_spi_control(void *control)
{
    if (control == nullptr || g_interrupt_control == nullptr) return 0;
    auto *device = static_cast<oracis::memory::NdsSpiControl *>(control);
    if (&device->interrupt_control() != g_interrupt_control ||
        g_spi_control != nullptr) {
        return 0;
    }
    g_spi_control = device;
    return 1;
}

extern "C" int oracis_bus_unbind_spi_control(void *control)
{
    if (control == nullptr || g_spi_control !=
            static_cast<oracis::memory::NdsSpiControl *>(control) ||
        (g_system_clock != nullptr &&
         g_system_clock->spi_control() == g_spi_control)) {
        return 0;
    }
    g_spi_control = nullptr;
    return 1;
}

extern "C" void oracis_host_set_keypad_state(uint16_t pressed_keys)
{
    g_host_keypad_pressed_keys = static_cast<uint16_t>(
        pressed_keys & oracis::memory::NdsKeypadControl::KeyMask);
    if (g_keypad_control != nullptr) {
        g_keypad_control->set_pressed_keys(g_host_keypad_pressed_keys);
    }
}

extern "C" int oracis_bus_validate_dispatch(
    NdsCpu cpu,
    const NdsStaticValidation *validation,
    uint32_t pc,
    uint32_t thumb,
    void *user)
{
    auto *memory = static_cast<oracis::memory::GuestMemory *>(user);
    if (memory == nullptr ||
        (cpu != NDS_ARM9 && cpu != NDS_ARM7) ||
        validation == nullptr || validation->expected == nullptr ||
        thumb > 1u ||
        (validation->dependency_count != 0u &&
         validation->dependencies == nullptr)) {
        return 0;
    }

    const uint32_t instruction_size = thumb != 0u ? 2u : 4u;
    const uint64_t validation_end =
        static_cast<uint64_t>(validation->addr) + validation->size;
    if (validation->size == 0u ||
        validation_end > UINT64_C(0x1'0000'0000) ||
        pc < validation->addr ||
        static_cast<uint64_t>(pc) + instruction_size > validation_end) {
        return 0;
    }

    if (memory->validate_execute(
            cpu, validation->addr, validation->size) !=
        oracis::memory::Status::Ok) {
        return 0;
    }
    if (memory->validate_identity(
            cpu, validation->addr, validation->size,
            validation->expected) != oracis::memory::Status::Ok) {
        return 0;
    }

    for (uint32_t index = 0u;
         index < validation->dependency_count;
         ++index) {
        const NdsStaticValidationRange &dependency =
            validation->dependencies[index];
        if (memory->validate_identity(
                cpu, dependency.addr, dependency.size,
                dependency.expected) != oracis::memory::Status::Ok) {
            return 0;
        }
    }
    return 1;
}

extern "C" uint8_t bus_read_u8(uint32_t address)
{
    return static_cast<uint8_t>(read_bus(address, 1u, "bus_read_u8"));
}

extern "C" uint16_t bus_read_u16(uint32_t address)
{
    return static_cast<uint16_t>(read_bus(address, 2u, "bus_read_u16"));
}

extern "C" uint32_t bus_read_u32(uint32_t address)
{
    return read_bus(address, 4u, "bus_read_u32");
}

extern "C" void bus_write_u8(uint32_t address, uint8_t value)
{
    write_bus(address, 1u, value, "bus_write_u8");
}

extern "C" void bus_write_u16(uint32_t address, uint16_t value)
{
    write_bus(address, 2u, value, "bus_write_u16");
}

extern "C" void bus_write_u32(uint32_t address, uint32_t value)
{
    write_bus(address, 4u, value, "bus_write_u32");
}
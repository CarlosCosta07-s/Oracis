#include "oracis/memory/nds_ext_key_input.hpp"
#include "oracis/memory/nds_touch_mapping.hpp"
#include "oracis/recomp/runtime_arm.h"

#include <cstdint>
#include <cstdio>

namespace {

using oracis::memory::NdsExtKeyInput;
using oracis::memory::NdsTouchCalibration;
using oracis::memory::NdsTouchPoint;
using oracis::memory::Status;
using oracis::memory::nds_touch_adc_from_screen;

int fail(const char *message)
{
    std::fprintf(stderr, "nds_touch_input_test: %s\n", message);
    return 1;
}

/* The default calibration maps exactly 16 ADC counts per pixel. */
static_assert(nds_touch_adc_from_screen(0, 0).x12 == 0x000u);
static_assert(nds_touch_adc_from_screen(255, 191).x12 == 0xFF0u);
static_assert(nds_touch_adc_from_screen(255, 191).y12 == 0xBF0u);

uint32_t ext_read(const NdsExtKeyInput &input, uint32_t width = 2u,
                  uint32_t address = NdsExtKeyInput::RegisterAddress)
{
    return input.read(NDS_ARM7, address, width).value;
}

} // namespace

int main()
{
    /* EXTKEYIN: nothing pressed, hinge open, pen up. */
    NdsExtKeyInput input;
    if (ext_read(input) != 0x007Fu) {
        return fail("idle EXTKEYIN did not read 0x007F");
    }

    /* Active-low buttons. */
    input.set_pressed_buttons(NdsExtKeyInput::HostButtonX);
    if (ext_read(input) != 0x007Eu) return fail("X press did not clear bit 0");
    input.set_pressed_buttons(NdsExtKeyInput::HostButtonY);
    if (ext_read(input) != 0x007Du) return fail("Y press did not clear bit 1");
    input.set_pressed_buttons(NdsExtKeyInput::HostButtonDebug);
    if (ext_read(input) != 0x0077u) return fail("debug press did not clear bit 3");
    input.set_pressed_buttons(0xFFu);
    if (ext_read(input) != 0x0074u) {
        return fail("combined press or host-mask truncation is wrong");
    }
    if (input.pressed_buttons() != NdsExtKeyInput::HostButtonMask) {
        return fail("host button mask was not limited to bits 0-2");
    }
    input.set_pressed_buttons(0u);

    /* Pen is active-low; hinge is active-high. */
    input.set_pen_down(true);
    if (ext_read(input) != 0x003Fu) return fail("pen contact did not clear bit 6");
    input.set_pen_down(false);
    input.set_hinge_closed(true);
    if (ext_read(input) != 0x00FFu) return fail("closed hinge did not set bit 7");
    input.set_hinge_closed(false);

    /* Byte lanes. */
    if (ext_read(input, 1u) != 0x7Fu ||
        ext_read(input, 1u, NdsExtKeyInput::RegisterAddress + 1u) != 0x00u) {
        return fail("EXTKEYIN byte-lane reads are wrong");
    }

    /* Read-only, ARM7 only, 16-bit aligned. */
    if (input.write(NDS_ARM7, NdsExtKeyInput::RegisterAddress, 2u, 0u) !=
            Status::PermissionDenied ||
        input.write(NDS_ARM7, NdsExtKeyInput::RegisterAddress, 1u, 0u) !=
            Status::PermissionDenied) {
        return fail("EXTKEYIN accepted a write");
    }
    if (input.read(NDS_ARM9, NdsExtKeyInput::RegisterAddress, 2u).status !=
            Status::CpuAccessDenied ||
        input.write(NDS_ARM9, NdsExtKeyInput::RegisterAddress, 2u, 0u) !=
            Status::CpuAccessDenied) {
        return fail("ARM9 reached the ARM7-only EXTKEYIN register");
    }
    if (input.read(NDS_ARM7, NdsExtKeyInput::RegisterAddress, 4u).status !=
            Status::InvalidWidth ||
        input.read(NDS_ARM7, NdsExtKeyInput::RegisterAddress + 1u, 2u).status !=
            Status::InvalidWidth) {
        return fail("EXTKEYIN accepted a bad width or misaligned access");
    }
    if (input.read(NDS_ARM7, NdsExtKeyInput::RegisterAddress + 2u, 2u).status !=
            Status::Unmapped ||
        input.read(NDS_ARM7, NdsExtKeyInput::RegisterAddress - 2u, 2u).status !=
            Status::Unmapped ||
        input.read(static_cast<NdsCpu>(77), NdsExtKeyInput::RegisterAddress, 2u)
                .status != Status::InvalidCpu) {
        return fail("EXTKEYIN neighbor or invalid-CPU access was accepted");
    }

    /* Default mapping: 16 counts per pixel. */
    {
        const NdsTouchPoint center = nds_touch_adc_from_screen(128, 96);
        if (center.x12 != 0x800u || center.y12 != 0x600u) {
            return fail("screen center did not map to 0x800/0x600");
        }
        const NdsTouchPoint one = nds_touch_adc_from_screen(1, 1);
        if (one.x12 != 16u || one.y12 != 16u) {
            return fail("one pixel did not map to 16 ADC counts");
        }
    }

    /* Out-of-range pixels clamp to the screen edges. */
    {
        const NdsTouchPoint low = nds_touch_adc_from_screen(-40, -1);
        const NdsTouchPoint high = nds_touch_adc_from_screen(9000, 300);
        if (low.x12 != 0u || low.y12 != 0u ||
            high.x12 != 0xFF0u || high.y12 != 0xBF0u) {
            return fail("out-of-range pixels were not clamped");
        }
    }

    /* Custom calibration: offset origin, different scale. */
    {
        NdsTouchCalibration calibration;
        calibration.adc_x1 = 0x100u;
        calibration.screen_x1 = 16u;
        calibration.adc_x2 = 0xF00u;
        calibration.screen_x2 = 240u;
        calibration.adc_y1 = 0x200u;
        calibration.screen_y1 = 16u;
        calibration.adc_y2 = 0xA00u;
        calibration.screen_y2 = 176u;
        const NdsTouchPoint first =
            nds_touch_adc_from_screen(16, 16, calibration);
        const NdsTouchPoint second =
            nds_touch_adc_from_screen(240, 176, calibration);
        const NdsTouchPoint middle =
            nds_touch_adc_from_screen(128, 96, calibration);
        if (first.x12 != 0x100u || first.y12 != 0x200u ||
            second.x12 != 0xF00u || second.y12 != 0xA00u) {
            return fail("calibration points did not map to their ADC values");
        }
        if (middle.x12 != 0x800u || middle.y12 != 0x600u) {
            return fail("calibration midpoint is wrong");
        }
        /* Pixels outside the two reference points extrapolate and clamp. */
        const NdsTouchPoint edge =
            nds_touch_adc_from_screen(0, 0, calibration);
        if (edge.x12 >= first.x12 || edge.y12 >= first.y12) {
            return fail("extrapolation below the first calibration point is wrong");
        }
    }

    /* Non-exact divisions round to nearest, for rising and falling slopes. */
    {
        NdsTouchCalibration rising;
        rising.adc_x2 = 0x800u; /* 2048 counts over 255 pixels = 8.03 each */
        /* x = 16 is exactly 128.502 counts: rounds up, truncation gives 128. */
        if (nds_touch_adc_from_screen(16, 0, rising).x12 != 129u ||
            nds_touch_adc_from_screen(1, 0, rising).x12 != 8u) {
            return fail("rising-slope mapping did not round to nearest");
        }
        NdsTouchCalibration falling;
        falling.adc_x1 = 0x800u;
        falling.adc_x2 = 0x000u;
        /* 2048 - 128.502 = 1919.498: nearest is 1919, truncation gives 1920. */
        if (nds_touch_adc_from_screen(16, 0, falling).x12 != 1919u ||
            nds_touch_adc_from_screen(1, 0, falling).x12 != 2040u) {
            return fail("falling-slope mapping did not round to nearest");
        }
    }

    /* A calibration with a falling slope and a degenerate span. */
    {
        NdsTouchCalibration inverted;
        inverted.adc_x1 = 0xF00u;
        inverted.adc_x2 = 0x100u;
        const NdsTouchPoint left = nds_touch_adc_from_screen(0, 0, inverted);
        const NdsTouchPoint right = nds_touch_adc_from_screen(255, 0, inverted);
        if (left.x12 != 0xF00u || right.x12 != 0x100u) {
            return fail("falling-slope calibration is wrong");
        }
        NdsTouchCalibration degenerate;
        degenerate.screen_x2 = degenerate.screen_x1;
        degenerate.adc_x1 = 0x123u;
        if (nds_touch_adc_from_screen(200, 0, degenerate).x12 != 0x123u) {
            return fail("degenerate calibration span did not fall back to ADC 1");
        }
        /* Extrapolation never leaves 12 bits. */
        NdsTouchCalibration steep;
        steep.screen_x2 = 1u;
        steep.adc_x2 = 0xFFFu;
        if (nds_touch_adc_from_screen(255, 0, steep).x12 != 0xFFFu) {
            return fail("steep calibration was not clamped to 12 bits");
        }
    }

    std::puts("nds_touch_input_test: ok");
    return 0;
}

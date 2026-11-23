/*
 * Conversion from a bottom-screen pixel to the raw 12-bit touchscreen ADC
 * coordinates that NdsSpiControl returns.
 *
 * Real software maps ADC values back to pixels with a two-point calibration
 * stored in the console's user settings (ADC x/y at two known screen points).
 * The default calibration here is a plain linear mapping across the full ADC
 * range and is an assumption: it round-trips with itself, but a game that
 * reads calibration from a user-supplied firmware image will land the pen a
 * few pixels away until the host feeds that calibration in through
 * NdsTouchCalibration. Nothing here reads or embeds firmware data.
 */
#ifndef ORACIS_MEMORY_NDS_TOUCH_MAPPING_HPP
#define ORACIS_MEMORY_NDS_TOUCH_MAPPING_HPP

#include <cstdint>

namespace oracis::memory {

struct NdsTouchCalibration {
    uint16_t adc_x1 = 0x000u;
    uint16_t adc_y1 = 0x000u;
    uint8_t screen_x1 = 0u;
    uint8_t screen_y1 = 0u;
    uint16_t adc_x2 = 0xFF0u;
    uint16_t adc_y2 = 0xBF0u;
    uint8_t screen_x2 = 255u;
    uint8_t screen_y2 = 191u;
};

struct NdsTouchPoint {
    uint16_t x12 = 0u;
    uint16_t y12 = 0u;
};

namespace detail {

constexpr uint16_t map_axis(int screen, int screen1, int screen2,
                            int adc1, int adc2) noexcept
{
    if (screen2 == screen1) return static_cast<uint16_t>(adc1 & 0x0FFF);
    /* Round to nearest; the numerator keeps the sign of the slope. */
    const long numerator =
        static_cast<long>(screen - screen1) * (adc2 - adc1);
    const long span = screen2 - screen1;
    long adc = adc1 + (numerator * 2 + (numerator >= 0 ? span : -span)) /
                          (span * 2);
    if (adc < 0) adc = 0;
    if (adc > 0x0FFF) adc = 0x0FFF;
    return static_cast<uint16_t>(adc);
}

} // namespace detail

/* Pixels outside the 256x192 screen are clamped to its edges. */
constexpr NdsTouchPoint nds_touch_adc_from_screen(
    int screen_x, int screen_y,
    const NdsTouchCalibration &calibration = {}) noexcept
{
    if (screen_x < 0) screen_x = 0;
    if (screen_x > 255) screen_x = 255;
    if (screen_y < 0) screen_y = 0;
    if (screen_y > 191) screen_y = 191;
    return {
        detail::map_axis(screen_x, calibration.screen_x1,
                         calibration.screen_x2, calibration.adc_x1,
                         calibration.adc_x2),
        detail::map_axis(screen_y, calibration.screen_y1,
                         calibration.screen_y2, calibration.adc_y1,
                         calibration.adc_y2),
    };
}

} // namespace oracis::memory

#endif /* ORACIS_MEMORY_NDS_TOUCH_MAPPING_HPP */

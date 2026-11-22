#include "oracis/memory/nds_3d_rasterizer.hpp"
#include "oracis/memory/nds_display_control.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace oracis::memory {
namespace {

constexpr int64_t kMaximumDepth =
    static_cast<int64_t>(Nds3dRasterizer::MaximumDepth);

int64_t edge_function(
    const NdsGeometryEngine::Vertex &a,
    const NdsGeometryEngine::Vertex &b,
    int64_t sample_x_twice,
    int64_t sample_y_twice) noexcept
{
    const int64_t ax =
        static_cast<int64_t>(a.screen_position[0]) * 2;
    const int64_t ay =
        static_cast<int64_t>(a.screen_position[1]) * 2;
    const int64_t bx =
        static_cast<int64_t>(b.screen_position[0]) * 2;
    const int64_t by =
        static_cast<int64_t>(b.screen_position[1]) * 2;
    return (bx - ax) * (sample_y_twice - ay) -
        (by - ay) * (sample_x_twice - ax);
}

/* Top-left convention for the renderer's Y-down screen coordinate system. */
bool top_left_edge(
    const NdsGeometryEngine::Vertex &a,
    const NdsGeometryEngine::Vertex &b) noexcept
{
    const int64_t dx =
        static_cast<int64_t>(b.screen_position[0]) -
        a.screen_position[0];
    const int64_t dy =
        static_cast<int64_t>(b.screen_position[1]) -
        a.screen_position[1];
    return dy < 0 || (dy == 0 && dx > 0);
}

bool edge_contains(
    int64_t value,
    const NdsGeometryEngine::Vertex &a,
    const NdsGeometryEngine::Vertex &b) noexcept
{
    return value > 0 || (value == 0 && top_left_edge(a, b));
}

uint8_t expand_five_to_six(uint32_t value) noexcept
{
    const uint32_t channel = value & UINT32_C(0x1F);
    return static_cast<uint8_t>(
        channel == 0u ? 0u : channel * 2u + 1u);
}

uint32_t normalized_depth(
    const NdsGeometryEngine::Vertex &vertex) noexcept
{
    const int64_t z = vertex.clip_position[2];
    const int64_t w = vertex.clip_position[3];
    if (w <= 0) return Nds3dRasterizer::MaximumDepth;

    const int64_t numerator = (z + w) * kMaximumDepth;
    const int64_t denominator = 2 * w;
    const int64_t depth = numerator / denominator;
    return static_cast<uint32_t>(std::clamp<int64_t>(
        depth, 0, kMaximumDepth));
}

uint32_t interpolate_depth(
    const std::array<int64_t, 3> &weights,
    const std::array<uint32_t, 3> &depths,
    int64_t area) noexcept
{
    if (area <= 0) return Nds3dRasterizer::MaximumDepth;
    const int64_t value =
        weights[0] * static_cast<int64_t>(depths[0]) +
        weights[1] * static_cast<int64_t>(depths[1]) +
        weights[2] * static_cast<int64_t>(depths[2]);
    return static_cast<uint32_t>(std::clamp<int64_t>(
        value / area, 0, kMaximumDepth));
}

uint32_t interpolate_w_buffer_depth(
    const std::array<const NdsGeometryEngine::Vertex *, 3> &vertices,
    const std::array<int64_t, 3> &weights,
    int64_t area) noexcept
{
    if (area <= 0) return Nds3dRasterizer::MaximumDepth;

    int64_t minimum_w = std::numeric_limits<int64_t>::max();
    for (const auto *vertex : vertices) {
        const int64_t w = vertex->clip_position[3];
        if (w <= 0) return Nds3dRasterizer::MaximumDepth;
        minimum_w = std::min(minimum_w, w);
    }

    /*
     * Reconstruct W from linearly interpolated reciprocal W. The common
     * minimum-W scale keeps the denominator bounded and avoids overflowing
     * the signed 64-bit accumulator for large clip-space coordinates.
     */
    long double reciprocal_sum = 0.0L;
    for (unsigned index = 0u; index < 3u; ++index) {
        const long double reciprocal =
            static_cast<long double>(minimum_w) /
            static_cast<long double>(vertices[index]->clip_position[3]);
        reciprocal_sum +=
            static_cast<long double>(weights[index]) * reciprocal;
    }
    if (!(reciprocal_sum > 0.0L)) {
        return Nds3dRasterizer::MaximumDepth;
    }

    const long double depth =
        (static_cast<long double>(area) *
         static_cast<long double>(minimum_w)) / reciprocal_sum;
    return static_cast<uint32_t>(std::clamp(
        std::round(depth),
        0.0L,
        static_cast<long double>(Nds3dRasterizer::MaximumDepth)));
}

uint16_t interpolate_color_channel(
    const std::array<int64_t, 3> &weights,
    const std::array<uint16_t, 3> &colors,
    unsigned shift,
    int64_t area) noexcept
{
    if (area <= 0) return 0u;
    int64_t value = 0;
    for (unsigned index = 0u; index < 3u; ++index) {
        const int64_t channel =
            (colors[index] >> shift) & UINT16_C(0x1F);
        value += weights[index] * channel;
    }
    return static_cast<uint16_t>(std::clamp<int64_t>(
        value / area, 0, 31));
}

bool perspective_texture_coordinates(
    const std::array<const NdsGeometryEngine::Vertex *, 3> &vertices,
    const std::array<int64_t, 3> &screen_weights,
    int16_t &texture_s,
    int16_t &texture_t) noexcept
{
    int64_t minimum_w = std::numeric_limits<int64_t>::max();
    for (const auto *vertex : vertices) {
        const int64_t w = vertex->clip_position[3];
        if (w <= 0) return false;
        minimum_w = std::min(minimum_w, w);
    }

    /*
     * Use minimum_w / W as a common-scaled reciprocal. Normalizing by the
     * smallest W keeps the weights bounded while preserving the exact
     * perspective-correct ratio:
     *
     *   (sum(lambda * attribute / W)) / (sum(lambda / W)).
     *
     * long double avoids overflow for the full signed 16-bit texture range
     * and also keeps Android and desktop implementations on the same path.
     */
    std::array<long double, 3> weights{};
    long double denominator = 0.0L;
    for (unsigned index = 0u; index < 3u; ++index) {
        const long double reciprocal =
            static_cast<long double>(minimum_w) /
            static_cast<long double>(vertices[index]->clip_position[3]);
        weights[index] =
            static_cast<long double>(screen_weights[index]) * reciprocal;
        denominator += weights[index];
    }
    if (!(denominator > 0.0L)) return false;

    long double s = 0.0L;
    long double t = 0.0L;
    for (unsigned index = 0u; index < 3u; ++index) {
        s += weights[index] * vertices[index]->texture_s;
        t += weights[index] * vertices[index]->texture_t;
    }
    s /= denominator;
    t /= denominator;

    const long double minimum =
        static_cast<long double>(std::numeric_limits<int16_t>::min());
    const long double maximum =
        static_cast<long double>(std::numeric_limits<int16_t>::max());
    texture_s = static_cast<int16_t>(std::clamp(
        std::round(s), minimum, maximum));
    texture_t = static_cast<int16_t>(std::clamp(
        std::round(t), minimum, maximum));
    return true;
}

void shade_fragment(
    uint8_t polygon_mode,
    uint16_t vertex_color,
    uint8_t polygon_alpha,
    const Nds3dTextureSample &texture,
    Nds3dFragment &fragment) noexcept
{
    const uint8_t vertex_r = expand_five_to_six(vertex_color);
    const uint8_t vertex_g = expand_five_to_six(vertex_color >> 5u);
    const uint8_t vertex_b = expand_five_to_six(vertex_color >> 10u);
    const uint8_t vertex_a = expand_five_to_six(polygon_alpha);

    if (texture.status == Nds3dTextureSampleStatus::NoTexture ||
        !texture.has_texture) {
        fragment.red6 = vertex_r;
        fragment.green6 = vertex_g;
        fragment.blue6 = vertex_b;
        fragment.alpha6 = vertex_a;
        fragment.alpha5 = polygon_alpha;
        fragment.textured = false;
        return;
    }

    const uint8_t texture_r = expand_five_to_six(texture.color_rgb555);
    const uint8_t texture_g = expand_five_to_six(
        texture.color_rgb555 >> 5u);
    const uint8_t texture_b = expand_five_to_six(
        texture.color_rgb555 >> 10u);
    const uint8_t texture_a = expand_five_to_six(texture.alpha);

    if (polygon_mode == 0u) {
        const auto modulate = [](
            uint8_t texture_channel, uint8_t vertex_channel) noexcept {
            return static_cast<uint8_t>(
                ((static_cast<uint32_t>(texture_channel) + 1u) *
                 (static_cast<uint32_t>(vertex_channel) + 1u) - 1u) / 64u);
        };
        fragment.red6 = modulate(texture_r, vertex_r);
        fragment.green6 = modulate(texture_g, vertex_g);
        fragment.blue6 = modulate(texture_b, vertex_b);
        fragment.alpha6 = modulate(texture_a, vertex_a);
        fragment.alpha5 = static_cast<uint8_t>(fragment.alpha6 >> 1u);
    } else {
        const auto decal = [texture_a](
            uint8_t texture_channel, uint8_t vertex_channel) noexcept {
            if (texture_a == 0u) return vertex_channel;
            if (texture_a == 63u) return texture_channel;
            return static_cast<uint8_t>(
                (static_cast<uint32_t>(texture_channel) * texture_a +
                 static_cast<uint32_t>(vertex_channel) *
                     (63u - texture_a)) / 64u);
        };
        fragment.red6 = decal(texture_r, vertex_r);
        fragment.green6 = decal(texture_g, vertex_g);
        fragment.blue6 = decal(texture_b, vertex_b);
        fragment.alpha6 = vertex_a;
        fragment.alpha5 = polygon_alpha;
    }
    fragment.textured = true;
}

Nds3dRasterStatus rasterize_triangle_scanline(
    const NdsGeometryEngine::Vertex &first,
    const NdsGeometryEngine::Vertex &second,
    const NdsGeometryEngine::Vertex &third,
    const NdsGeometryEngine::Polygon &polygon,
    const Nds3dTextureSampler &textures,
    uint16_t scanline,
    bool w_buffer,
    Nds3dFragmentCallback callback,
    void *context) noexcept
{
    if (!first.screen_position_valid ||
        !second.screen_position_valid ||
        !third.screen_position_valid ||
        first.clip_position[3] <= 0 ||
        second.clip_position[3] <= 0 ||
        third.clip_position[3] <= 0) {
        return Nds3dRasterStatus::InvalidGeometry;
    }
    if (polygon.line_segment || polygon.alpha == 0u) {
        return Nds3dRasterStatus::WireframeNotSupported;
    }
    if (polygon.polygon_mode > 1u) {
        return Nds3dRasterStatus::UnsupportedPolygonMode;
    }

    const NdsGeometryEngine::Vertex *a = &first;
    const NdsGeometryEngine::Vertex *b = &second;
    const NdsGeometryEngine::Vertex *c = &third;
    int64_t area = edge_function(
        *a, *b,
        static_cast<int64_t>(c->screen_position[0]) * 2,
        static_cast<int64_t>(c->screen_position[1]) * 2);
    if (area == 0) return Nds3dRasterStatus::Ok;
    if (area < 0) {
        std::swap(b, c);
        area = -area;
    }

    const int32_t minimum_x = std::max<int32_t>(
        0, std::min({a->screen_position[0],
                     b->screen_position[0],
                     c->screen_position[0]}));
    const int32_t maximum_x = std::min<int32_t>(
        static_cast<int32_t>(Nds3dRasterizer::ScreenWidth - 1u),
        std::max({a->screen_position[0],
                  b->screen_position[0],
                  c->screen_position[0]}));
    const int32_t minimum_y = std::min({
        a->screen_position[1],
        b->screen_position[1],
        c->screen_position[1]});
    const int32_t maximum_y = std::max({
        a->screen_position[1],
        b->screen_position[1],
        c->screen_position[1]});
    if (static_cast<int32_t>(scanline) < minimum_y ||
        static_cast<int32_t>(scanline) > maximum_y ||
        minimum_x > maximum_x) {
        return Nds3dRasterStatus::Ok;
    }

    const std::array<const NdsGeometryEngine::Vertex *, 3> vertices{
        a, b, c
    };
    const std::array<uint32_t, 3> vertex_depths{
        normalized_depth(*a),
        normalized_depth(*b),
        normalized_depth(*c)
    };
    const std::array<uint16_t, 3> vertex_colors{
        a->color_rgb555, b->color_rgb555, c->color_rgb555
    };

    for (int32_t x = minimum_x; x <= maximum_x; ++x) {
        const int64_t sample_x_twice =
            static_cast<int64_t>(x) * 2 + 1;
        const int64_t sample_y_twice =
            static_cast<int64_t>(scanline) * 2 + 1;
        const std::array<int64_t, 3> weights{
            edge_function(*b, *c, sample_x_twice, sample_y_twice),
            edge_function(*c, *a, sample_x_twice, sample_y_twice),
            edge_function(*a, *b, sample_x_twice, sample_y_twice)
        };

        if (!edge_contains(weights[0], *b, *c) ||
            !edge_contains(weights[1], *c, *a) ||
            !edge_contains(weights[2], *a, *b)) {
            continue;
        }

        int16_t texture_s = 0;
        int16_t texture_t = 0;
        if (!perspective_texture_coordinates(
                vertices, weights, texture_s, texture_t)) {
            return Nds3dRasterStatus::InvalidGeometry;
        }

        const Nds3dTextureSample texture = textures.sample(
            polygon.texture_parameters,
            polygon.texture_palette_base,
            texture_s,
            texture_t);
        if (texture.status != Nds3dTextureSampleStatus::Ok &&
            texture.status != Nds3dTextureSampleStatus::NoTexture) {
            return Nds3dRasterStatus::TextureFailure;
        }

        const uint16_t red = interpolate_color_channel(
            weights, vertex_colors, 0u, area);
        const uint16_t green = interpolate_color_channel(
            weights, vertex_colors, 5u, area);
        const uint16_t blue = interpolate_color_channel(
            weights, vertex_colors, 10u, area);
        const uint16_t interpolated_color = static_cast<uint16_t>(
            red | (green << 5u) | (blue << 10u));

        Nds3dFragment fragment{};
        fragment.x = static_cast<uint8_t>(x);
        fragment.y = static_cast<uint8_t>(scanline);
        fragment.depth24 = w_buffer
            ? interpolate_w_buffer_depth(vertices, weights, area)
            : interpolate_depth(weights, vertex_depths, area);
        fragment.w_buffer = w_buffer;
        fragment.polygon_id = polygon.polygon_id;
        fragment.fog_enabled = polygon.fog_enabled;
        fragment.depth_equal = polygon.depth_equal;
        fragment.translucent_depth_write =
            polygon.translucent_depth_write;
        shade_fragment(
            polygon.polygon_mode,
            interpolated_color,
            polygon.alpha,
            texture,
            fragment);
        /*
         * Discard on the final alpha, after texture-mode blending. In Decal
         * mode, a zero-alpha texel selects the vertex color and vertex alpha;
         * only the resulting fragment alpha determines transparency.
         */
        if (fragment.alpha6 == 0u) continue;
        callback(context, fragment);
    }

    return Nds3dRasterStatus::Ok;
}

} // namespace

namespace {

struct DepthTestedCallbackContext {
    Nds3dDepthBuffer *depth_buffer = nullptr;
    Nds3dFragmentCallback callback = nullptr;
    void *callback_context = nullptr;
};

struct CompositedScanlineContext {
    Nds3dDepthBuffer *depth_buffer = nullptr;
    Nds3dColorBuffer *color_buffer = nullptr;
    bool alpha_blending_enabled = false;
    bool alpha_test_enabled = false;
    uint8_t alpha_test_reference = 0u;
};

void forward_depth_passed_fragment(
    void *opaque,
    const Nds3dFragment &fragment) noexcept
{
    auto *context = static_cast<DepthTestedCallbackContext *>(opaque);
    if (context != nullptr && context->depth_buffer != nullptr &&
        context->callback != nullptr &&
        context->depth_buffer->test_and_update(fragment)) {
        context->callback(context->callback_context, fragment);
    }
}

void composite_depth_passed_fragment(
    void *opaque,
    const Nds3dFragment &fragment) noexcept
{
    auto *context = static_cast<CompositedScanlineContext *>(opaque);
    if (context == nullptr || context->depth_buffer == nullptr ||
        context->color_buffer == nullptr) {
        return;
    }

    if (context->alpha_test_enabled) {
        if (fragment.alpha5 <= context->alpha_test_reference) return;
    } else if (fragment.alpha5 == 0u) {
        return;
    }

    if (context->depth_buffer->test_and_update(fragment)) {
        static_cast<void>(context->color_buffer->blend_fragment(
            fragment, context->alpha_blending_enabled));
    }
}

} // namespace

bool Nds3dColorBuffer::write_pixel(
    uint8_t x,
    uint8_t y,
    const Nds3dColorPixel &pixel) noexcept
{
    if (y >= ScreenHeight ||
        pixel.red6 > 63u || pixel.green6 > 63u ||
        pixel.blue6 > 63u || pixel.alpha5 > 31u ||
        pixel.polygon_id > 63u) {
        return false;
    }
    const std::size_t index =
        static_cast<std::size_t>(y) * ScreenWidth + x;
    pixels_[index] = pixel;
    return true;
}

void Nds3dColorBuffer::clear(
    uint8_t red6,
    uint8_t green6,
    uint8_t blue6,
    uint8_t alpha5,
    uint8_t polygon_id,
    bool fog_enabled) noexcept
{
    const Nds3dColorPixel initial{
        static_cast<uint8_t>(std::min<uint8_t>(red6, 63u)),
        static_cast<uint8_t>(std::min<uint8_t>(green6, 63u)),
        static_cast<uint8_t>(std::min<uint8_t>(blue6, 63u)),
        static_cast<uint8_t>(std::min<uint8_t>(alpha5, 31u)),
        polygon_id,
        fog_enabled
    };
    pixels_.fill(initial);
}

bool Nds3dColorBuffer::blend_fragment(
    const Nds3dFragment &fragment,
    bool alpha_blending_enabled) noexcept
{
    if (fragment.y >= ScreenHeight ||
        fragment.alpha6 == 0u ||
        fragment.alpha5 == 0u ||
        fragment.red6 > 63u ||
        fragment.green6 > 63u ||
        fragment.blue6 > 63u ||
        fragment.alpha5 > 31u) {
        return false;
    }

    const std::size_t index =
        static_cast<std::size_t>(fragment.y) * ScreenWidth + fragment.x;
    Nds3dColorPixel &destination = pixels_[index];

    const uint32_t source_alpha = fragment.alpha5;
    const bool bypass_blending =
        !alpha_blending_enabled ||
        source_alpha == 31u ||
        destination.alpha5 == 0u;

    if (bypass_blending) {
        destination.red6 = fragment.red6;
        destination.green6 = fragment.green6;
        destination.blue6 = fragment.blue6;
    } else {
        const uint32_t source_weight = source_alpha + 1u;
        const uint32_t destination_weight = 31u - source_alpha;
        const auto blend_channel = [source_weight, destination_weight](
            uint8_t source, uint8_t old) noexcept {
            return static_cast<uint8_t>(
                (static_cast<uint32_t>(source) * source_weight +
                 static_cast<uint32_t>(old) * destination_weight) / 32u);
        };
        destination.red6 = blend_channel(fragment.red6, destination.red6);
        destination.green6 = blend_channel(fragment.green6, destination.green6);
        destination.blue6 = blend_channel(fragment.blue6, destination.blue6);
    }

    destination.alpha5 = static_cast<uint8_t>(
        std::max<uint32_t>(source_alpha, destination.alpha5));
    destination.polygon_id = fragment.polygon_id;
    destination.fog_enabled = fragment.fog_enabled;
    return true;
}

const Nds3dColorPixel *Nds3dColorBuffer::pixel_at(
    uint8_t x, uint8_t y) const noexcept
{
    if (y >= ScreenHeight) return nullptr;
    const std::size_t index =
        static_cast<std::size_t>(y) * ScreenWidth + x;
    return &pixels_[index];
}

bool Nds3dColorBuffer::copy_bg0_scanline(
    uint16_t scanline, Nds3dBg0Scanline &output) const noexcept
{
    if (scanline >= ScreenHeight) return false;
    for (uint32_t x = 0u; x < ScreenWidth; ++x) {
        const Nds3dColorPixel &pixel = pixels_[
            static_cast<std::size_t>(scanline) * ScreenWidth + x];
        const uint16_t red5 = static_cast<uint16_t>(pixel.red6 >> 1u);
        const uint16_t green5 = static_cast<uint16_t>(pixel.green6 >> 1u);
        const uint16_t blue5 = static_cast<uint16_t>(pixel.blue6 >> 1u);
        output[x] = {
            static_cast<uint16_t>(
                red5 | (green5 << 5u) | (blue5 << 10u)),
            pixel.alpha5
        };
    }
    return true;
}

void Nds3dDepthBuffer::clear(uint32_t depth) noexcept
{
    depths_.fill(std::min(depth, MaximumDepth));
}

bool Nds3dDepthBuffer::write_depth(
    uint8_t x, uint8_t y, uint32_t depth) noexcept
{
    if (y >= ScreenHeight || depth > MaximumDepth) return false;
    const std::size_t index =
        static_cast<std::size_t>(y) * ScreenWidth + x;
    depths_[index] = depth;
    return true;
}

bool Nds3dDepthBuffer::test_and_update(
    const Nds3dFragment &fragment) noexcept
{
    if (fragment.y >= ScreenHeight ||
        fragment.depth24 > MaximumDepth ||
        fragment.alpha6 == 0u) {
        return false;
    }

    const std::size_t index =
        static_cast<std::size_t>(fragment.y) * ScreenWidth + fragment.x;
    const uint32_t previous = depths_[index];
    bool passes = false;
    if (fragment.depth_equal) {
        const uint32_t difference = previous > fragment.depth24
            ? previous - fragment.depth24
            : fragment.depth24 - previous;
        const uint32_t tolerance = fragment.w_buffer
            ? WDepthEqualTolerance
            : DepthEqualTolerance;
        passes = difference <= tolerance;
    } else {
        passes = fragment.depth24 < previous;
    }
    if (!passes) return false;

    /*
     * Opaque fragments update depth. Translucent fragments do so only when
     * POLYGON_ATTR.Bit11 enables translucent depth writes.
     */
    if (fragment.alpha6 == 63u || fragment.translucent_depth_write) {
        depths_[index] = fragment.depth24;
    }
    return true;
}

uint32_t Nds3dDepthBuffer::depth_at(
    uint8_t x, uint8_t y) const noexcept
{
    if (y >= ScreenHeight) return MaximumDepth;
    const std::size_t index =
        static_cast<std::size_t>(y) * ScreenWidth + x;
    return depths_[index];
}

Nds3dRasterStatus Nds3dRasterizer::render_scanline_depth_tested(
    uint16_t scanline,
    Nds3dDepthBuffer &depth_buffer,
    Nds3dFragmentCallback callback,
    void *context) const noexcept
{
    if (callback == nullptr) return Nds3dRasterStatus::MissingCallback;

    DepthTestedCallbackContext callback_context{
        &depth_buffer, callback, context
    };
    return render_scanline(
        scanline, forward_depth_passed_fragment, &callback_context);
}

Nds3dRasterStatus Nds3dRasterizer::initialize_rear_plane(
    Nds3dColorBuffer &color_buffer,
    Nds3dDepthBuffer &depth_buffer,
    const NdsDisplayControl &display_control) const noexcept
{
    if (!display_control.installed()) {
        return Nds3dRasterStatus::DisplayControlUnavailable;
    }
    const uint32_t clear_color = display_control.three_d_clear_color();
    const uint8_t polygon_id = static_cast<uint8_t>(
        (clear_color >> 24u) & UINT32_C(0x3F));

    if (display_control.three_d_rear_plane_bitmap_enabled()) {
        constexpr uint32_t bitmap_bytes = NdsVramControl::BankSize;
        const uint32_t column_offset =
            display_control.three_d_clear_image_offset() & UINT16_C(0x00FF);
        const uint32_t row_offset =
            (display_control.three_d_clear_image_offset() >> 8u) &
            UINT16_C(0x00FF);
        constexpr std::array<uint8_t, 2> bitmap_slots{{2u, 3u}};

        for (const uint8_t slot : bitmap_slots) {
            const ReadResult first =
                textures_.read_texture_slot_halfword(slot, 0u);
            const ReadResult last =
                textures_.read_texture_slot_halfword(
                    slot, bitmap_bytes - UINT32_C(2));
            if (first.status != Status::Ok || last.status != Status::Ok) {
                return Nds3dRasterStatus::TextureFailure;
            }
        }

        /* Preflight all visible reads before changing either staging buffer. */
        for (uint32_t y = 0u; y < ScreenHeight; ++y) {
            const uint32_t source_row = (row_offset + y) & UINT32_C(0xFF);
            for (uint32_t x = 0u; x < ScreenWidth; ++x) {
                const uint32_t source_column =
                    (column_offset + x) & UINT32_C(0xFF);
                const uint32_t byte_offset =
                    ((source_row * UINT32_C(256)) + source_column) * 2u;
                if (textures_.read_texture_slot_halfword(
                        2u, byte_offset).status != Status::Ok ||
                    textures_.read_texture_slot_halfword(
                        3u, byte_offset).status != Status::Ok) {
                    return Nds3dRasterStatus::TextureFailure;
                }
            }
        }

        for (uint32_t y = 0u; y < ScreenHeight; ++y) {
            const uint32_t source_row = (row_offset + y) & UINT32_C(0xFF);
            for (uint32_t x = 0u; x < ScreenWidth; ++x) {
                const uint32_t source_column =
                    (column_offset + x) & UINT32_C(0xFF);
                const uint32_t byte_offset =
                    ((source_row * UINT32_C(256)) + source_column) * 2u;
                const ReadResult color_word =
                    textures_.read_texture_slot_halfword(2u, byte_offset);
                const ReadResult depth_word =
                    textures_.read_texture_slot_halfword(3u, byte_offset);
                if (color_word.status != Status::Ok ||
                    depth_word.status != Status::Ok) {
                    return Nds3dRasterStatus::TextureFailure;
                }

                const uint16_t color_raw = static_cast<uint16_t>(color_word.value);
                const uint16_t depth_raw = static_cast<uint16_t>(depth_word.value);
                const uint16_t red5 = color_raw & UINT16_C(0x001F);
                const uint16_t green5 = static_cast<uint16_t>(
                    (color_raw >> 5u) & UINT16_C(0x001F));
                const uint16_t blue5 = static_cast<uint16_t>(
                    (color_raw >> 10u) & UINT16_C(0x001F));
                const uint8_t alpha5 =
                    (color_raw & UINT16_C(0x8000)) != 0u ? 31u : 0u;
                const uint16_t depth15 =
                    static_cast<uint16_t>(depth_raw & UINT16_C(0x7FFF));
                const uint32_t depth24 =
                    static_cast<uint32_t>(depth15) * UINT32_C(0x200) +
                    ((static_cast<uint32_t>(depth15) + 1u) /
                        UINT32_C(0x8000)) * UINT32_C(0x1FF);
                const Nds3dColorPixel pixel{
                    expand_five_to_six(red5),
                    expand_five_to_six(green5),
                    expand_five_to_six(blue5),
                    alpha5,
                    polygon_id,
                    (depth_raw & UINT16_C(0x8000)) != 0u
                };
                if (!color_buffer.write_pixel(
                        static_cast<uint8_t>(x),
                        static_cast<uint8_t>(y), pixel) ||
                    !depth_buffer.write_depth(
                        static_cast<uint8_t>(x),
                        static_cast<uint8_t>(y), depth24)) {
                    return Nds3dRasterStatus::InvalidGeometry;
                }
            }
        }
        return Nds3dRasterStatus::Ok;
    }
    const uint16_t red5 = static_cast<uint16_t>(clear_color & UINT32_C(0x1F));
    const uint16_t green5 = static_cast<uint16_t>(
        (clear_color >> 5u) & UINT32_C(0x1F));
    const uint16_t blue5 = static_cast<uint16_t>(
        (clear_color >> 10u) & UINT32_C(0x1F));
    const uint8_t fog = static_cast<uint8_t>(
        (clear_color >> 15u) & UINT32_C(1));
    const uint8_t alpha5 = static_cast<uint8_t>(
        (clear_color >> 16u) & UINT32_C(0x1F));
    color_buffer.clear(
        expand_five_to_six(red5),
        expand_five_to_six(green5),
        expand_five_to_six(blue5),
        alpha5,
        polygon_id,
        fog != 0u);
    depth_buffer.clear(display_control.three_d_clear_depth_expanded24());
    return Nds3dRasterStatus::Ok;
}

Nds3dRasterStatus Nds3dRasterizer::render_scanline_composited(
    uint16_t scanline,
    Nds3dDepthBuffer &depth_buffer,
    Nds3dColorBuffer &color_buffer,
    const NdsDisplayControl &display_control) const noexcept
{
    if (!display_control.installed()) {
        return Nds3dRasterStatus::DisplayControlUnavailable;
    }
    return render_scanline_composited(
        scanline,
        depth_buffer,
        color_buffer,
        display_control.three_d_alpha_blending_enabled(),
        display_control.three_d_alpha_test_enabled(),
        display_control.three_d_alpha_test_reference());
}

Nds3dRasterStatus Nds3dRasterizer::render_scanline_to_cache(
    uint16_t scanline,
    Nds3dDepthBuffer &depth_buffer,
    Nds3dColorBuffer &color_buffer,
    Nds3dScanlineCache &cache,
    const NdsDisplayControl &display_control) const noexcept
{
    if (scanline >= ScreenHeight) return Nds3dRasterStatus::InvalidScanline;

    const Nds3dScanlineCacheStatus available = cache.can_enqueue(scanline);
    if (available == Nds3dScanlineCacheStatus::Full) {
        return Nds3dRasterStatus::ScanlineCacheFull;
    }
    if (available != Nds3dScanlineCacheStatus::Ok) {
        return Nds3dRasterStatus::ScanlineCacheOutOfOrder;
    }

    const Nds3dRasterStatus rendered =
        render_scanline_composited(
            scanline, depth_buffer, color_buffer, display_control);
    if (rendered != Nds3dRasterStatus::Ok) return rendered;

    Nds3dBg0Scanline line{};
    if (!color_buffer.copy_bg0_scanline(scanline, line)) {
        return Nds3dRasterStatus::InvalidScanline;
    }
    const Nds3dScanlineCacheStatus queued = cache.enqueue(scanline, line);
    if (queued == Nds3dScanlineCacheStatus::Full) {
        return Nds3dRasterStatus::ScanlineCacheFull;
    }
    if (queued != Nds3dScanlineCacheStatus::Ok) {
        return Nds3dRasterStatus::ScanlineCacheInvalid;
    }
    return Nds3dRasterStatus::Ok;
}

Nds3dRasterStatus Nds3dRasterizer::render_scanline_composited(
    uint16_t scanline,
    Nds3dDepthBuffer &depth_buffer,
    Nds3dColorBuffer &color_buffer,
    bool alpha_blending_enabled,
    bool alpha_test_enabled,
    uint8_t alpha_test_reference) const noexcept
{
    if (alpha_test_reference > 31u) {
        return Nds3dRasterStatus::InvalidAlphaTestReference;
    }
    if (geometry_.render_polygon_count() == 0u) {
        return Nds3dRasterStatus::Ok;
    }
    if ((geometry_.render_swap_parameters() & UINT8_C(1)) == 0u) {
        /*
         * SWAP_BUFFERS bit 0 selects Y-sorting for translucent polygons.
         * Solid polygons (alpha 31) do not need that ordering: the depth
         * buffer resolves their visibility. Keep rejecting alpha 1..30 until
         * the DS-specific translucent sort is implemented.
         */
        for (uint32_t index = 0u;
             index < geometry_.render_polygon_count(); ++index) {
            const NdsGeometryEngine::Polygon *polygon =
                geometry_.render_polygon_at(index);
            if (polygon == nullptr) return Nds3dRasterStatus::InvalidGeometry;
            if (polygon->alpha > 0u && polygon->alpha < 31u) {
                return Nds3dRasterStatus::TranslucentSortingNotSupported;
            }
        }
    }

    CompositedScanlineContext context{
        &depth_buffer,
        &color_buffer,
        alpha_blending_enabled,
        alpha_test_enabled,
        alpha_test_reference
    };
    return render_scanline(
        scanline, composite_depth_passed_fragment, &context);
}

Nds3dRasterStatus Nds3dRasterizer::render_scanline(
    uint16_t scanline,
    Nds3dFragmentCallback callback,
    void *context) const noexcept
{
    if (scanline >= ScreenHeight) {
        return Nds3dRasterStatus::InvalidScanline;
    }
    if (callback == nullptr) return Nds3dRasterStatus::MissingCallback;

    const uint32_t vertex_count = geometry_.render_vertex_count();
    const uint32_t polygon_count = geometry_.render_polygon_count();
    const bool w_buffer = geometry_.render_uses_w_buffer();

    for (uint32_t polygon_index = 0u;
         polygon_index < polygon_count; ++polygon_index) {
        const NdsGeometryEngine::Polygon *polygon =
            geometry_.render_polygon_at(polygon_index);
        if (polygon == nullptr) return Nds3dRasterStatus::InvalidGeometry;
        if (!polygon->render_enabled || polygon->vertex_count < 3u) continue;

        for (uint8_t triangle = 1u;
             triangle + 1u < polygon->vertex_count; ++triangle) {
            const uint32_t first_index = polygon->vertex_indices[0];
            const uint32_t second_index =
                polygon->vertex_indices[triangle];
            const uint32_t third_index =
                polygon->vertex_indices[triangle + 1u];
            if (first_index >= vertex_count ||
                second_index >= vertex_count ||
                third_index >= vertex_count) {
                return Nds3dRasterStatus::InvalidGeometry;
            }

            const auto *first = geometry_.render_vertex_at(first_index);
            const auto *second = geometry_.render_vertex_at(second_index);
            const auto *third = geometry_.render_vertex_at(third_index);
            if (first == nullptr || second == nullptr || third == nullptr) {
                return Nds3dRasterStatus::InvalidGeometry;
            }

            const Nds3dRasterStatus result =
                rasterize_triangle_scanline(
                    *first, *second, *third, *polygon, textures_,
                    scanline, w_buffer, callback, context);
            if (result != Nds3dRasterStatus::Ok) return result;
        }
    }

    return Nds3dRasterStatus::Ok;
}

} // namespace oracis::memory

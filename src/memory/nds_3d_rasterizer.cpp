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

/*
 * Edge marking flags the pixel footprint crossed by a polygon perimeter
 * segment. edge_function uses doubled sample coordinates, so the L1
 * half-pixel support radius is |dx| + |dy| in this integer domain.
 */
bool pixel_touches_edge_band(
    int64_t value,
    const NdsGeometryEngine::Vertex &a,
    const NdsGeometryEngine::Vertex &b) noexcept
{
    const int64_t dx = static_cast<int64_t>(b.screen_position[0]) -
        a.screen_position[0];
    const int64_t dy = static_cast<int64_t>(b.screen_position[1]) -
        a.screen_position[1];
    const int64_t absolute_value = value < 0 ? -value : value;
    const int64_t threshold = (dx < 0 ? -dx : dx) +
        (dy < 0 ? -dy : dy);
    return absolute_value <= threshold;
}

bool is_translucent_polygon(
    const NdsGeometryEngine::Polygon &polygon) noexcept
{
    const uint32_t texture_format =
        (polygon.texture_parameters >> 26u) & UINT32_C(7);
    return (polygon.alpha > 0u && polygon.alpha < 31u) ||
        texture_format == 1u || texture_format == 6u;
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


uint8_t interpolate_fog_density(
    uint32_t depth24,
    uint16_t fog_offset,
    uint8_t fog_shift,
    const std::array<uint8_t, 32> &density_table) noexcept
{
    const uint32_t depth15 = std::min<uint32_t>(
        depth24 >> 9u, UINT32_C(0x7FFF));
    const uint32_t fog_step = UINT32_C(0x400) >> fog_shift;
    if (fog_step == 0u) {
        return depth15 <= fog_offset ? density_table[0] : density_table[31];
    }

    const uint32_t first_boundary =
        static_cast<uint32_t>(fog_offset) + fog_step;
    if (depth15 <= first_boundary) return density_table[0];

    uint32_t previous_boundary = first_boundary;
    for (uint32_t index = 1u; index < density_table.size(); ++index) {
        const uint32_t boundary =
            static_cast<uint32_t>(fog_offset) + fog_step * (index + 1u);
        if (depth15 <= boundary) {
            const int32_t previous_density = density_table[index - 1u];
            const int32_t next_density = density_table[index];
            const int32_t distance = static_cast<int32_t>(
                depth15 - previous_boundary);
            const int32_t interpolated = previous_density +
                ((next_density - previous_density) * distance) /
                    static_cast<int32_t>(fog_step);
            return static_cast<uint8_t>(std::clamp(interpolated, 0, 127));
        }
        previous_boundary = boundary;
    }
    return density_table[31];
}

void shade_fragment(
    uint8_t polygon_mode,
    uint16_t vertex_color,
    uint8_t polygon_alpha,
    const Nds3dTextureSample &texture,
    const std::array<uint16_t, 32> *toon_table,
    bool highlight_shading,
    Nds3dFragment &fragment) noexcept
{
    const uint8_t vertex_r = expand_five_to_six(vertex_color);
    const uint8_t vertex_g = expand_five_to_six(vertex_color >> 5u);
    const uint8_t vertex_b = expand_five_to_six(vertex_color >> 10u);
    const uint8_t vertex_a = expand_five_to_six(polygon_alpha);

    uint8_t shading_r = vertex_r;
    uint8_t shading_g = vertex_g;
    uint8_t shading_b = vertex_b;
    if (polygon_mode == 2u && toon_table != nullptr) {
        const uint32_t index = vertex_color & UINT16_C(0x001F);
        const uint16_t toon_color = (*toon_table)[index];
        shading_r = expand_five_to_six(toon_color);
        shading_g = expand_five_to_six(toon_color >> 5u);
        shading_b = expand_five_to_six(toon_color >> 10u);
    }

    if (texture.status == Nds3dTextureSampleStatus::NoTexture ||
        !texture.has_texture) {
        fragment.red6 = shading_r;
        fragment.green6 = shading_g;
        fragment.blue6 = shading_b;
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

    const auto modulate = [](
        uint8_t texture_channel, uint8_t shading_channel) noexcept {
        return static_cast<uint8_t>(
            ((static_cast<uint32_t>(texture_channel) + 1u) *
             (static_cast<uint32_t>(shading_channel) + 1u) - 1u) / 64u);
    };

    if (polygon_mode == 0u || polygon_mode == 2u) {
        fragment.red6 = modulate(texture_r, shading_r);
        fragment.green6 = modulate(texture_g, shading_g);
        fragment.blue6 = modulate(texture_b, shading_b);
        if (polygon_mode == 2u && highlight_shading) {
            fragment.red6 = static_cast<uint8_t>(
                std::min<uint32_t>(63u, fragment.red6 + shading_r));
            fragment.green6 = static_cast<uint8_t>(
                std::min<uint32_t>(63u, fragment.green6 + shading_g));
            fragment.blue6 = static_cast<uint8_t>(
                std::min<uint32_t>(63u, fragment.blue6 + shading_b));
        }
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
    const std::array<uint16_t, 32> *toon_table,
    bool highlight_shading,
    bool edge_marking_enabled,
    bool edge_ab,
    bool edge_bc,
    bool edge_ca,
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
    if (polygon.polygon_mode > 3u ||
        (polygon.polygon_mode == 2u && toon_table == nullptr)) {
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
        std::swap(edge_ab, edge_ca);
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

        Nds3dTextureSample texture{};
        if (polygon.polygon_mode == 3u) {
            /* Shadow mode uses the polygon vertex color, not a texture. */
            texture.status = Nds3dTextureSampleStatus::NoTexture;
        } else {
            texture = textures.sample(
                polygon.texture_parameters,
                polygon.texture_palette_base,
                texture_s,
                texture_t);
        }
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
        fragment.polygon_mode = polygon.polygon_mode;
        fragment.fog_enabled = polygon.fog_enabled;
        fragment.depth_equal = polygon.depth_equal;
        fragment.translucent = polygon.alpha < 31u;
        fragment.edge_candidate = edge_marking_enabled &&
            polygon.alpha == 31u && (
                (edge_bc && pixel_touches_edge_band(weights[0], *b, *c)) ||
                (edge_ca && pixel_touches_edge_band(weights[1], *c, *a)) ||
                (edge_ab && pixel_touches_edge_band(weights[2], *a, *b)));
        fragment.translucent_depth_write =
            polygon.translucent_depth_write;
        shade_fragment(
            polygon.polygon_mode,
            interpolated_color,
            polygon.alpha,
            texture,
            toon_table,
            highlight_shading,
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
    Nds3dShadowStencilBuffer *shadow_stencil = nullptr;
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

    if (!context->depth_buffer->test_and_update(fragment)) return;

    if (fragment.polygon_mode == 3u) {
        if (context->shadow_stencil == nullptr) return;
        if (fragment.polygon_id == 0u) {
            /* Pass 1: mark back-facing shadow-volume pixels without drawing. */
            static_cast<void>(context->shadow_stencil->mark(
                fragment.x, fragment.y));
            return;
        }

        /* Pass 2: consume the mask before checking the receiver polygon ID. */
        if (context->shadow_stencil->test_and_clear(
                fragment.x, fragment.y)) {
            return;
        }
        const Nds3dColorPixel *destination = context->color_buffer->pixel_at(
            fragment.x, fragment.y);
        if (destination == nullptr ||
            destination->polygon_id == fragment.polygon_id) {
            return;
        }
    }

    static_cast<void>(context->color_buffer->blend_fragment(
        fragment, context->alpha_blending_enabled));
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
    if (!fragment.translucent) {
        destination.edge_candidate = fragment.edge_candidate;
        destination.edge_polygon_id = fragment.polygon_id;
    }
    destination.fog_enabled = fragment.translucent
        ? (destination.fog_enabled && fragment.fog_enabled)
        : fragment.fog_enabled;
    return true;
}


bool Nds3dColorBuffer::apply_fog_scanline(
    uint16_t scanline,
    const Nds3dDepthBuffer &depth_buffer,
    const NdsDisplayControl &display_control) noexcept
{
    if (scanline >= ScreenHeight || !display_control.installed()) {
        return false;
    }
    if (!display_control.three_d_fog_enabled()) return true;

    const uint32_t fog_color = display_control.three_d_fog_color();
    const uint8_t fog_red = expand_five_to_six(fog_color);
    const uint8_t fog_green = expand_five_to_six(fog_color >> 5u);
    const uint8_t fog_blue = expand_five_to_six(fog_color >> 10u);
    const uint8_t fog_alpha = static_cast<uint8_t>(
        (fog_color >> 16u) & UINT32_C(0x1F));
    const bool alpha_only = display_control.three_d_fog_alpha_only();
    const uint16_t fog_offset = display_control.three_d_fog_offset();
    const uint8_t fog_shift = display_control.three_d_fog_shift();
    const auto &density_table = display_control.three_d_fog_density_table();

    for (uint32_t x = 0u; x < ScreenWidth; ++x) {
        const std::size_t index =
            static_cast<std::size_t>(scanline) * ScreenWidth + x;
        Nds3dColorPixel &pixel = pixels_[index];
        if (!pixel.fog_enabled) continue;

        uint32_t density = interpolate_fog_density(
            depth_buffer.depth_at(static_cast<uint8_t>(x), scanline),
            fog_offset, fog_shift, density_table);
        if (density == 127u) density = 128u;

        const uint32_t inverse_density = 128u - density;
        const auto blend = [density, inverse_density](
            uint8_t fog, uint8_t source) noexcept {
            return static_cast<uint8_t>(
                (static_cast<uint32_t>(fog) * density +
                 static_cast<uint32_t>(source) * inverse_density) / 128u);
        };
        if (!alpha_only) {
            pixel.red6 = blend(fog_red, pixel.red6);
            pixel.green6 = blend(fog_green, pixel.green6);
            pixel.blue6 = blend(fog_blue, pixel.blue6);
        }
        pixel.alpha5 = blend(fog_alpha, pixel.alpha5);
    }
    return true;
}

bool Nds3dColorBuffer::apply_edge_marking_scanline(
    uint16_t scanline,
    const Nds3dDepthBuffer &depth_buffer,
    const std::array<uint16_t, 8> &edge_colors,
    uint8_t rear_plane_polygon_id,
    uint32_t rear_plane_depth) noexcept
{
    if (scanline >= ScreenHeight ||
        rear_plane_polygon_id > 63u ||
        rear_plane_depth > Nds3dDepthBuffer::MaximumDepth) {
        return false;
    }

    const auto edge_visible = [](
        uint8_t edge_id, uint32_t edge_depth,
        uint8_t neighbor_id, uint32_t neighbor_depth) noexcept {
        return edge_id != neighbor_id && edge_depth < neighbor_depth;
    };

    for (uint32_t x = 0u; x < ScreenWidth; ++x) {
        Nds3dColorPixel &pixel = pixels_[
            static_cast<std::size_t>(scanline) * ScreenWidth + x];
        if (!pixel.edge_candidate) continue;

        const uint8_t edge_id = pixel.edge_polygon_id;
        const uint32_t edge_depth = depth_buffer.depth_at(
            static_cast<uint8_t>(x), static_cast<uint8_t>(scanline));
        bool draw_edge = false;
        const auto test_neighbor = [&](int32_t neighbor_x,
                                       int32_t neighbor_y) noexcept {
            if (draw_edge) return;
            if (neighbor_x < 0 ||
                neighbor_x >= static_cast<int32_t>(ScreenWidth) ||
                neighbor_y < 0 ||
                neighbor_y >= static_cast<int32_t>(ScreenHeight)) {
                draw_edge = edge_visible(
                    edge_id, edge_depth,
                    rear_plane_polygon_id, rear_plane_depth);
                return;
            }
            const Nds3dColorPixel &neighbor = pixels_[
                static_cast<std::size_t>(neighbor_y) * ScreenWidth +
                static_cast<uint32_t>(neighbor_x)];
            const uint32_t neighbor_depth = depth_buffer.depth_at(
                static_cast<uint8_t>(neighbor_x),
                static_cast<uint8_t>(neighbor_y));
            draw_edge = edge_visible(
                edge_id, edge_depth, neighbor.polygon_id, neighbor_depth);
        };

        test_neighbor(static_cast<int32_t>(x) - 1, scanline);
        test_neighbor(static_cast<int32_t>(x) + 1, scanline);
        test_neighbor(x, static_cast<int32_t>(scanline) - 1);
        test_neighbor(x, static_cast<int32_t>(scanline) + 1);

        if (draw_edge) {
            const uint16_t color = edge_colors[edge_id >> 3u];
            pixel.red6 = expand_five_to_six(color);
            pixel.green6 = expand_five_to_six(color >> 5u);
            pixel.blue6 = expand_five_to_six(color >> 10u);
        }
        pixel.edge_candidate = false;
    }
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

void Nds3dShadowStencilBuffer::clear() noexcept
{
    bits_.fill(0u);
}

bool Nds3dShadowStencilBuffer::mark(uint8_t x, uint8_t y) noexcept
{
    if (y >= ScreenHeight) return false;
    bits_[static_cast<std::size_t>(y) * ScreenWidth + x] = 1u;
    return true;
}

bool Nds3dShadowStencilBuffer::is_marked(
    uint8_t x, uint8_t y) const noexcept
{
    if (y >= ScreenHeight) return false;
    return bits_[static_cast<std::size_t>(y) * ScreenWidth + x] != 0u;
}

bool Nds3dShadowStencilBuffer::test_and_clear(
    uint8_t x, uint8_t y) noexcept
{
    if (y >= ScreenHeight) return false;
    const std::size_t index =
        static_cast<std::size_t>(y) * ScreenWidth + x;
    const bool marked = bits_[index] != 0u;
    bits_[index] = 0u;
    return marked;
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
    const Nds3dRasterStatus status = render_scanline_composited_impl(
        scanline, depth_buffer, color_buffer,
        display_control.three_d_alpha_blending_enabled(),
        display_control.three_d_alpha_test_enabled(),
        display_control.three_d_alpha_test_reference(),
        &display_control.three_d_toon_table(),
        display_control.three_d_highlight_shading_enabled(),
        display_control.three_d_edge_marking_enabled(), nullptr);
    if (status != Nds3dRasterStatus::Ok) return status;
    return color_buffer.apply_fog_scanline(
            scanline, depth_buffer, display_control)
        ? Nds3dRasterStatus::Ok
        : Nds3dRasterStatus::InvalidScanline;
}

Nds3dRasterStatus Nds3dRasterizer::render_scanline_composited(
    uint16_t scanline,
    Nds3dDepthBuffer &depth_buffer,
    Nds3dColorBuffer &color_buffer,
    const NdsDisplayControl &display_control,
    Nds3dShadowStencilBuffer &shadow_stencil) const noexcept
{
    if (!display_control.installed()) {
        return Nds3dRasterStatus::DisplayControlUnavailable;
    }
    const Nds3dRasterStatus status = render_scanline_composited_impl(
        scanline, depth_buffer, color_buffer,
        display_control.three_d_alpha_blending_enabled(),
        display_control.three_d_alpha_test_enabled(),
        display_control.three_d_alpha_test_reference(),
        &display_control.three_d_toon_table(),
        display_control.three_d_highlight_shading_enabled(),
        display_control.three_d_edge_marking_enabled(), &shadow_stencil);
    if (status != Nds3dRasterStatus::Ok) return status;
    return color_buffer.apply_fog_scanline(
            scanline, depth_buffer, display_control)
        ? Nds3dRasterStatus::Ok
        : Nds3dRasterStatus::InvalidScanline;
}

Nds3dRasterStatus Nds3dRasterizer::render_scanline_to_cache(
    uint16_t scanline,
    Nds3dDepthBuffer &depth_buffer,
    Nds3dColorBuffer &color_buffer,
    Nds3dScanlineCache &cache,
    const NdsDisplayControl &display_control,
    bool edge_marking_pipeline,
    Nds3dShadowStencilBuffer *shadow_stencil) const noexcept
{
    if (!display_control.installed()) {
        return Nds3dRasterStatus::DisplayControlUnavailable;
    }

    if (!edge_marking_pipeline) {
        if (scanline >= ScreenHeight) {
            return Nds3dRasterStatus::InvalidScanline;
        }
        const Nds3dScanlineCacheStatus available =
            cache.can_enqueue(scanline);
        if (available == Nds3dScanlineCacheStatus::Full) {
            return Nds3dRasterStatus::ScanlineCacheFull;
        }
        if (available != Nds3dScanlineCacheStatus::Ok) {
            return Nds3dRasterStatus::ScanlineCacheOutOfOrder;
        }

        const Nds3dRasterStatus rendered =
            render_scanline_composited_impl(
                scanline, depth_buffer, color_buffer,
                display_control.three_d_alpha_blending_enabled(),
                display_control.three_d_alpha_test_enabled(),
                display_control.three_d_alpha_test_reference(),
                &display_control.three_d_toon_table(),
                display_control.three_d_highlight_shading_enabled(), false,
                shadow_stencil);
        if (rendered != Nds3dRasterStatus::Ok) return rendered;
        if (!color_buffer.apply_fog_scanline(
                scanline, depth_buffer, display_control)) {
            return Nds3dRasterStatus::InvalidScanline;
        }

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

    if (scanline > ScreenHeight) {
        return Nds3dRasterStatus::InvalidScanline;
    }
    const uint16_t output_scanline = scanline == 0u
        ? 0u
        : (scanline == ScreenHeight
            ? static_cast<uint16_t>(ScreenHeight - 1u)
            : static_cast<uint16_t>(scanline - 1u));
    const Nds3dScanlineCacheStatus available =
        cache.can_enqueue(output_scanline);
    if (available == Nds3dScanlineCacheStatus::Full) {
        return Nds3dRasterStatus::ScanlineCacheFull;
    }
    if (available != Nds3dScanlineCacheStatus::Ok) {
        return Nds3dRasterStatus::ScanlineCacheOutOfOrder;
    }

    if (scanline < ScreenHeight) {
        const Nds3dRasterStatus rendered =
            render_scanline_composited_impl(
                scanline, depth_buffer, color_buffer,
                display_control.three_d_alpha_blending_enabled(),
                display_control.three_d_alpha_test_enabled(),
                display_control.three_d_alpha_test_reference(),
                &display_control.three_d_toon_table(),
                display_control.three_d_highlight_shading_enabled(), true,
                shadow_stencil);
        if (rendered != Nds3dRasterStatus::Ok) return rendered;
        /* The first line is a top-neighbor prerequisite, not a queued line. */
        if (scanline == 0u) return Nds3dRasterStatus::Ok;
    }

    const uint32_t rear_plane_depth =
        display_control.three_d_clear_depth_expanded24();
    const uint8_t rear_plane_polygon_id = static_cast<uint8_t>(
        (display_control.three_d_clear_color() >> 24u) & UINT32_C(0x3F));
    if (!color_buffer.apply_edge_marking_scanline(
            output_scanline, depth_buffer,
            display_control.three_d_edge_color_table(),
            rear_plane_polygon_id, rear_plane_depth)) {
        return Nds3dRasterStatus::InvalidScanline;
    }
    if (!color_buffer.apply_fog_scanline(
            output_scanline, depth_buffer, display_control)) {
        return Nds3dRasterStatus::InvalidScanline;
    }

    Nds3dBg0Scanline line{};
    if (!color_buffer.copy_bg0_scanline(output_scanline, line)) {
        return Nds3dRasterStatus::InvalidScanline;
    }
    const Nds3dScanlineCacheStatus queued =
        cache.enqueue(output_scanline, line);
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
    return render_scanline_composited_impl(
        scanline, depth_buffer, color_buffer, alpha_blending_enabled,
        alpha_test_enabled, alpha_test_reference, nullptr, false, false,
        nullptr);
}

Nds3dRasterStatus Nds3dRasterizer::render_scanline_composited_impl(
    uint16_t scanline,
    Nds3dDepthBuffer &depth_buffer,
    Nds3dColorBuffer &color_buffer,
    bool alpha_blending_enabled,
    bool alpha_test_enabled,
    uint8_t alpha_test_reference,
    const std::array<uint16_t, 32> *toon_table,
    bool highlight_shading,
    bool edge_marking_enabled,
    Nds3dShadowStencilBuffer *shadow_stencil) const noexcept
{
    if (alpha_test_reference > 31u) {
        return Nds3dRasterStatus::InvalidAlphaTestReference;
    }
    if (geometry_.render_polygon_count() == 0u) {
        return Nds3dRasterStatus::Ok;
    }

    bool has_shadow_polygons = false;
    bool shadow_render_pass_started = false;
    for (uint32_t index = 0u;
         index < geometry_.render_polygon_count(); ++index) {
        const NdsGeometryEngine::Polygon *polygon =
            geometry_.render_polygon_at(index);
        if (polygon == nullptr) return Nds3dRasterStatus::InvalidGeometry;
        if (polygon->polygon_mode != 3u) continue;
        has_shadow_polygons = true;
        const bool back_render =
            (polygon->attributes & (UINT32_C(1) << 6u)) != 0u;
        const bool front_render =
            (polygon->attributes & (UINT32_C(1) << 7u)) != 0u;
        if (polygon->alpha == 0u || polygon->alpha == 31u ||
            polygon->line_segment || !back_render ||
            (polygon->polygon_id == 0u && front_render) ||
            (polygon->polygon_id != 0u && !front_render)) {
            return Nds3dRasterStatus::InvalidShadowPolygon;
        }
        if (polygon->polygon_id == 0u) {
            if (shadow_render_pass_started) {
                return Nds3dRasterStatus::ShadowPassOrderInvalid;
            }
        } else {
            shadow_render_pass_started = true;
        }
    }
    if (has_shadow_polygons && shadow_stencil == nullptr) {
        return Nds3dRasterStatus::ShadowStencilUnavailable;
    }
    const uint32_t polygon_count = geometry_.render_polygon_count();
    if (polygon_count > NdsGeometryEngine::PolygonRamCapacity) {
        return Nds3dRasterStatus::InvalidGeometry;
    }

    std::array<uint32_t, NdsGeometryEngine::PolygonRamCapacity> manual_order{};
    const uint32_t *polygon_order = nullptr;
    const bool manual_translucent_order =
        (geometry_.render_swap_parameters() & UINT8_C(1)) != 0u;
    if (manual_translucent_order) {
        /*
         * The DS always renders opaque polygons first. In manual mode the
         * caller controls only the relative order within the translucent
         * group; preserve that submission order exactly (including Shadow
         * mask/render pairs). A3I5 and A5I3 textures belong to this group
         * even when POLYGON_ATTR alpha is 31.
         */
        uint32_t opaque_count = 0u;
        uint32_t translucent_count = 0u;
        for (uint32_t index = 0u; index < polygon_count; ++index) {
            const NdsGeometryEngine::Polygon *polygon =
                geometry_.render_polygon_at(index);
            if (polygon == nullptr) return Nds3dRasterStatus::InvalidGeometry;
            if (!is_translucent_polygon(*polygon)) {
                manual_order[opaque_count++] = index;
            }
        }
        translucent_count = opaque_count;
        for (uint32_t index = 0u; index < polygon_count; ++index) {
            const NdsGeometryEngine::Polygon *polygon =
                geometry_.render_polygon_at(index);
            if (polygon == nullptr) return Nds3dRasterStatus::InvalidGeometry;
            if (is_translucent_polygon(*polygon)) {
                manual_order[translucent_count++] = index;
            }
        }
        if (translucent_count != polygon_count) {
            return Nds3dRasterStatus::InvalidGeometry;
        }
        polygon_order = manual_order.data();
    } else {
        /* Automatic DS Y-sort has its own ordering rule. Do not replace it
         * with submission order or a guessed depth sort. */
        for (uint32_t index = 0u; index < polygon_count; ++index) {
            const NdsGeometryEngine::Polygon *polygon =
                geometry_.render_polygon_at(index);
            if (polygon == nullptr) return Nds3dRasterStatus::InvalidGeometry;
            if (is_translucent_polygon(*polygon)) {
                return Nds3dRasterStatus::TranslucentSortingNotSupported;
            }
        }
    }

    CompositedScanlineContext context{
        &depth_buffer,
        &color_buffer,
        shadow_stencil,
        alpha_blending_enabled,
        alpha_test_enabled,
        alpha_test_reference
    };
    return render_scanline_impl(
        scanline, composite_depth_passed_fragment, &context,
        toon_table, highlight_shading, edge_marking_enabled, polygon_order);
}

Nds3dRasterStatus Nds3dRasterizer::render_scanline(
    uint16_t scanline,
    Nds3dFragmentCallback callback,
    void *context) const noexcept
{
    return render_scanline_impl(
        scanline, callback, context, nullptr, false, false, nullptr);
}

Nds3dRasterStatus Nds3dRasterizer::render_scanline_impl(
    uint16_t scanline,
    Nds3dFragmentCallback callback,
    void *context,
    const std::array<uint16_t, 32> *toon_table,
    bool highlight_shading,
    bool edge_marking_enabled,
    const uint32_t *polygon_order) const noexcept
{
    if (scanline >= ScreenHeight) {
        return Nds3dRasterStatus::InvalidScanline;
    }
    if (callback == nullptr) return Nds3dRasterStatus::MissingCallback;

    const uint32_t vertex_count = geometry_.render_vertex_count();
    const uint32_t polygon_count = geometry_.render_polygon_count();
    const bool w_buffer = geometry_.render_uses_w_buffer();

    for (uint32_t polygon_sequence = 0u;
         polygon_sequence < polygon_count; ++polygon_sequence) {
        const uint32_t polygon_index = polygon_order == nullptr
            ? polygon_sequence : polygon_order[polygon_sequence];
        if (polygon_index >= polygon_count) {
            return Nds3dRasterStatus::InvalidGeometry;
        }
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

            const bool edge_ab = triangle == 1u;
            const bool edge_bc = true;
            const bool edge_ca =
                triangle + 1u == static_cast<uint8_t>(polygon->vertex_count - 1u);
            const Nds3dRasterStatus result =
                rasterize_triangle_scanline(
                    *first, *second, *third, *polygon, textures_,
                    scanline, w_buffer, toon_table, highlight_shading,
                    edge_marking_enabled, edge_ab, edge_bc, edge_ca,
                    callback, context);
            if (result != Nds3dRasterStatus::Ok) return result;
        }
    }

    return Nds3dRasterStatus::Ok;
}

} // namespace oracis::memory

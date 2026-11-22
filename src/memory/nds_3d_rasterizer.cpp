#include "oracis/memory/nds_3d_rasterizer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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
    if (polygon.line_segment) {
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
        fragment.depth24 = interpolate_depth(
            weights, vertex_depths, area);
        fragment.polygon_id = polygon.polygon_id;
        fragment.fog_enabled = polygon.fog_enabled;
        shade_fragment(
            polygon.polygon_mode,
            interpolated_color,
            polygon.alpha,
            texture,
            fragment);
        callback(context, fragment);
    }

    return Nds3dRasterStatus::Ok;
}

} // namespace

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
                    scanline, callback, context);
            if (result != Nds3dRasterStatus::Ok) return result;
        }
    }

    return Nds3dRasterStatus::Ok;
}

} // namespace oracis::memory

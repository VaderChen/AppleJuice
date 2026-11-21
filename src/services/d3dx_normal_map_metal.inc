R"metal(
#include <metal_stdlib>
using namespace metal;

struct NormalMapOptions {
    uint flags;
    uint channel;
    float amplitude;
    uint expand_red;
};

static uint normal_map_edge(int index, int limit, bool mirror)
{
    return mirror ? uint(clamp(index, 0, limit - 1)) : uint((index + limit) % limit);
}

static float normal_map_height(float4 pixel, uint channel)
{
    switch (channel) {
        case 1: return pixel.r;
        case 2: return pixel.b;
        case 4: return pixel.g;
        case 8: return pixel.a;
        default: return dot(pixel.rgb, float3(0.2125f, 0.7154f, 0.0721f));
    }
}

kernel void normal_map(texture2d<float, access::read> source [[texture(0)]],
                       texture2d<float, access::write> target [[texture(1)]],
                       constant NormalMapOptions& options [[buffer(0)]],
                       uint2 point [[thread_position_in_grid]])
{
    const uint width = target.get_width();
    const uint height = target.get_height();
    if (point.x >= width || point.y >= height) return;

    float3 heights[3];
    for (int row = 0; row < 3; ++row) {
        const uint y = normal_map_edge(int(point.y) + row - 1, int(height), options.flags & 0x20000u);
        for (int column = 0; column < 3; ++column) {
            const uint x = normal_map_edge(int(point.x) + column - 1, int(width), options.flags & 0x10000u);
            float4 pixel = source.read(uint2(x, y));
            if (options.expand_red) pixel.rgb = pixel.rrr;
            heights[row][column] = normal_map_height(pixel, options.channel);
        }
    }

    // 先平均三組差分，再除以兩個像素的間距。
    const float3 delta_x = float3(heights[0].x - heights[0].z,
                                  heights[1].x - heights[1].z,
                                  heights[2].x - heights[2].z);
    const float3 delta_y = heights[0] - heights[2];
    const float slope = options.amplitude / 6.0f;
    float3 normal = normalize(float3(dot(delta_x, float3(slope)),
                                     dot(delta_y, float3(slope)), 1.0f));
    if (options.flags & 0x80000u) normal = -normal;

    float alpha = 1.0f;
    if (options.flags & 0x100000u) {
        const float center = heights[1].y;
        float sum = 0.0f;
        for (int row = 0; row < 3; ++row)
            sum += dot(max(heights[row] - center, 0.0f), float3(1.0f));
        const float delta = max(sum * (0.125f * options.amplitude), 0.0f);
        alpha = 1.0f - delta * rsqrt(1.0f + delta * delta);
    }
    target.write(float4(normal * 0.5f + 0.5f, alpha), point);
}
)metal"

R"metal(
#include <metal_stdlib>
using namespace metal;

struct Projection {
    uint edge;
    uint order;
    uint tiles_x;
    uint tiles_y;
    float normalization[36];
};

static float3 cube_direction(uint face, float u, float v)
{
    switch (face) {
        case 0: return float3(1, -v, -u);
        case 1: return float3(-1, -v, u);
        case 2: return float3(u, 1, v);
        case 3: return float3(u, -1, -v);
        case 4: return float3(u, -v, 1);
        default: return float3(-u, -v, -1);
    }
}

static float3 sum_simd(float3 value)
{
    return float3(simd_sum(value.x), simd_sum(value.y), simd_sum(value.z));
}

kernel void project_tiles(texture2d_array<float, access::read> cube [[texture(0)]],
                          constant Projection& options [[buffer(0)]],
                          device float4* partial [[buffer(1)]],
                          uint3 tile [[threadgroup_position_in_grid]],
                          uint3 local [[thread_position_in_threadgroup]],
                          uint lane [[thread_index_in_simdgroup]],
                          uint wave [[simdgroup_index_in_threadgroup]],
                          uint simd_width [[threads_per_simdgroup]])
{
    float3 coefficients[37] = {};
    const uint count = options.order * options.order + 1;
    const uint2 grid = uint2(options.tiles_x, options.tiles_y) * uint2(8, 4);
    const uint2 start = tile.xy * uint2(8, 4) + local.xy;
    for (uint y = start.y; y < options.edge; y += grid.y) {
        for (uint x = start.x; x < options.edge; x += grid.x) {
            const float2 uv = (float2(x, y) * 2.0f + 1.0f) / float(options.edge) - 1.0f;
            const float radius2 = 1.0f + dot(uv, uv);
            const float weight = 4.0f * rsqrt(radius2) / radius2;
            const float3 direction = cube_direction(tile.z, uv.x, uv.y) * rsqrt(radius2);
            const float3 integrand = cube.read(uint2(x, y), tile.z).rgb * weight;
            coefficients[count - 1].x += weight;
            // 關聯 Legendre 多項式與 (x+iy)^m 的遞迴，係數順序為 l(l+1)+m。
            float sectoral = 1.0f;
            float2 phase(1.0f, 0.0f);
            for (uint m = 0; m < options.order; ++m) {
                float polynomial = sectoral;
                float previous = 0.0f;
                for (uint l = m; l < options.order; ++l) {
                    const uint center = l * (l + 1);
                    const float3 term = integrand * (options.normalization[l * 6 + m] * polynomial);
                    coefficients[center + m] += term * phase.x;
                    if (m) coefficients[center - m] += term * phase.y;
                    const float next = (float(2 * l + 1) * direction.z * polynomial -
                                        float(l + m) * previous) / float(l + 1 - m);
                    previous = polynomial;
                    polynomial = next;
                }
                sectoral *= -float(2 * m + 1);
                phase = float2(phase.x * direction.x - phase.y * direction.y,
                               phase.x * direction.y + phase.y * direction.x);
            }
        }
    }
    threadgroup packed_float3 subtotals[37][32];
    for (uint i = 0; i < count; ++i) {
        const float3 sum = sum_simd(coefficients[i]);
        if (!lane) subtotals[i][wave] = packed_float3(sum);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    const uint tile_index = (tile.z * options.tiles_y + tile.y) * options.tiles_x + tile.x;
    const uint tiles = options.tiles_x * options.tiles_y * 6;
    const uint waves = (32 + simd_width - 1) / simd_width;
    for (uint i = local.y * 8 + local.x; i < count; i += 32) {
        float3 sum(0.0f);
        for (uint w = 0; w < waves; ++w) sum += float3(subtotals[i][w]);
        partial[i * tiles + tile_index] = float4(sum, 0.0f);
    }
}

kernel void reduce_tiles(constant Projection& options [[buffer(0)]],
                         device const float4* partial [[buffer(1)]],
                         device float4* coefficients [[buffer(2)]],
                         uint index [[threadgroup_position_in_grid]],
                         uint thread_index [[thread_index_in_threadgroup]],
                         uint lane [[thread_index_in_simdgroup]],
                         uint wave [[simdgroup_index_in_threadgroup]],
                         uint simd_width [[threads_per_simdgroup]])
{
    const uint tiles = options.tiles_x * options.tiles_y * 6;
    float3 sum(0.0f);
    for (uint t = thread_index; t < tiles; t += 64) sum += partial[index * tiles + t].rgb;
    sum = sum_simd(sum);
    threadgroup packed_float3 subtotals[64];
    if (!lane) subtotals[wave] = packed_float3(sum);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (!thread_index) {
        sum = float3(0.0f);
        const uint waves = (64 + simd_width - 1) / simd_width;
        for (uint w = 0; w < waves; ++w) sum += float3(subtotals[w]);
        coefficients[index] = float4(sum, 0.0f);
    }
}
)metal"

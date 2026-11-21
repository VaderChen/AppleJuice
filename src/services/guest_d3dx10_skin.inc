#include "d3dx10_skin_abi.h"

#pragma GCC push_options
#pragma GCC target("sse2", "fpmath=sse")
#include <xmmintrin.h>

typedef struct SkinInfluence {
    UINT vertex;
    float weight;
} SkinInfluence;

typedef struct SkinBone {
    SkinInfluence *influences;
    UINT count, capacity;
} SkinBone;

typedef struct SkinInfo {
    D3dx10Skin iface;
    LONG references;
    HANDLE heap;
    SRWLOCK lock;
    SkinBone *bones;
    UINT *vertex_influences;
    UINT bone_count, bone_capacity, vertex_count, vertex_capacity;
} SkinInfo;

typedef struct SkinLock {
    SRWLOCK *lock;
    BOOL exclusive;
} SkinLock;

static SkinLock skin_lock(SkinInfo *skin, BOOL exclusive)
{
    if (exclusive) AcquireSRWLockExclusive(&skin->lock);
    else AcquireSRWLockShared(&skin->lock);
    return (SkinLock){ &skin->lock, exclusive };
}

static void skin_unlock(SkinLock *guard)
{
    if (guard->exclusive) ReleaseSRWLockExclusive(guard->lock);
    else ReleaseSRWLockShared(guard->lock);
}

#define SKIN_GUARD(exclusive) \
    SkinInfo *skin = (SkinInfo *)iface; \
    SkinLock lock __attribute__((cleanup(skin_unlock))) = skin_lock(skin, exclusive)

static void *skin_array(HANDLE heap, SIZE_T count, SIZE_T element)
{
    return count && count <= SIZE_MAX / element ? HeapAlloc(heap, HEAP_ZERO_MEMORY, count * element) : NULL;
}

static void skin_free(HANDLE heap, void *pointer)
{
    if (pointer) HeapFree(heap, 0, pointer);
}

static void *skin_reserve(HANDLE heap, void *old, UINT *capacity, UINT needed, SIZE_T element)
{
    if (needed <= *capacity) return old;
    if (needed > SIZE_MAX / element) return NULL;
    const UINT increment = *capacity / 2 + 8;
    UINT target = *capacity <= UINT_MAX - increment ? *capacity + increment : needed;
    if (target < needed || target > SIZE_MAX / element) target = needed;
    void *memory = old ? HeapReAlloc(heap, HEAP_ZERO_MEMORY, old, (SIZE_T)target * element) :
            skin_array(heap, target, element);
    if (memory) *capacity = target;
    return memory;
}

static void skin_free_bones(HANDLE heap, SkinBone *bones, UINT count)
{
    if (!bones) return;
    for (UINT b = 0; b < count; ++b) skin_free(heap, bones[b].influences);
    skin_free(heap, bones);
}

static ULONG WINAPI skin_addref(D3dx10Skin *iface)
{
    return (ULONG)InterlockedIncrement(&((SkinInfo *)iface)->references);
}

static ULONG WINAPI skin_release(D3dx10Skin *iface)
{
    SkinInfo *skin = (SkinInfo *)iface;
    const ULONG count = (ULONG)InterlockedDecrement(&skin->references);
    if (!count) {
        skin_free_bones(skin->heap, skin->bones, skin->bone_count);
        skin_free(skin->heap, skin->vertex_influences);
        skin_free(skin->heap, skin);
    }
    return count;
}

static HRESULT WINAPI skin_query(D3dx10Skin *iface, const GUID *iid, void **output)
{
    static const GUID unknown = {0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
    static const GUID skin_info = {0x420bd604,0x1c76,0x4a34,{0xa4,0x66,0xe4,0x5d,0x06,0x58,0xa3,0x2c}};
    if (!output) return E_POINTER;
    *output = NULL;
    if (!iid) return E_INVALIDARG;
    if (!same_guid(iid, &unknown) && !same_guid(iid, &skin_info)) return E_NOINTERFACE;
    skin_addref(iface);
    *output = iface;
    return S_OK;
}

static UINT WINAPI skin_num_vertices(D3dx10Skin *iface)
{
    SKIN_GUARD(FALSE);
    return skin->vertex_count;
}

static UINT WINAPI skin_num_bones(D3dx10Skin *iface)
{
    SKIN_GUARD(FALSE);
    return skin->bone_count;
}

static UINT WINAPI skin_max_influences(D3dx10Skin *iface)
{
    SKIN_GUARD(FALSE);
    UINT maximum = 0;
    for (UINT v = 0; v < skin->vertex_count; ++v)
        if (skin->vertex_influences[v] > maximum) maximum = skin->vertex_influences[v];
    return maximum;
}

static HRESULT WINAPI skin_add_vertices(D3dx10Skin *iface, UINT count)
{
    SKIN_GUARD(TRUE);
    if (!count) return S_OK;
    if (count > UINT_MAX - skin->vertex_count) return E_INVALIDARG;
    const UINT total = skin->vertex_count + count;
    UINT *indices = skin_reserve(skin->heap, skin->vertex_influences, &skin->vertex_capacity, total, sizeof(*indices));
    if (!indices) return E_OUTOFMEMORY;
    skin->vertex_influences = indices;
    skin->vertex_count = total;
    return S_OK;
}

static HRESULT WINAPI skin_add_bones(D3dx10Skin *iface, UINT count)
{
    SKIN_GUARD(TRUE);
    if (!count) return S_OK;
    if (count > UINT_MAX - skin->bone_count) return E_INVALIDARG;
    const UINT total = skin->bone_count + count;
    SkinBone *bones = skin_reserve(skin->heap, skin->bones, &skin->bone_capacity, total, sizeof(*bones));
    if (!bones) return E_OUTOFMEMORY;
    skin->bones = bones;
    skin->bone_count = total;
    return S_OK;
}

static void skin_clear_bone(SkinInfo *skin, SkinBone *bone)
{
    for (UINT i = 0; i < bone->count; ++i) --skin->vertex_influences[bone->influences[i].vertex];
    skin_free(skin->heap, bone->influences);
    *bone = (SkinBone){0};
}

static HRESULT WINAPI skin_clear_influences(D3dx10Skin *iface, UINT index)
{
    SKIN_GUARD(TRUE);
    if (index >= skin->bone_count) return E_INVALIDARG;
    skin_clear_bone(skin, &skin->bones[index]);
    return S_OK;
}

static HRESULT WINAPI skin_remove_bone(D3dx10Skin *iface, UINT index)
{
    SKIN_GUARD(TRUE);
    if (index >= skin->bone_count) return E_INVALIDARG;
    skin_clear_bone(skin, &skin->bones[index]);
    --skin->bone_count;
    for (UINT i = index; i < skin->bone_count; ++i) skin->bones[i] = skin->bones[i + 1];
    skin->bones[skin->bone_count] = (SkinBone){0};
    return S_OK;
}

static void skin_replace(SkinInfo *skin, SkinBone *bones, UINT bone_count, UINT *vertices, UINT vertex_count)
{
    skin_free_bones(skin->heap, skin->bones, skin->bone_count);
    skin_free(skin->heap, skin->vertex_influences);
    skin->bones = bones;
    skin->bone_count = skin->bone_capacity = bone_count;
    skin->vertex_influences = vertices;
    skin->vertex_count = skin->vertex_capacity = vertex_count;
}

static HRESULT WINAPI skin_remap_bones(D3dx10Skin *iface, UINT count, const UINT *map)
{
    SKIN_GUARD(TRUE);
    if (count && !map) return E_INVALIDARG;
    for (UINT i = 0; i < count; ++i) if (map[i] >= skin->bone_count) return E_INVALIDARG;

    SkinBone *bones = skin_array(skin->heap, count, sizeof(*bones));
    UINT *vertices = skin_array(skin->heap, skin->vertex_count, sizeof(*vertices));
    if ((count && !bones) || (skin->vertex_count && !vertices)) goto oom;
    for (UINT i = 0; i < count; ++i) {
        const SkinBone *source = &skin->bones[map[i]];
        SkinBone *target = &bones[i];
        target->influences = skin_array(skin->heap, source->count, sizeof(*target->influences));
        if (source->count && !target->influences) goto oom;
        target->count = target->capacity = source->count;
        for (UINT j = 0; j < source->count; ++j) {
            target->influences[j] = source->influences[j];
            ++vertices[source->influences[j].vertex];
        }
    }
    skin_replace(skin, bones, count, vertices, skin->vertex_count);
    return S_OK;

oom:
    skin_free_bones(skin->heap, bones, count);
    skin_free(skin->heap, vertices);
    return E_OUTOFMEMORY;
}

static HRESULT WINAPI skin_remap_vertices(D3dx10Skin *iface, UINT count, const UINT *map)
{
    SKIN_GUARD(TRUE);
    if (count && !map) return E_INVALIDARG;
    for (UINT i = 0; i < count; ++i) if (map[i] >= skin->vertex_count) return E_INVALIDARG;

    // 每個新索引指向一個原頂點。倒排串列保留一個原頂點被複製到多個新索引的情況。
    UINT *heads = skin_array(skin->heap, skin->vertex_count, sizeof(*heads));
    UINT *next = skin_array(skin->heap, count, sizeof(*next));
    UINT *vertices = skin_array(skin->heap, count, sizeof(*vertices));
    SkinBone *bones = skin_array(skin->heap, skin->bone_count, sizeof(*bones));
    if ((skin->vertex_count && !heads) || (count && (!next || !vertices)) || (skin->bone_count && !bones)) goto oom;
    for (UINT i = count; i; --i) {
        next[i - 1] = heads[map[i - 1]];
        heads[map[i - 1]] = i;
    }
    for (UINT b = 0; b < skin->bone_count; ++b) {
        const SkinBone *source = &skin->bones[b];
        SkinBone *target = &bones[b];
        UINT influences = 0;
        for (UINT i = 0; i < source->count; ++i)
            for (UINT v = heads[source->influences[i].vertex]; v; v = next[v - 1]) ++influences;
        target->influences = skin_array(skin->heap, influences, sizeof(*target->influences));
        if (influences && !target->influences) goto oom;
        target->capacity = influences;
        for (UINT i = 0; i < source->count; ++i) {
            const SkinInfluence *old = &source->influences[i];
            for (UINT v = heads[old->vertex]; v; v = next[v - 1]) {
                target->influences[target->count++] = (SkinInfluence){ v - 1, old->weight };
                ++vertices[v - 1];
            }
        }
    }
    skin_free(skin->heap, heads);
    skin_free(skin->heap, next);
    skin_replace(skin, bones, skin->bone_count, vertices, count);
    return S_OK;

oom:
    skin_free(skin->heap, heads);
    skin_free(skin->heap, next);
    skin_free(skin->heap, vertices);
    skin_free_bones(skin->heap, bones, skin->bone_count);
    return E_OUTOFMEMORY;
}

static BOOL skin_valid_weight(float weight)
{
    return weight >= 0 && weight <= 1;
}

static HRESULT WINAPI skin_add_influences(D3dx10Skin *iface, UINT index, UINT count, const UINT *vertices, const float *weights)
{
    SKIN_GUARD(TRUE);
    if (index >= skin->bone_count || (count && (!vertices || !weights))) return E_INVALIDARG;
    if (!count) return S_OK;
    for (UINT i = 0; i < count; ++i)
        if (vertices[i] >= skin->vertex_count || !skin_valid_weight(weights[i])) return E_INVALIDARG;

    SkinBone *bone = &skin->bones[index];
    UINT *slots = skin_array(skin->heap, skin->vertex_count, sizeof(*slots));
    if (!slots) return E_OUTOFMEMORY;
    for (UINT i = 0; i < bone->count; ++i) slots[bone->influences[i].vertex] = i + 1;
    UINT total = bone->count;
    for (UINT i = 0; i < count; ++i)
        if (!slots[vertices[i]]) slots[vertices[i]] = ++total;
    SkinInfluence *influences = skin_reserve(skin->heap, bone->influences, &bone->capacity, total, sizeof(*influences));
    if (!influences) {
        skin_free(skin->heap, slots);
        return E_OUTOFMEMORY;
    }
    bone->influences = influences;
    for (UINT i = 0; i < count; ++i) {
        const UINT vertex = vertices[i], slot = slots[vertex] - 1;
        if (slot >= bone->count) {
            ++bone->count;
            ++skin->vertex_influences[vertex];
        }
        influences[slot] = (SkinInfluence){ vertex, weights[i] };
    }
    skin_free(skin->heap, slots);
    return S_OK;
}

static UINT WINAPI skin_influence_count(D3dx10Skin *iface, UINT index)
{
    SKIN_GUARD(FALSE);
    return index < skin->bone_count ? skin->bones[index].count : 0;
}

static HRESULT WINAPI skin_get_influences(D3dx10Skin *iface, UINT index, UINT offset, UINT count, UINT *vertices, float *weights)
{
    SKIN_GUARD(FALSE);
    if (index >= skin->bone_count || (count && (!vertices || !weights))) return E_INVALIDARG;
    const SkinBone *bone = &skin->bones[index];
    if (offset > bone->count || count > bone->count - offset) return E_INVALIDARG;
    for (UINT i = 0; i < count; ++i) {
        vertices[i] = bone->influences[offset + i].vertex;
        weights[i] = bone->influences[offset + i].weight;
    }
    return S_OK;
}

static HRESULT WINAPI skin_find_influence(D3dx10Skin *iface, UINT index, UINT vertex, UINT *result)
{
    SKIN_GUARD(FALSE);
    if (!result || index >= skin->bone_count || vertex >= skin->vertex_count) return E_INVALIDARG;
    const SkinBone *bone = &skin->bones[index];
    for (UINT i = 0; i < bone->count; ++i) if (bone->influences[i].vertex == vertex) {
        *result = i;
        return S_OK;
    }
    return E_INVALIDARG;
}

static HRESULT WINAPI skin_set_influence(D3dx10Skin *iface, UINT index, UINT influence, float weight)
{
    SKIN_GUARD(TRUE);
    if (index >= skin->bone_count || influence >= skin->bones[index].count || !skin_valid_weight(weight))
        return E_INVALIDARG;
    skin->bones[index].influences[influence].weight = weight;
    return S_OK;
}

static HRESULT WINAPI skin_get_influence(D3dx10Skin *iface, UINT index, UINT influence, float *weight)
{
    SKIN_GUARD(FALSE);
    if (!weight || index >= skin->bone_count || influence >= skin->bones[index].count) return E_INVALIDARG;
    *weight = skin->bones[index].influences[influence].weight;
    return S_OK;
}

typedef struct SkinRank {
    SkinInfluence *influence;
    UINT bone;
} SkinRank;

static BOOL skin_rank_worse(const SkinRank *a, const SkinRank *b)
{
    const float aw = a->influence->weight, bw = b->influence->weight;
    return aw == bw ? a->bone > b->bone : aw < bw;
}

static void skin_sift_rank(SkinRank *ranks, UINT count, UINT root)
{
    const SkinRank item = ranks[root];
    while (root < count / 2) {
        UINT child = 2 * root + 1;
        if (child + 1 < count && skin_rank_worse(&ranks[child + 1], &ranks[child])) ++child;
        if (!skin_rank_worse(&ranks[child], &item)) break;
        ranks[root] = ranks[child];
        root = child;
    }
    ranks[root] = item;
}

static void skin_sort_ranks(SkinRank *ranks, UINT count)
{
    for (UINT parent = count / 2; parent; --parent) skin_sift_rank(ranks, count, parent - 1);
    for (UINT end = count; end > 1; --end) {
        const SkinRank last = ranks[end - 1];
        ranks[end - 1] = ranks[0];
        ranks[0] = last;
        skin_sift_rank(ranks, end - 1, 0);
    }
}

static HRESULT WINAPI skin_compact(D3dx10Skin *iface, UINT max_influences, UINT scale_mode, float minimum)
{
    SKIN_GUARD(TRUE);
    if (scale_mode > 2 || !skin_valid_weight(minimum)) return E_INVALIDARG;
    SIZE_T total = 0;
    for (UINT v = 0; v < skin->vertex_count; ++v) {
        if (skin->vertex_influences[v] > SIZE_MAX - total) return E_OUTOFMEMORY;
        total += skin->vertex_influences[v];
    }
    if (!total) return S_OK;
    SIZE_T *ends = skin_array(skin->heap, skin->vertex_count, sizeof(*ends));
    SkinRank *ranks = skin_array(skin->heap, total, sizeof(*ranks));
    if (!ends || !ranks) {
        skin_free(skin->heap, ends);
        skin_free(skin->heap, ranks);
        return E_OUTOFMEMORY;
    }
    SIZE_T base = 0;
    for (UINT v = 0; v < skin->vertex_count; ++v) {
        ends[v] = base;
        base += skin->vertex_influences[v];
    }
    for (UINT b = 0; b < skin->bone_count; ++b) {
        SkinBone *bone = &skin->bones[b];
        for (UINT i = 0; i < bone->count; ++i) {
            SkinInfluence *influence = &bone->influences[i];
            ranks[ends[influence->vertex]++] = (SkinRank){ influence, b };
        }
    }
    for (UINT v = 0; v < skin->vertex_count; ++v) {
        const UINT count = skin->vertex_influences[v];
        if (!count) continue;
        SkinRank *list = ranks + ends[v] - count;
        if (max_influences && count > max_influences) skin_sort_ranks(list, count);
        double before = 0, after = 0;
        for (UINT i = 0; i < count; ++i) {
            float *weight = &list[i].influence->weight;
            before += *weight;
            if (i < max_influences && *weight >= minimum) after += *weight;
            else *weight = -1;
        }
        if (!scale_mode || !after) continue;
        const double target = scale_mode == 1 ? 1 : before;
        for (UINT i = 0; i < count; ++i) {
            float *weight = &list[i].influence->weight;
            if (*weight >= 0) *weight = (float)((double)*weight * target / after);
        }
    }
    for (UINT b = 0; b < skin->bone_count; ++b) {
        SkinBone *bone = &skin->bones[b];
        UINT kept = 0;
        for (UINT i = 0; i < bone->count; ++i) {
            const SkinInfluence item = bone->influences[i];
            if (item.weight >= 0) bone->influences[kept++] = item;
            else --skin->vertex_influences[item.vertex];
        }
        bone->count = kept;
    }
    skin_free(skin->heap, ranks);
    skin_free(skin->heap, ends);
    return S_OK;
}

typedef struct SkinChannelValue { float components[4]; } SkinChannelValue;

static BOOL skin_buffer_range(const void *buffer, UINT start, UINT count, UINT stride)
{
    const uint64_t end = ((uint64_t)start + count) * stride;
    return buffer && end <= SIZE_MAX && (uintptr_t)buffer <= UINTPTR_MAX - (SIZE_T)end;
}

static HRESULT WINAPI skin_software(D3dx10Skin *iface, UINT start, UINT count, const void *source, UINT src_stride,
        void *destination, UINT dst_stride, const D3dx10SkinMatrix *transforms, const D3dx10SkinMatrix *normal_transforms,
        const D3dx10SkinChannel *channels, UINT channel_count)
{
    SKIN_GUARD(FALSE);
    if (start > skin->vertex_count || count > skin->vertex_count - start) return E_INVALIDARG;
    if (!count || !channel_count) return S_OK;
    if (!channels || (skin->bone_count && !transforms) ||
        !skin_buffer_range(source, start, count, src_stride) || !skin_buffer_range(destination, 0, count, dst_stride) ||
        src_stride < 12 || dst_stride < 12) return E_INVALIDARG;
    for (UINT c = 0; c < channel_count; ++c)
        if (channels[c].source_offset > src_stride - 12 || channels[c].destination_offset > dst_stride - 12)
            return E_INVALIDARG;
    if (channel_count > SIZE_MAX / count) return E_OUTOFMEMORY;
    SkinChannelValue *values = skin_array(skin->heap, (SIZE_T)count * channel_count, sizeof(*values));
    BYTE *skinned = skin_array(skin->heap, count, sizeof(*skinned));
    if (!values || !skinned) {
        skin_free(skin->heap, values);
        skin_free(skin->heap, skinned);
        return E_OUTOFMEMORY;
    }

    for (UINT b = 0; b < skin->bone_count; ++b) {
        const SkinBone *bone = &skin->bones[b];
        for (UINT i = 0; i < bone->count; ++i) {
            const SkinInfluence influence = bone->influences[i];
            if (influence.vertex < start || influence.vertex - start >= count || influence.weight == 0) continue;
            const UINT output = influence.vertex - start;
            const BYTE *vertex = (const BYTE *)source + (SIZE_T)influence.vertex * src_stride;
            skinned[output] = 1;
            for (UINT c = 0; c < channel_count; ++c) {
                const D3dx10SkinChannel *channel = &channels[c];
                const D3dx10SkinMatrix *matrix = channel->normal && normal_transforms ? &normal_transforms[b] : &transforms[b];
                float point[3];
                __builtin_memcpy(point, vertex + channel->source_offset, sizeof(point));
                // 四個輸出分量共用 SSE2 指令，resident AOT 轉為 ARM64 向量。在平移除外的法線通道保留向量長度。
                const __m128 x = _mm_mul_ps(_mm_set1_ps(point[0]), _mm_loadu_ps(matrix->m[0]));
                const __m128 y = _mm_mul_ps(_mm_set1_ps(point[1]), _mm_loadu_ps(matrix->m[1]));
                const __m128 z = _mm_mul_ps(_mm_set1_ps(point[2]), _mm_loadu_ps(matrix->m[2]));
                __m128 transformed = _mm_add_ps(_mm_add_ps(x, y), z);
                if (!channel->normal) transformed = _mm_add_ps(transformed, _mm_loadu_ps(matrix->m[3]));
                float *accumulated = values[(SIZE_T)output * channel_count + c].components;
                const __m128 weighted = _mm_mul_ps(transformed, _mm_set1_ps(influence.weight));
                _mm_storeu_ps(accumulated, _mm_add_ps(_mm_loadu_ps(accumulated), weighted));
            }
        }
    }
    // 先完成來源快照與混合，支援來源／目的緩衝重疊及不同的通道配置。
    for (UINT v = 0; v < count; ++v) if (!skinned[v]) {
        const BYTE *input = (const BYTE *)source + (SIZE_T)(start + v) * src_stride;
        for (UINT c = 0; c < channel_count; ++c)
            __builtin_memcpy(values[(SIZE_T)v * channel_count + c].components, input + channels[c].source_offset, 12);
    }
    for (UINT v = 0; v < count; ++v) {
        BYTE *output = (BYTE *)destination + (SIZE_T)v * dst_stride;
        for (UINT c = 0; c < channel_count; ++c)
            __builtin_memcpy(output + channels[c].destination_offset, values[(SIZE_T)v * channel_count + c].components, 12);
    }
    skin_free(skin->heap, values);
    skin_free(skin->heap, skinned);
    return S_OK;
}

#undef SKIN_GUARD

static const D3dx10SkinVtbl skin_vtable = {
    .QueryInterface = skin_query,
    .AddRef = skin_addref,
    .Release = skin_release,
    .GetNumVertices = skin_num_vertices,
    .GetNumBones = skin_num_bones,
    .GetMaxBoneInfluences = skin_max_influences,
    .AddVertices = skin_add_vertices,
    .RemapVertices = skin_remap_vertices,
    .AddBones = skin_add_bones,
    .RemoveBone = skin_remove_bone,
    .RemapBones = skin_remap_bones,
    .AddBoneInfluences = skin_add_influences,
    .ClearBoneInfluences = skin_clear_influences,
    .GetBoneInfluenceCount = skin_influence_count,
    .GetBoneInfluences = skin_get_influences,
    .FindBoneInfluenceIndex = skin_find_influence,
    .SetBoneInfluence = skin_set_influence,
    .GetBoneInfluence = skin_get_influence,
    .Compact = skin_compact,
    .DoSoftwareSkinning = skin_software,
};

__declspec(dllexport) HRESULT WINAPI D3DX10CreateSkinInfo(D3dx10Skin **output)
{
    if (!output) return E_INVALIDARG;
    *output = NULL;
    HANDLE heap = GetProcessHeap();
    SkinInfo *skin = HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(*skin));
    if (!skin) return E_OUTOFMEMORY;
    skin->iface.lpVtbl = &skin_vtable;
    skin->references = 1;
    skin->heap = heap;
    *output = &skin->iface;
    return S_OK;
}

#pragma GCC pop_options

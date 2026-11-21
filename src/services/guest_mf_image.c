/* 所有入口均為 Windows ABI；純運算、逐列複製由 resident AOT 降為 ARM64。 */
#include "mf_image_layout.h"
#include "mf_fp16_layout.h"
#include <emmintrin.h>
#if defined(__i386__)
#define MF_CALL __attribute__((stdcall))
#else
#define MF_CALL
#endif
#define MF_EXPORT __declspec(dllexport) MfImageWord MF_CALL
MF_EXPORT MFGetStrideForBitmapInfoHeader(MfImageWord format,MfImageWord width,MfImageLong* result){
    if(!result)return MF_IMAGE_POINTER;return mf_image_stride(format,width,result);
}
MF_EXPORT MFGetPlaneSize(MfImageWord format,MfImageWord width,MfImageWord height,MfImageWord* result){
    if(!result)return MF_IMAGE_POINTER;return mf_image_size(format,width,height,0,result);
}
MF_EXPORT MFCalculateImageSize(const MfImageByte* subtype,MfImageWord width,MfImageWord height,MfImageWord* result){
    if(!result)return MF_IMAGE_POINTER;*result=0;if(!subtype)return MF_IMAGE_POINTER;
    return mf_image_size(mf_image_guid_format(subtype),width,height,1,result);
}
MF_EXPORT MFCopyImage(MfImageByte* dest,MfImageLong dest_stride,const MfImageByte* src,MfImageLong src_stride,MfImageWord width,MfImageWord rows){
    if(!rows || !width)return 0;if(!dest || !src)return MF_IMAGE_POINTER;
    for(MfImageWord y=0;y<rows;++y){
        for(MfImageWord x=0;x<width;++x)dest[x]=src[x];
        if(y+1<rows){dest+=dest_stride;src+=src_stride;}
    }
    return 0;
}

typedef struct __attribute__((packed, may_alias)) { uint16_t bits; } HalfBits;
typedef struct __attribute__((packed, may_alias)) { uint32_t bits; } FloatBits;

__attribute__((target("sse2")))
static uint32_t expand_vectors(FloatBits* destination, const HalfBits* source, uint32_t count) {
    uint32_t index = 0;
    for (; count - index >= 4; index += 4) {
        const __m128i half = _mm_unpacklo_epi16(_mm_loadl_epi64((const __m128i*)(source + index)),
                                               _mm_setzero_si128());
        const __m128i exponent = _mm_and_si128(half, _mm_set1_epi32(0x7c00));
        const __m128i special = _mm_or_si128(_mm_cmpeq_epi32(exponent, _mm_setzero_si128()),
                                             _mm_cmpeq_epi32(exponent, _mm_set1_epi32(0x7c00)));
        if (_mm_movemask_epi8(special)) {
            for (unsigned lane = 0; lane < 4; ++lane)
                destination[index + lane].bits = mf_fp16_expand(source[index + lane].bits);
            continue;
        }
        const __m128i sign = _mm_slli_epi32(_mm_and_si128(half, _mm_set1_epi32(0x8000)), 16);
        const __m128i magnitude = _mm_slli_epi32(_mm_and_si128(half, _mm_set1_epi32(0x7fff)), 13);
        const __m128i value = _mm_add_epi32(magnitude, _mm_set1_epi32(0x38000000));
        _mm_storeu_si128((__m128i*)(destination + index), _mm_or_si128(sign, value));
    }
    return index;
}

__attribute__((target("sse2")))
static uint32_t narrow_vectors(HalfBits* destination, const FloatBits* source, uint32_t count) {
    uint32_t index = 0;
    for (; count - index >= 4; index += 4) {
        const __m128i value = _mm_loadu_si128((const __m128i*)(source + index));
        const __m128i magnitude = _mm_and_si128(value, _mm_set1_epi32(0x7fffffff));
        const __m128i normal = _mm_and_si128(_mm_cmpgt_epi32(magnitude, _mm_set1_epi32(0x387fffff)),
                                             _mm_cmpgt_epi32(_mm_set1_epi32(0x47800000), magnitude));
        if (_mm_movemask_epi8(normal) != 0xffff) {
            for (unsigned lane = 0; lane < 4; ++lane)
                destination[index + lane].bits = mf_fp16_narrow(source[index + lane].bits);
            continue;
        }
        const __m128i sign = _mm_and_si128(_mm_srli_epi32(value, 16), _mm_set1_epi32(0x8000));
        const __m128i tie = _mm_and_si128(_mm_srli_epi32(magnitude, 13), _mm_set1_epi32(1));
        const __m128i rounded = _mm_add_epi32(magnitude, _mm_add_epi32(_mm_set1_epi32(0xfff), tie));
        const __m128i shifted = _mm_srli_epi32(_mm_sub_epi32(rounded, _mm_set1_epi32(0x38000000)), 13);
        const __m128i signed_words = _mm_srai_epi32(_mm_slli_epi32(_mm_or_si128(sign, shifted), 16), 16);
        _mm_storel_epi64((__m128i*)(destination + index), _mm_packs_epi32(signed_words, signed_words));
    }
    return index;
}

MF_EXPORT MFConvertFromFP16Array(void* output, const void* input, MfImageWord count) {
    MfFp16Plan plan;
    const MfImageWord status = mf_fp16_plan((uintptr_t)output, (uintptr_t)input, count, UINTPTR_MAX, 1, &plan);
    if (status || !count) return status;

    FloatBits* destination = output;
    const HalfBits* source = input;
    if (plan.overlaps) {
        for (uint32_t index = 0; index < plan.split; ++index)
            destination[index].bits = mf_fp16_expand(source[index].bits);
        for (uint32_t index = count; index > plan.split; --index)
            destination[index - 1].bits = mf_fp16_expand(source[index - 1].bits);
    } else {
        for (uint32_t index = expand_vectors(destination, source, count); index < count; ++index)
            destination[index].bits = mf_fp16_expand(source[index].bits);
    }
    return 0;
}

MF_EXPORT MFConvertToFP16Array(void* output, const void* input, MfImageWord count) {
    MfFp16Plan plan;
    const MfImageWord status = mf_fp16_plan((uintptr_t)output, (uintptr_t)input, count, UINTPTR_MAX, 0, &plan);
    if (status || !count) return status;

    HalfBits* destination = output;
    const FloatBits* source = input;
    for (uint32_t index = plan.split; index > 0; --index)
        destination[index - 1].bits = mf_fp16_narrow(source[index - 1].bits);
    const uint32_t first = plan.overlaps ? plan.split : narrow_vectors(destination, source, count);
    for (uint32_t index = first; index < count; ++index)
        destination[index].bits = mf_fp16_narrow(source[index].bits);
    return 0;
}

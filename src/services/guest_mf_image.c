/* 所有入口均為 Windows ABI；純運算、逐列複製由 resident AOT 降為 ARM64。 */
#include "mf_image_layout.h"
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

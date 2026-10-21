#pragma once
#import <Metal/Metal.h>
#include <memory>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <stdexcept>

// 每個繪圖後端只保留最近一個共用影格 binding。owner 保持 mmap 存活，
// 不以裸位址猜測生命週期，避免 unmap 後相同 VA 被重用時讀到舊頁面。
class MetalSharedFrame {
    std::shared_ptr<void> owner_;
    id<MTLBuffer> buffer_;
    id<MTLTexture> texture_;
    void* bytes_{};
    std::size_t length_{};
    std::uint32_t stride_{},width_{},height_{};
    MTLPixelFormat format_{};
    std::uint64_t reused_{},buffers_{},textures_{};
public:
    ~MetalSharedFrame(){
        if(std::getenv("X64ARM_SHARED_FRAME_TRACE"))std::fprintf(stderr,"[metal-shared-frame] reuse=%llu buffers=%llu textures=%llu\n",static_cast<unsigned long long>(reused_),static_cast<unsigned long long>(buffers_),static_cast<unsigned long long>(textures_));
    }
    id<MTLBuffer> buffer(id<MTLDevice> device,void* bytes,std::size_t length,const std::shared_ptr<void>& owner){
        if(owner && owner_==owner && bytes_==bytes && length_==length && buffer_){++reused_;return buffer_;}
        auto result=[device newBufferWithBytesNoCopy:bytes length:length options:MTLResourceStorageModeShared deallocator:nil];
        if(!result)throw std::runtime_error("Metal 共用影格 buffer 建立失敗");++buffers_;
        // 呼叫者在解除影格鎖前等待 GPU 完成；丟棄舊 binding 時沒有仍在讀取的命令。
        texture_=nil;buffer_=nil;owner_.reset();bytes_=nullptr;length_=0;
        if(owner){owner_=owner;bytes_=bytes;length_=length;buffer_=result;}
        return result;
    }
    id<MTLTexture> texture(id<MTLDevice> device,void* bytes,std::size_t length,std::uint32_t stride,
                          MTLPixelFormat format,std::uint32_t width,std::uint32_t height,const std::shared_ptr<void>& owner){
        const auto input=buffer(device,bytes,length,owner);
        if(owner && texture_ && stride_==stride && format_==format && width_==width && height_==height)return texture_;
        auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
        descriptor.storageMode=MTLStorageModeShared;descriptor.usage=MTLTextureUsageShaderRead;
        auto result=[input newTextureWithDescriptor:descriptor offset:0 bytesPerRow:stride];
        if(!result)throw std::runtime_error("Metal 共用影格紋理建立失敗");++textures_;
        if(owner){texture_=result;stride_=stride;format_=format;width_=width;height_=height;}
        return result;
    }
};

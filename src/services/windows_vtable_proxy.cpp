// COM 介面的 *_Proxy 匯出（windowscodecs 等）：在 Windows 上是呼叫介面方法的薄包裝。此處直接跳到物件 vtable 的方法位址，
// 呼叫者放好的堆疊與暫存器（x86 stdcall 的雙槽 double、x64 的 XMM 浮點參數）原封不動，由方法本身返回與清理堆疊，
// 因此本家族與 WIC 的內建物件或程式自行實作的介面都相容。
#include "x64arm/windows_services.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <string_view>

namespace x64arm {
namespace {
// (DLL, 匯出名稱, vtable 索引（含 IUnknown 三個方法）, x86 引數槽數（含 this，double 佔兩槽）)。
constexpr std::uint32_t avi_bad_handle=0x8004406cU;
enum class AviConversion { none, file_info, stream_info, create_stream };
struct ProxyEntry {
    std::string_view dll,name;
    unsigned index,slots;
    std::uint32_t null_result=0x80004003U;
    AviConversion conversion=AviConversion::none;
};
constexpr ProxyEntry entries[]{
    {"avifil32.dll","AVIFileAddRef",1,1,0},
    {"avifil32.dll","AVIFileRelease",2,1,0},
    {"avifil32.dll","AVIFileInfoW",3,3,avi_bad_handle},
    {"avifil32.dll","AVIFileGetStream",4,4,avi_bad_handle},
    {"avifil32.dll","AVIFileCreateStreamW",5,3,avi_bad_handle},
    {"avifil32.dll","AVIFileWriteData",6,4,avi_bad_handle},
    {"avifil32.dll","AVIFileReadData",7,4,avi_bad_handle},
    {"avifil32.dll","AVIFileEndRecord",8,1,avi_bad_handle},
    {"avifil32.dll","AVIStreamAddRef",1,1,0},
    {"avifil32.dll","AVIStreamRelease",2,1,0},
    {"avifil32.dll","AVIStreamInfoW",4,3,avi_bad_handle},
    {"avifil32.dll","AVIStreamFindSample",5,3,UINT32_MAX},
    {"avifil32.dll","AVIStreamReadFormat",6,4,avi_bad_handle},
    {"avifil32.dll","AVIStreamSetFormat",7,4,avi_bad_handle},
    {"avifil32.dll","AVIStreamRead",8,7,avi_bad_handle},
    {"avifil32.dll","AVIStreamWrite",9,8,avi_bad_handle},
    {"avifil32.dll","AVIStreamReadData",11,4,avi_bad_handle},
    {"avifil32.dll","AVIStreamWriteData",12,4,avi_bad_handle},
    {"avifil32.dll","AVIStreamGetFrame",3,2,0},
    {"avifil32.dll","AVIStreamGetFrameClose",2,1,0},
    {"avifil32.dll","AVIFileInfo",3,3,avi_bad_handle,AviConversion::file_info},
    {"avifil32.dll","AVIFileInfoA",3,3,avi_bad_handle,AviConversion::file_info},
    {"avifil32.dll","AVIStreamInfo",4,3,avi_bad_handle,AviConversion::stream_info},
    {"avifil32.dll","AVIStreamInfoA",4,3,avi_bad_handle,AviConversion::stream_info},
    {"avifil32.dll","AVIFileCreateStream",5,3,avi_bad_handle,AviConversion::create_stream},
    {"avifil32.dll","AVIFileCreateStreamA",5,3,avi_bad_handle,AviConversion::create_stream},
    {"windowscodecs.dll","IPropertyBag2_Write_Proxy",4,4},
    {"windowscodecs.dll","IWICBitmapClipper_Initialize_Proxy",8,3},
    {"windowscodecs.dll","IWICBitmapCodecInfo_DoesSupportAnimation_Proxy",18,2},
    {"windowscodecs.dll","IWICBitmapCodecInfo_DoesSupportLossless_Proxy",20,2},
    {"windowscodecs.dll","IWICBitmapCodecInfo_DoesSupportMultiframe_Proxy",21,2},
    {"windowscodecs.dll","IWICBitmapCodecInfo_GetContainerFormat_Proxy",11,2},
    {"windowscodecs.dll","IWICBitmapCodecInfo_GetDeviceManufacturer_Proxy",14,4},
    {"windowscodecs.dll","IWICBitmapCodecInfo_GetDeviceModels_Proxy",15,4},
    {"windowscodecs.dll","IWICBitmapCodecInfo_GetFileExtensions_Proxy",17,4},
    {"windowscodecs.dll","IWICBitmapCodecInfo_GetMimeTypes_Proxy",16,4},
    {"windowscodecs.dll","IWICBitmapDecoder_CopyPalette_Proxy",7,2},
    {"windowscodecs.dll","IWICBitmapDecoder_GetColorContexts_Proxy",10,4},
    {"windowscodecs.dll","IWICBitmapDecoder_GetDecoderInfo_Proxy",6,2},
    {"windowscodecs.dll","IWICBitmapDecoder_GetFrameCount_Proxy",12,2},
    {"windowscodecs.dll","IWICBitmapDecoder_GetFrame_Proxy",13,3},
    {"windowscodecs.dll","IWICBitmapDecoder_GetMetadataQueryReader_Proxy",8,2},
    {"windowscodecs.dll","IWICBitmapDecoder_GetPreview_Proxy",9,2},
    {"windowscodecs.dll","IWICBitmapDecoder_GetThumbnail_Proxy",11,2},
    {"windowscodecs.dll","IWICBitmapEncoder_Commit_Proxy",11,1},
    {"windowscodecs.dll","IWICBitmapEncoder_CreateNewFrame_Proxy",10,3},
    {"windowscodecs.dll","IWICBitmapEncoder_GetEncoderInfo_Proxy",5,2},
    {"windowscodecs.dll","IWICBitmapEncoder_GetMetadataQueryWriter_Proxy",12,2},
    {"windowscodecs.dll","IWICBitmapEncoder_Initialize_Proxy",3,3},
    {"windowscodecs.dll","IWICBitmapEncoder_SetPalette_Proxy",7,2},
    {"windowscodecs.dll","IWICBitmapEncoder_SetThumbnail_Proxy",8,2},
    {"windowscodecs.dll","IWICBitmapFlipRotator_Initialize_Proxy",8,3},
    {"windowscodecs.dll","IWICBitmapFrameDecode_GetColorContexts_Proxy",9,4},
    {"windowscodecs.dll","IWICBitmapFrameDecode_GetMetadataQueryReader_Proxy",8,2},
    {"windowscodecs.dll","IWICBitmapFrameDecode_GetThumbnail_Proxy",10,2},
    {"windowscodecs.dll","IWICBitmapFrameEncode_Commit_Proxy",12,1},
    {"windowscodecs.dll","IWICBitmapFrameEncode_GetMetadataQueryWriter_Proxy",13,2},
    {"windowscodecs.dll","IWICBitmapFrameEncode_Initialize_Proxy",3,2},
    {"windowscodecs.dll","IWICBitmapFrameEncode_SetColorContexts_Proxy",7,3},
    {"windowscodecs.dll","IWICBitmapFrameEncode_SetResolution_Proxy",5,5},
    {"windowscodecs.dll","IWICBitmapFrameEncode_SetSize_Proxy",4,3},
    {"windowscodecs.dll","IWICBitmapFrameEncode_SetThumbnail_Proxy",9,2},
    {"windowscodecs.dll","IWICBitmapFrameEncode_WriteSource_Proxy",11,3},
    {"windowscodecs.dll","IWICBitmapLock_GetDataPointer_STA_Proxy",5,3},
    {"windowscodecs.dll","IWICBitmapLock_GetStride_Proxy",4,2},
    {"windowscodecs.dll","IWICBitmapScaler_Initialize_Proxy",8,5},
    {"windowscodecs.dll","IWICBitmapSource_CopyPalette_Proxy",6,2},
    {"windowscodecs.dll","IWICBitmapSource_CopyPixels_Proxy",7,5},
    {"windowscodecs.dll","IWICBitmapSource_GetPixelFormat_Proxy",4,2},
    {"windowscodecs.dll","IWICBitmapSource_GetResolution_Proxy",5,3},
    {"windowscodecs.dll","IWICBitmapSource_GetSize_Proxy",3,3},
    {"windowscodecs.dll","IWICBitmap_Lock_Proxy",8,4},
    {"windowscodecs.dll","IWICBitmap_SetPalette_Proxy",9,2},
    {"windowscodecs.dll","IWICBitmap_SetResolution_Proxy",10,5},
    {"windowscodecs.dll","IWICColorContext_InitializeFromMemory_Proxy",4,3},
    {"windowscodecs.dll","IWICComponentFactory_CreateMetadataWriterFromReader_Proxy",31,4},
    {"windowscodecs.dll","IWICComponentFactory_CreateQueryWriterFromBlockWriter_Proxy",33,3},
    {"windowscodecs.dll","IWICComponentInfo_GetAuthor_Proxy",6,4},
    {"windowscodecs.dll","IWICComponentInfo_GetCLSID_Proxy",4,2},
    {"windowscodecs.dll","IWICComponentInfo_GetFriendlyName_Proxy",10,4},
    {"windowscodecs.dll","IWICComponentInfo_GetSpecVersion_Proxy",9,4},
    {"windowscodecs.dll","IWICComponentInfo_GetVersion_Proxy",8,4},
    {"windowscodecs.dll","IWICFastMetadataEncoder_Commit_Proxy",3,1},
    {"windowscodecs.dll","IWICFastMetadataEncoder_GetMetadataQueryWriter_Proxy",4,2},
    {"windowscodecs.dll","IWICFormatConverter_Initialize_Proxy",8,8},
    {"windowscodecs.dll","IWICImagingFactory_CreateBitmapClipper_Proxy",12,2},
    {"windowscodecs.dll","IWICImagingFactory_CreateBitmapFlipRotator_Proxy",13,2},
    {"windowscodecs.dll","IWICImagingFactory_CreateBitmapFromHBITMAP_Proxy",21,5},
    {"windowscodecs.dll","IWICImagingFactory_CreateBitmapFromHICON_Proxy",22,3},
    {"windowscodecs.dll","IWICImagingFactory_CreateBitmapFromMemory_Proxy",20,8},
    {"windowscodecs.dll","IWICImagingFactory_CreateBitmapFromSource_Proxy",18,4},
    {"windowscodecs.dll","IWICImagingFactory_CreateBitmapScaler_Proxy",11,2},
    {"windowscodecs.dll","IWICImagingFactory_CreateBitmap_Proxy",17,6},
    {"windowscodecs.dll","IWICImagingFactory_CreateComponentInfo_Proxy",6,3},
    {"windowscodecs.dll","IWICImagingFactory_CreateDecoderFromFileHandle_Proxy",5,5},
    {"windowscodecs.dll","IWICImagingFactory_CreateDecoderFromFilename_Proxy",3,6},
    {"windowscodecs.dll","IWICImagingFactory_CreateDecoderFromStream_Proxy",4,5},
    {"windowscodecs.dll","IWICImagingFactory_CreateEncoder_Proxy",8,4},
    {"windowscodecs.dll","IWICImagingFactory_CreateFastMetadataEncoderFromDecoder_Proxy",24,3},
    {"windowscodecs.dll","IWICImagingFactory_CreateFastMetadataEncoderFromFrameDecode_Proxy",25,3},
    {"windowscodecs.dll","IWICImagingFactory_CreateFormatConverter_Proxy",10,2},
    {"windowscodecs.dll","IWICImagingFactory_CreatePalette_Proxy",9,2},
    {"windowscodecs.dll","IWICImagingFactory_CreateQueryWriterFromReader_Proxy",27,4},
    {"windowscodecs.dll","IWICImagingFactory_CreateQueryWriter_Proxy",26,4},
    {"windowscodecs.dll","IWICImagingFactory_CreateStream_Proxy",14,2},
    {"windowscodecs.dll","IWICMetadataBlockReader_GetCount_Proxy",4,2},
    {"windowscodecs.dll","IWICMetadataBlockReader_GetReaderByIndex_Proxy",5,3},
    {"windowscodecs.dll","IWICMetadataQueryReader_GetContainerFormat_Proxy",3,2},
    {"windowscodecs.dll","IWICMetadataQueryReader_GetEnumerator_Proxy",6,2},
    {"windowscodecs.dll","IWICMetadataQueryReader_GetLocation_Proxy",4,4},
    {"windowscodecs.dll","IWICMetadataQueryReader_GetMetadataByName_Proxy",5,3},
    {"windowscodecs.dll","IWICMetadataQueryWriter_RemoveMetadataByName_Proxy",8,2},
    {"windowscodecs.dll","IWICMetadataQueryWriter_SetMetadataByName_Proxy",7,3},
    {"windowscodecs.dll","IWICPalette_GetColorCount_Proxy",8,2},
    {"windowscodecs.dll","IWICPalette_GetColors_Proxy",9,4},
    {"windowscodecs.dll","IWICPalette_GetType_Proxy",7,2},
    {"windowscodecs.dll","IWICPalette_HasAlpha_Proxy",12,2},
    {"windowscodecs.dll","IWICPalette_InitializeCustom_Proxy",4,3},
    {"windowscodecs.dll","IWICPalette_InitializeFromBitmap_Proxy",5,4},
    {"windowscodecs.dll","IWICPalette_InitializeFromPalette_Proxy",6,2},
    {"windowscodecs.dll","IWICPalette_InitializePredefined_Proxy",3,3},
    {"windowscodecs.dll","IWICPixelFormatInfo_GetBitsPerPixel_Proxy",13,2},
    {"windowscodecs.dll","IWICPixelFormatInfo_GetChannelCount_Proxy",14,2},
    {"windowscodecs.dll","IWICPixelFormatInfo_GetChannelMask_Proxy",15,5},
    {"windowscodecs.dll","IWICStream_InitializeFromIStream_Proxy",14,2},
    {"windowscodecs.dll","IWICStream_InitializeFromMemory_Proxy",16,3},
    {"windowscodecs.dll","WICCreateColorContext_Proxy",15,2},
};
constexpr std::uint32_t pointer_error=0x80004003U;
}
std::span<const WindowsServiceDescriptor> vtable_proxy_service_descriptors(){
    static const auto table=[]{std::vector<WindowsServiceDescriptor> result;for(const auto& entry:entries)result.push_back({entry.dll,entry.name,entry.slots});return result;}();
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_vtable_proxy(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);
    const auto entry=std::find_if(std::begin(entries),std::end(entries),[&](const ProxyEntry& e){return e.name==service.symbol && e.dll==service.family;});
    if(entry==std::end(entries)){arg.return_to(state,0x80004001U);return std::nullopt;}
    const std::uint8_t bits=x86_?32:64;
    try{
        // NULL 介面或 vtable 無效時回 E_POINTER，不轉成存取違規。
        const auto object=arg(0);if(!object){arg.return_to(state,entry->null_result);return std::nullopt;}
        const auto table=memory.load(GuestAddress{object},bits);if(!table){arg.return_to(state,pointer_error);return std::nullopt;}
        const auto method=memory.load(GuestAddress{table+std::uint64_t{entry->index}*(bits/8)},bits);if(!method){arg.return_to(state,pointer_error);return std::nullopt;}
        if(!handles(GuestAddress{method}))memory.check(GuestAddress{method},1,Access::Execute);
        if(entry->conversion!=AviConversion::none){
            const auto conversion=entry->conversion;
            return nested_continuation(state,memory,"AviInfo",[this,service,conversion,method](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);
                const bool create=conversion==AviConversion::create_stream;
                // A/W 結構只在末尾的 64 字元陣列寬度不同，之前的欄位均為固定寬度。
                const unsigned prefix=conversion==AviConversion::file_info?44U:76U;
                const unsigned wide_size=prefix+128;
                const auto caller_buffer=call(create?2:1);
                const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
                if(!create && static_cast<std::uint32_t>(call(2))<prefix+64)co_return finish(0x8004406bU);
                if(!caller_buffer)co_return finish(0x8004406aU);
                GuestScratch scratch(*this,m,s);
                const auto wide=scratch.allocate(wide_size);
                if(!wide)co_return finish(0x80044067U);
                m.write(GuestAddress{wide},std::vector<std::uint8_t>(wide_size));
                std::vector<std::uint8_t> fixed(prefix);
                if(create){
                    m.read(GuestAddress{caller_buffer},fixed);
                    m.write(GuestAddress{wide},fixed);
                    unsigned length=0;
                    while(length<63 && m.load(GuestAddress{caller_buffer+prefix+length},8))++length;
                    if(length && !nested_service(s,m,"kernel32.dll","MultiByteToWideChar",{0,0,caller_buffer+prefix,length,wide+prefix,63}))
                        co_return finish(0x8004406aU);
                    co_return finish(co_await service_callback::call(method,{call(0),call(1),wide}));
                }
                m.check(GuestAddress{caller_buffer},prefix+64,Access::Write);
                const auto result=static_cast<std::uint32_t>(co_await service_callback::call(method,{call(0),wide,wide_size}));
                if(!(result&0x80000000U)){
                    m.read(GuestAddress{wide},fixed);
                    m.write(GuestAddress{caller_buffer},fixed);
                    unsigned length=0;
                    while(length<64 && m.load(GuestAddress{wide+prefix+2*length},16))++length;
                    // 找到可完整裝入的前綴，避免在多位元組字元中間截斷。
                    while(length && nested_service(s,m,"kernel32.dll","WideCharToMultiByte",{0,0,wide+prefix,length,0,0,0,0})>63)--length;
                    const auto bytes=length?nested_service(s,m,"kernel32.dll","WideCharToMultiByte",{0,0,wide+prefix,length,caller_buffer+prefix,63,0,0}):0;
                    m.store(GuestAddress{caller_buffer+prefix+bytes},8,0);
                }
                co_return finish(result);
            });
        }
        state.rip=GuestAddress{method};return std::nullopt;
    }catch(const MemoryFault&){arg.return_to(state,pointer_error);return std::nullopt;}
}
}

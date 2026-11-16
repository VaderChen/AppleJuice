// ColorSync 色彩轉換：設定檔序列 → ColorSyncTransform，32 位元浮點無 alpha 交錯資料轉換。
#include "x64arm/color_transform.hpp"
#include <ColorSync/ColorSync.h>

namespace x64arm {
struct NativeColorTransform {ColorSyncTransformRef transform{};~NativeColorTransform(){if(transform)CFRelease(transform);}};
namespace {
CFStringRef intent_name(std::uint32_t intent){
    switch(intent){case 1:return kColorSyncRenderingIntentRelative;case 2:return kColorSyncRenderingIntentSaturation;case 3:return kColorSyncRenderingIntentAbsolute;default:return kColorSyncRenderingIntentPerceptual;}
}
struct Release {CFTypeRef value;~Release(){if(value)CFRelease(value);}};
}
std::shared_ptr<NativeColorTransform> native_color_transform(const std::vector<std::vector<std::uint8_t>>& profiles,const std::vector<std::uint32_t>& intents){
    if(profiles.empty())return nullptr;
    const auto array=CFArrayCreateMutable(kCFAllocatorDefault,0,&kCFTypeArrayCallBacks);if(!array)return nullptr;Release array_release{array};
    std::vector<ColorSyncProfileRef> refs;struct ReleaseAll {std::vector<ColorSyncProfileRef>& items;~ReleaseAll(){for(auto item:items)if(item)CFRelease(item);}} release_all{refs};
    for(const auto& bytes:profiles){
        const auto data=CFDataCreate(kCFAllocatorDefault,bytes.data(),static_cast<CFIndex>(bytes.size()));if(!data)return nullptr;Release data_release{data};
        const auto profile=ColorSyncProfileCreate(data,nullptr);if(!profile)return nullptr;refs.push_back(profile);
    }
    const auto add=[&](ColorSyncProfileRef profile,std::uint32_t intent,CFStringRef tag){
        const void* keys[]{kColorSyncProfile,kColorSyncRenderingIntent,kColorSyncTransformTag};const void* values[]{profile,intent_name(intent),tag};
        const auto entry=CFDictionaryCreate(kCFAllocatorDefault,keys,values,3,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);if(!entry)return false;
        CFArrayAppendValue(array,entry);CFRelease(entry);return true;
    };
    const auto intent_at=[&](std::size_t index){return intents.empty()?0U:intents[std::min(index,intents.size()-1)];};
    // 單一設定檔（device link）為裝置對裝置；其餘首段裝置→PCS、末段 PCS→裝置，中間段先到裝置再回 PCS。
    if(refs.size()==1){if(!add(refs[0],intent_at(0),kColorSyncTransformDeviceToDevice))return nullptr;}
    else for(std::size_t i=0;i<refs.size();++i){
        const auto intent=intent_at(i);
        if(i==0){if(!add(refs[i],intent,kColorSyncTransformDeviceToPCS))return nullptr;}
        else if(i+1==refs.size()){if(!add(refs[i],intent,kColorSyncTransformPCSToDevice))return nullptr;}
        else if(!add(refs[i],intent,kColorSyncTransformPCSToDevice) || !add(refs[i],intent,kColorSyncTransformDeviceToPCS))return nullptr;
    }
    auto result=std::make_shared<NativeColorTransform>();result->transform=ColorSyncTransformCreate(array,nullptr);if(!result->transform)return nullptr;return result;
}
bool native_color_convert(const NativeColorTransform& transform,std::size_t count,unsigned input_channels,const float* input,unsigned output_channels,float* output,bool extended){
    if(!transform.transform || !count)return count==0;
    const void* keys[]{kColorSyncConvertUseExtendedRange};const void* values[]{extended?kCFBooleanTrue:kCFBooleanFalse};
    const auto options=CFDictionaryCreate(kCFAllocatorDefault,keys,values,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);Release options_release{options};
    // 以一列 count 像素轉換；ColorSyncDataLayout 為 alpha 資訊與位元組序（主機序）。
    const auto layout=static_cast<ColorSyncDataLayout>(static_cast<std::uint32_t>(kColorSyncAlphaNone)|static_cast<std::uint32_t>(kColorSyncByteOrderDefault));
    return ColorSyncTransformConvert(transform.transform,count,1,output,kColorSync32BitFloat,layout,count*output_channels*sizeof(float),input,kColorSync32BitFloat,layout,count*input_channels*sizeof(float),options);
}
}

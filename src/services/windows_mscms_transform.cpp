// mscms.dll 色彩轉換、具名色彩、PostScript 色彩字典與列印排存輔助：
// CreateColorTransformA／W、CreateMultiProfileTransform、DeleteColorTransform、TranslateColors、TranslateBitmapBits、CheckColors、CheckBitmapBits、
// GetCMMInfo、CreateDeviceLinkProfile、ConvertColorNameToIndex、ConvertIndexToColorName、GetNamedProfileInfo、GetPS2ColorRenderingIntent、
// GetPS2ColorSpaceArray、GetPS2ColorRenderingDictionary、GenerateCopyFilePaths、SpoolerCopyFileEvent。
// 轉換以宿主 ColorSync 處理 0..1 浮點裝置值；mscms 的 COLOR 與點陣格式在這裡解碼／編碼。
#include "ansi_codepage.hpp"
#include "windows_icm_store.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace x64arm {
namespace {
// 點陣格式：每像素位元組數、通道數與讀寫方式。PCS 格式（XYZ／Yxy／Lab）沿用 3 通道的正規化編碼。
struct BitmapFormat {unsigned bytes{},channels{};enum class Kind {Bytes,Words,Packed555,Packed565,Packed10} kind{};std::array<std::uint8_t,8> order{};};
std::optional<BitmapFormat> bitmap_format(std::uint32_t format){
    using K=BitmapFormat::Kind;
    switch(format){
    case 0x0000:case 0x0101:case 0x0102:case 0x0103:case 0x0104:return BitmapFormat{2,3,K::Packed555,{0,1,2}};
    case 0x0001:return BitmapFormat{2,3,K::Packed565,{0,1,2}};
    case 0x0002:case 0x0201:case 0x0202:case 0x0203:case 0x0204:return BitmapFormat{3,3,K::Bytes,{0,1,2}};
    case 0x0004:return BitmapFormat{3,3,K::Bytes,{2,1,0}};
    case 0x0205:case 0x0206:case 0x0207:case 0x0208:{const unsigned n=format-0x0200;return BitmapFormat{n,n,K::Bytes,{0,1,2,3,4,5,6,7}};}
    case 0x0209:return BitmapFormat{1,1,K::Bytes,{0}};
    // x 在第一個位元組（忽略）：xRGB、xBGR、xG3CH。
    case 0x0008:return BitmapFormat{4,3,K::Bytes,{1,2,3}};
    case 0x0010:return BitmapFormat{4,3,K::Bytes,{3,2,1}};
    case 0x0304:return BitmapFormat{4,3,K::Bytes,{1,2,3}};
    case 0x0305:return BitmapFormat{4,4,K::Bytes,{3,2,1,0}};
    case 0x0020:return BitmapFormat{4,4,K::Bytes,{0,1,2,3}};
    case 0x0009:case 0x0401:case 0x0402:case 0x0403:case 0x0404:return BitmapFormat{4,3,K::Packed10,{0,1,2}};
    case 0x000a:case 0x0501:case 0x0502:case 0x0503:case 0x0504:return BitmapFormat{6,3,K::Words,{0,1,2}};
    case 0x0505:return BitmapFormat{2,1,K::Words,{0}};
    default:return std::nullopt;
    }
}
void decode_pixel(const BitmapFormat& format,const std::uint8_t* pixel,float* out){
    using K=BitmapFormat::Kind;const auto word=[&](unsigned at){return static_cast<unsigned>(pixel[at]|pixel[at+1]<<8);};
    switch(format.kind){
    case K::Bytes:for(unsigned c=0;c<format.channels;++c)out[c]=pixel[format.order[c]]/255.0F;break;
    case K::Words:for(unsigned c=0;c<format.channels;++c)out[c]=static_cast<float>(word(format.order[c]*2U))/65535.0F;break;
    case K::Packed555:{const auto v=word(0);out[0]=static_cast<float>((v>>10)&31U)/31.0F;out[1]=static_cast<float>((v>>5)&31U)/31.0F;out[2]=static_cast<float>(v&31U)/31.0F;break;}
    case K::Packed565:{const auto v=word(0);out[0]=static_cast<float>((v>>11)&31U)/31.0F;out[1]=static_cast<float>((v>>5)&63U)/63.0F;out[2]=static_cast<float>(v&31U)/31.0F;break;}
    case K::Packed10:{const auto v=static_cast<std::uint32_t>(word(0)|word(2)<<16);out[0]=static_cast<float>((v>>20)&1023U)/1023.0F;out[1]=static_cast<float>((v>>10)&1023U)/1023.0F;out[2]=static_cast<float>(v&1023U)/1023.0F;break;}
    }
}
void encode_pixel(const BitmapFormat& format,const float* in,std::uint8_t* pixel){
    using K=BitmapFormat::Kind;const auto q=[&](unsigned c,unsigned scale){return static_cast<unsigned>(std::lround(std::clamp(in[c],0.0F,1.0F)*static_cast<float>(scale)));};
    const auto put=[&](unsigned at,unsigned value){pixel[at]=static_cast<std::uint8_t>(value);pixel[at+1]=static_cast<std::uint8_t>(value>>8);};
    switch(format.kind){
    case K::Bytes:if(format.bytes==4 && format.channels==3)pixel[0]=0;for(unsigned c=0;c<format.channels;++c)pixel[format.order[c]]=static_cast<std::uint8_t>(q(c,255));break;
    case K::Words:for(unsigned c=0;c<format.channels;++c)put(format.order[c]*2U,q(c,65535));break;
    case K::Packed555:put(0,q(0,31)<<10|q(1,31)<<5|q(2,31));break;
    case K::Packed565:put(0,q(0,31)<<11|q(1,63)<<5|q(2,31));break;
    case K::Packed10:{const auto v=q(0,1023)<<20|q(1,1023)<<10|q(2,1023);put(0,v&0xffffU);put(2,v>>16);break;}
    }
}
// COLOR 的通道：GRAY／RGB／XYZ／Yxy／Lab／3_CHANNEL／CMYK 為 WORD，5～8_CHANNEL 為 BYTE（HiFiCOLOR）。
struct ColorKind {unsigned channels{};bool bytes{};};
std::optional<ColorKind> color_kind(std::uint32_t type){
    switch(type){case 1:return ColorKind{1,false};case 2:case 3:case 4:case 5:case 6:return ColorKind{3,false};case 7:return ColorKind{4,false};case 8:case 9:case 10:case 11:return ColorKind{type-3,true};default:return std::nullopt;}
}
// 超出 0..1 的最大距離換成 RESULT（0 在色域內，越大越遠）。
std::uint8_t gamut_result(const float* values,unsigned channels){
    float excess=0;for(unsigned c=0;c<channels;++c)excess=std::max({excess,-values[c],values[c]-1.0F});
    if(excess<=1.0F/512)return 0;return static_cast<std::uint8_t>(std::clamp(std::lround(excess*255.0F),1L,255L));
}
std::string ps_number(double value){char text[32];std::snprintf(text,sizeof text,"%.6g",value);return text;}
// 'curv' 轉成 PostScript 程序：count 0 為恆等、1 為 gamma、表格以 256 點取樣。inverse 時產生編碼方向（線性→裝置）。
std::optional<std::string> curve_procedure(const std::vector<std::uint8_t>* data,bool inverse){
    if(!data || data->size()<12 || icc_u32(*data,0)!=0x63757276)return std::nullopt;const auto count=icc_u32(*data,8);if(12+std::uint64_t{count}*2>data->size())return std::nullopt;
    const auto entry=[&](std::uint32_t i){return static_cast<double>(((*data)[12+i*2]<<8)|(*data)[13+i*2])/65535.0;};
    if(count==0)return std::string("{}");
    if(count==1){const double gamma=static_cast<double>(((*data)[12]<<8)|(*data)[13])/256.0;if(gamma<=0)return std::nullopt;return "{"+ps_number(inverse?1.0/gamma:gamma)+" exp}";}
    std::string table="[";
    for(unsigned i=0;i<256;++i){const double x=i/255.0;double y=0;
        if(!inverse){const double at=x*(count-1);const auto low=static_cast<std::uint32_t>(std::floor(at));const auto high=std::min(low+1,count-1);y=entry(low)+(entry(high)-entry(low))*(at-low);}
        else {std::uint32_t k=0;while(k+1<count && entry(k+1)<x)++k;const double a=entry(k),b=entry(std::min(k+1,count-1));y=(k+(b>a?(x-a)/(b-a):0))/(count-1);}
        table+=ps_number(std::clamp(y,0.0,1.0));table+=i%16==15?"\n":" ";}
    table+="]";return "{dup 0 lt {pop 0} if dup 1 gt {pop 1} if 255 mul round cvi "+table+" exch get}";
}
std::optional<std::array<double,3>> xyz_tag(const IccProfile& model,std::uint32_t tag){
    const auto* data=model.find(tag);if(!data || data->size()<20 || icc_u32(*data,0)!=0x58595a20)return std::nullopt;
    std::array<double,3> value{};for(unsigned i=0;i<3;++i)value[i]=static_cast<std::int32_t>(icc_u32(*data,8+i*4))/65536.0;return value;
}
}
std::span<const WindowsServiceDescriptor> mscms_transform_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"mscms.dll","CreateColorTransformA",4},{"mscms.dll","CreateColorTransformW",4},{"mscms.dll","CreateMultiProfileTransform",6},{"mscms.dll","DeleteColorTransform",1},
        {"mscms.dll","TranslateColors",6},{"mscms.dll","TranslateBitmapBits",11},{"mscms.dll","CheckColors",5},{"mscms.dll","CheckBitmapBits",9},{"mscms.dll","GetCMMInfo",2},
        {"mscms.dll","CreateDeviceLinkProfile",7},{"mscms.dll","ConvertColorNameToIndex",4},{"mscms.dll","ConvertIndexToColorName",4},{"mscms.dll","GetNamedProfileInfo",2},
        {"mscms.dll","GetPS2ColorRenderingIntent",4},{"mscms.dll","GetPS2ColorSpaceArray",6},{"mscms.dll","GetPS2ColorRenderingDictionary",5},
        {"mscms.dll","GenerateCopyFilePaths",9},{"mscms.dll","SpoolerCopyFileEvent",3},
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_mscms_transform(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto fail=[&](std::uint32_t error){last_error_=error;return finish(0);};
    const auto d=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    auto& opened=icm_profiles();IcmStore store(*this,state,memory);
    const auto profile=[&](std::uint64_t handle){const auto found=opened.profiles.find(handle);return found==opened.profiles.end()?nullptr:&found->second;};
    // 設定檔序列 → 轉換：輸入取第一個設定檔的資料色彩空間；輸出取最後一個（device link 為其 PCS 欄位）。
    const auto build=[&](const std::vector<std::vector<std::uint8_t>>& sequence,const std::vector<std::uint32_t>& intents)->std::optional<IcmProfiles::Transform>{
        if(sequence.empty() || sequence.size()>16){last_error_=87;return std::nullopt;}
        IcmProfiles::Transform transform;transform.input_space=icc_u32(sequence.front(),16);
        const bool link=sequence.size()==1 && icc_u32(sequence.front(),12)==0x6c696e6b;transform.output_space=link?icc_u32(sequence.front(),20):icc_u32(sequence.back(),16);
        transform.input=icc_space_channels(transform.input_space);transform.output=icc_space_channels(transform.output_space);
        if(!transform.input || !transform.output){last_error_=icm_error_invalid_profile;return std::nullopt;}
        transform.native=native_color_transform(sequence,intents);if(!transform.native){last_error_=icm_error_invalid_profile;return std::nullopt;}return transform;
    };
    const auto keep=[&](IcmProfiles::Transform transform){const auto handle=opened.next;opened.next+=4;opened.transforms.emplace(handle,std::move(transform));return handle;};
    const auto handles=[&](std::uint64_t array,std::uint32_t count,std::uint64_t intents_pointer,std::uint32_t intent_count,std::vector<std::vector<std::uint8_t>>& sequence,std::vector<std::uint32_t>& intents){
        if(!array || !count || count>16 || (intent_count && !intents_pointer))return false;
        for(std::uint32_t i=0;i<count;++i){const auto* entry=profile(memory.load(GuestAddress{array+i*unit},bits));if(!entry){last_error_=6;return false;}sequence.push_back(icc_build(entry->model));}
        for(std::uint32_t i=0;i<std::min(intent_count,16U);++i)intents.push_back(static_cast<std::uint32_t>(memory.load(GuestAddress{intents_pointer+i*4},32)));return true;
    };
    const auto transform_of=[&](std::uint64_t handle){const auto found=opened.transforms.find(handle);return found==opened.transforms.end()?nullptr:&found->second;};
    const auto convert=[&](const IcmProfiles::Transform& transform,std::size_t count,const std::vector<float>& input,std::vector<float>& output,bool extended){
        output.assign(count*transform.output,0);return native_color_convert(*transform.native,count,transform.input,input.data(),transform.output,output.data(),extended);
    };
    // 以 NUL 結尾的字串或資料輸出：*size 為位元組數，緩衝區為 NULL 時只回報大小。
    const auto output_bytes=[&](std::uint64_t buffer,std::uint64_t size,const std::vector<std::uint8_t>& bytes){
        if(!size)return fail(87);const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{size},32));memory.store(GuestAddress{size},32,bytes.size());
        if(!buffer)return finish(1);if(capacity<bytes.size())return fail(122);memory.write(GuestAddress{buffer},bytes);return finish(1);
    };
    try{
        if(name.starts_with("CreateColorTransform")){
            // {LOGCOLORSPACE*, hDestProfile, hTargetProfile, dwFlags}：來源由 LOGCOLORSPACE 產生（有 lcsFilename 時讀該設定檔），目標設定檔用於模擬。
            const bool wide=name.ends_with('W');if(!arg(0) || !arg(1))return fail(87);
            std::array<std::uint32_t,17> fields{};for(unsigned i=0;i<17;++i)fields[i]=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+i*4},32));if(fields[0]!=0x50534f43)return fail(87);
            const auto file=wide?read_utf16(memory,GuestAddress{arg(0)+68},259):ansi_to_utf16(read_ansi(memory,arg(0)+68,259));
            std::vector<std::uint8_t> source;
            if(!file.empty()){const auto bytes=store.read_profile(icm_profile_path(file));if(!bytes)return finish(0);source=*bytes;}
            else {IccRgbParameters parameters;
                if(fields[3]==lcs_srgb || fields[3]==lcs_windows_color_space)parameters=icc_srgb_parameters();
                else if(fields[3]==lcs_calibrated_rgb){for(unsigned c=0;c<3;++c)for(unsigned i=0;i<3;++i)parameters.colorants[c][i]=static_cast<std::int32_t>(fields[5+c*3+i])/1073741824.0;
                    for(unsigned i=0;i<3;++i){parameters.gamma[i]=fields[14+i]/65536.0;if(parameters.gamma[i]<=0)return fail(87);parameters.white[i]=parameters.colorants[0][i]+parameters.colorants[1][i]+parameters.colorants[2][i];}}
                else return fail(87);source=icc_rgb_profile(parameters);}
            const std::uint32_t intent=fields[4]==1?2U:fields[4]==2?1U:fields[4]==8?3U:0U;
            std::vector<std::vector<std::uint8_t>> sequence{source};
            if(arg(2)){const auto* target=profile(arg(2));if(!target)return fail(6);sequence.push_back(icc_build(target->model));}
            const auto* dest=profile(arg(1));if(!dest)return fail(6);sequence.push_back(icc_build(dest->model));
            auto transform=build(sequence,{intent});if(!transform)return finish(0);return finish(keep(std::move(*transform)));
        }
        if(name=="CreateMultiProfileTransform"){
            // {pahProfiles, nProfiles, padwIntent, nIntents, dwFlags, indexPreferredCMM}。
            std::vector<std::vector<std::uint8_t>> sequence;std::vector<std::uint32_t> intents;if(!handles(arg(0),d(1),arg(2),d(3),sequence,intents))return finish(0);
            auto transform=build(sequence,intents);if(!transform)return finish(0);return finish(keep(std::move(*transform)));
        }
        if(name=="DeleteColorTransform")return opened.transforms.erase(arg(0))?finish(1):fail(6);
        if(name=="GetCMMInfo"){
            // 內建 Windows CMM（'Win '）；描述與圖示沒有資源，回 0。
            if(arg(0) && !transform_of(arg(0)))return fail(6);
            switch(d(1)){case 0:return finish(0x00060000);case 1:return finish(0x57696e20);case 2:return finish(2);case 3:case 4:return finish(0x00010000);default:return finish(0);}
        }
        if(name=="TranslateColors" || name=="CheckColors"){
            // TranslateColors {h, paInput, nColors, ctInput, paOutput, ctOutput}；CheckColors {h, paInput, nColors, ctInput, paResult}。COLOR 在 x86 為 8、x64 為 16 位元組。
            const auto* transform=transform_of(arg(0));if(!transform)return fail(6);const auto count=d(2);if(!count)return finish(1);if(!arg(1) || !arg(4) || count>1u<<24)return fail(87);
            const auto input=color_kind(d(3));if(!input || input->channels!=transform->input)return fail(87);const unsigned stride=x86_?8U:16U;
            std::vector<float> values(static_cast<std::size_t>(count)*input->channels),converted;
            for(std::uint32_t i=0;i<count;++i)for(unsigned c=0;c<input->channels;++c)
                values[i*input->channels+c]=input->bytes?static_cast<float>(memory.load(GuestAddress{arg(1)+i*stride+c},8))/255.0F:static_cast<float>(memory.load(GuestAddress{arg(1)+i*stride+c*2},16))/65535.0F;
            const bool check=name=="CheckColors";if(!convert(*transform,count,values,converted,check))return fail(2010);
            if(check){for(std::uint32_t i=0;i<count;++i)memory.store(GuestAddress{arg(4)+i},8,gamut_result(&converted[i*transform->output],transform->output));return finish(1);}
            const auto output=color_kind(d(5));if(!output || output->channels!=transform->output)return fail(87);
            for(std::uint32_t i=0;i<count;++i){memory.write(GuestAddress{arg(4)+i*stride},std::vector<std::uint8_t>(stride));for(unsigned c=0;c<output->channels;++c){const auto v=std::clamp(converted[i*output->channels+c],0.0F,1.0F);
                if(output->bytes)memory.store(GuestAddress{arg(4)+i*stride+c},8,static_cast<std::uint32_t>(std::lround(v*255)));else memory.store(GuestAddress{arg(4)+i*stride+c*2},16,static_cast<std::uint32_t>(std::lround(v*65535)));}}
            return finish(1);
        }
        if(name=="TranslateBitmapBits" || name=="CheckBitmapBits"){
            // TranslateBitmapBits {h, src, bmInput, width, height, inputStride, dest, bmOutput, outputStride, callback, data}；
            // CheckBitmapBits {h, src, bmInput, width, height, stride, paResult, callback, data}。stride 0 時每列補齊到 DWORD；回呼每列呼叫，回 FALSE 取消。
            const bool check=name=="CheckBitmapBits";const auto* transform=transform_of(arg(0));if(!transform)return fail(6);
            const auto input=bitmap_format(d(2));const auto width=d(3),height=d(4);if(!input || input->channels!=transform->input || !arg(1) || !arg(6))return fail(87);
            std::optional<BitmapFormat> output;if(!check){output=bitmap_format(d(7));if(!output || output->channels!=transform->output)return fail(87);}
            if(!width || !height)return finish(1);if(width>65536 || height>65536)return fail(87);
            const auto in_stride=d(5)?std::uint64_t{d(5)}:(std::uint64_t{width}*input->bytes+3)&~std::uint64_t{3};
            const auto out_stride=check?std::uint64_t{width}:(d(8)?std::uint64_t{d(8)}:(std::uint64_t{width}*output->bytes+3)&~std::uint64_t{3});
            if(in_stride<std::uint64_t{width}*input->bytes || (!check && out_stride<std::uint64_t{width}*output->bytes))return fail(87);
            const auto callback=arg(check?7:9),data=arg(check?8:10);const auto source=arg(1),target=arg(6);const auto transform_copy=*transform;const auto in_format=*input;const auto out_format=output;
            // 一列：解碼、轉換、編碼（或寫出色域結果）。
            const auto row=[transform_copy,in_format,out_format,check,width,source,target,in_stride,out_stride](GuestMemory& m,std::uint32_t y){
                std::vector<std::uint8_t> raw(static_cast<std::size_t>(width)*in_format.bytes);m.read(GuestAddress{source+y*in_stride},raw);
                std::vector<float> values(static_cast<std::size_t>(width)*in_format.channels),converted(static_cast<std::size_t>(width)*transform_copy.output);
                for(std::uint32_t x=0;x<width;++x)decode_pixel(in_format,&raw[x*in_format.bytes],&values[x*in_format.channels]);
                if(!native_color_convert(*transform_copy.native,width,transform_copy.input,values.data(),transform_copy.output,converted.data(),check))return false;
                std::vector<std::uint8_t> encoded(check?width:static_cast<std::size_t>(width)*out_format->bytes);
                for(std::uint32_t x=0;x<width;++x){if(check)encoded[x]=gamut_result(&converted[x*transform_copy.output],transform_copy.output);else encode_pixel(*out_format,&converted[x*transform_copy.output],&encoded[x*out_format->bytes]);}
                m.write(GuestAddress{target+y*out_stride},encoded);return true;
            };
            if(!callback){for(std::uint32_t y=0;y<height;++y)if(!row(memory,y))return fail(2010);return finish(1);}
            return nested_continuation(state,memory,"TranslateBitmapBits",[this,service,row,height,callback,data](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);
                for(std::uint32_t y=0;y<height;++y){
                    if(!row(m,y)){last_error_=2010;call.return_to(s,0);co_return std::nullopt;}
                    if(!static_cast<std::uint32_t>(co_await service_callback::call(callback,{height,y+1,data}))){last_error_=1223;call.return_to(s,0);co_return std::nullopt;}
                }
                call.return_to(s,1);co_return std::nullopt;
            });
        }
        if(name=="CreateDeviceLinkProfile"){
            // {pahProfiles, nProfiles, padwIntents, nIntents, dwFlags, PBYTE* pProfileData, indexPreferredCMM}：以轉換取樣 lut16（mft2）A2B0，
            // 類別 'link'、色彩空間為輸入、PCS 欄位為輸出，附 profileSequenceDesc；以 GlobalAlloc 交回。
            if(!arg(5))return fail(87);std::vector<std::vector<std::uint8_t>> sequence;std::vector<std::uint32_t> intents;if(!handles(arg(0),d(1),arg(2),d(3),sequence,intents))return finish(0);
            auto transform=build(sequence,intents);if(!transform)return finish(0);const unsigned in=transform->input,out=transform->output;if(in>8)return fail(87);
            const unsigned grid=in==1?256U:in==2?33U:in==3?17U:in==4?9U:in<=6?5U:3U;std::size_t points=1;for(unsigned i=0;i<in;++i)points*=grid;
            std::vector<float> samples(points*in),converted;for(std::size_t p=0;p<points;++p){auto index=p;for(unsigned c=in;c-->0;){samples[p*in+c]=static_cast<float>(index%grid)/static_cast<float>(grid-1);index/=grid;}}
            if(!convert(*transform,points,samples,converted,false))return fail(2010);
            auto model=icc_empty();icc_put32(model.header,12,0x6c696e6b);icc_put32(model.header,16,transform->input_space);icc_put32(model.header,20,transform->output_space);icc_put32(model.header,40,0x4d534654);
            icc_put32(model.header,64,intents.empty()?0:intents.front());
            const auto add=[&](std::uint32_t tag,std::vector<std::uint8_t> data){model.tags.emplace_back(tag,model.data.size());model.data.push_back(std::move(data));};
            const auto text_description=[](std::string_view text){std::vector<std::uint8_t> data(12+text.size()+1+8+3+67);icc_put32(data,0,0x64657363);icc_put32(data,8,static_cast<std::uint32_t>(text.size()+1));std::copy(text.begin(),text.end(),data.begin()+12);return data;};
            add(0x64657363,text_description("Device link"));{const std::string text="No copyright, use freely";std::vector<std::uint8_t> data(8+text.size()+1);icc_put32(data,0,0x74657874);std::copy(text.begin(),text.end(),data.begin()+8);add(0x63707274,std::move(data));}
            {std::vector<std::uint8_t> lut(48+in*2*2+points*out*2+out*2*2);icc_put32(lut,0,0x6d667432);lut[8]=static_cast<std::uint8_t>(in);lut[9]=static_cast<std::uint8_t>(out);lut[10]=static_cast<std::uint8_t>(grid);
                for(unsigned i=0;i<3;++i)icc_put32(lut,12+i*16,0x00010000);lut[48]=0;lut[49]=0;
                const auto put16=[&](std::size_t at,unsigned value){lut[at]=static_cast<std::uint8_t>(value>>8);lut[at+1]=static_cast<std::uint8_t>(value);};
                put16(48,2);put16(50,2);std::size_t at=52;for(unsigned c=0;c<in;++c){put16(at,0);put16(at+2,65535);at+=4;}
                for(std::size_t p=0;p<points*out;++p){put16(at,static_cast<unsigned>(std::lround(std::clamp(converted[p],0.0F,1.0F)*65535)));at+=2;}
                for(unsigned c=0;c<out;++c){put16(at,0);put16(at+2,65535);at+=4;}lut.resize(at);add(0x41324230,std::move(lut));}
            {std::vector<std::uint8_t> pseq(12);icc_put32(pseq,0,0x70736571);icc_put32(pseq,8,static_cast<std::uint32_t>(sequence.size()));
                for(const auto& bytes:sequence){std::vector<std::uint8_t> item(20);icc_put32(item,0,icc_u32(bytes,48));icc_put32(item,4,icc_u32(bytes,52));icc_put32(item,8,icc_u32(bytes,56));icc_put32(item,12,icc_u32(bytes,60));
                    pseq.insert(pseq.end(),item.begin(),item.end());for(unsigned k=0;k<2;++k){const auto desc=text_description("");pseq.insert(pseq.end(),desc.begin(),desc.end());}}
                add(0x70736571,std::move(pseq));}
            const auto bytes=icc_build(model);const auto block=nested_service(state,memory,"kernel32.dll","GlobalAlloc",{0,bytes.size()});if(!block)return fail(8);
            memory.write(GuestAddress{block},bytes);memory.store(GuestAddress{arg(5)},bits,block);return finish(1);
        }
        if(name=="GetNamedProfileInfo" || name=="ConvertColorNameToIndex" || name=="ConvertIndexToColorName"){
            // namedColor2Type（'ncl2'）：{flags, count, nDeviceCoords, prefix[32], suffix[32], {root[32], PCS×3, device×n}…}；索引從 1 開始。
            const auto* entry=profile(arg(0));if(!entry)return fail(6);const auto* data=entry->model.find(0x6e636c32);if(!data || data->size()<84 || icc_u32(*data,0)!=0x6e636c32)return fail(2012);
            const auto count=icc_u32(*data,12),devices=icc_u32(*data,16);const std::size_t record=32+6+std::size_t{devices}*2;if(devices>15 || count>(data->size()-84)/record)return fail(icm_error_invalid_profile);
            const auto root=[&](std::uint32_t index){const auto at=84+index*record;std::string text(reinterpret_cast<const char*>(data->data()+at),32);text.resize(std::min(text.find('\0'),std::size_t{31}));return text;};
            if(name=="GetNamedProfileInfo"){if(!arg(1))return fail(87);std::vector<std::uint8_t> info(76);for(unsigned i=0;i<3;++i)for(unsigned b=0;b<4;++b)info[i*4+b]=static_cast<std::uint8_t>(icc_u32(*data,8+i*4)>>(8*b));
                std::copy_n(data->begin()+20,64,info.begin()+12);info[12+31]=0;info[44+31]=0;memory.write(GuestAddress{arg(1)},info);return finish(1);}
            const auto n=d(3);if(n && (!arg(1) || !arg(2)))return fail(87);
            for(std::uint32_t i=0;i<n;++i){
                if(name=="ConvertColorNameToIndex"){const auto wanted=read_ansi(memory,arg(1)+i*32,32);std::uint32_t found=0;for(std::uint32_t k=0;k<count && !found;++k)if(root(k)==wanted)found=k+1;
                    if(!found)return fail(87);memory.store(GuestAddress{arg(2)+i*4},32,found);}
                else {const auto index=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)+i*4},32));if(!index || index>count)return fail(87);
                    std::vector<std::uint8_t> text(32);const auto value=root(index-1);std::copy(value.begin(),value.end(),text.begin());memory.write(GuestAddress{arg(2)+i*32},text);}
            }
            return finish(1);
        }
        if(name.starts_with("GetPS2")){
            // 設定檔內有 PostScript tag（'ps2i'、'ps2s'、'psd0'～'psd3'，dataType）時直接取用；否則由矩陣／TRC RGB 或灰階 TRC 產生 ASCII 的 CIEBasedABC／A 與 type 1 CRD。
            const auto* entry=profile(arg(0));if(!entry)return fail(6);const auto& model=entry->model;const auto intent=d(1);if(intent>3)return fail(87);
            const bool intent_query=name=="GetPS2ColorRenderingIntent",array=name=="GetPS2ColorSpaceArray";
            const auto buffer=arg(intent_query?2:array?3:2),size=arg(intent_query?3:array?4:3),binary=intent_query?0:arg(array?5:4);
            const std::uint32_t tag=intent_query?0x70733269:array?0x70733273:(0x70736430+intent);
            if(const auto* data=model.find(tag);data && data->size()>=12 && icc_u32(*data,0)==0x64617461){
                if(binary)memory.store(GuestAddress{binary},32,icc_u32(*data,8)&1U);return output_bytes(buffer,size,std::vector<std::uint8_t>(data->begin()+12,data->end()));}
            std::string text;
            if(intent_query){constexpr const char* names[]{"/Perceptual","/RelativeColorimetric","/Saturation","/AbsoluteColorimetric"};text=names[intent];}
            else {
                const auto white=xyz_tag(model,0x77747074).value_or(std::array{0.9642,1.0,0.8249});const auto space=icc_u32(model.header,16);
                const auto vector=[&](const std::array<double,3>& v){return "["+ps_number(v[0])+" "+ps_number(v[1])+" "+ps_number(v[2])+"]";};
                if(space==0x47524159){
                    if(array && d(2)!=1 && d(2)!=5)return fail(87);const auto curve=curve_procedure(model.find(0x6b545243),!array);if(!curve)return fail(50);
                    // 灰階 CRD 以 Y 編碼到三個分量（MatrixABC 只取 Y）。
                    text=array?"[/CIEBasedA <<\n/DecodeA "+*curve+"\n/MatrixA "+vector(white)+"\n/WhitePoint "+vector(white)+"\n>>]\n"
                              :"<<\n/ColorRenderingType 1\n/WhitePoint "+vector(white)+"\n/MatrixABC [0 0 0 1 1 1 0 0 0]\n/EncodeABC ["+*curve+" "+*curve+" "+*curve+"]\n>>\n";
                }else if(space==0x52474220){
                    if(array && d(2)!=2 && d(2)!=6)return fail(87);
                    const auto r=xyz_tag(model,0x7258595a),g=xyz_tag(model,0x6758595a),b=xyz_tag(model,0x6258595a);if(!r || !g || !b)return fail(50);
                    std::array<std::optional<std::string>,3> curves{curve_procedure(model.find(0x72545243),!array),curve_procedure(model.find(0x67545243),!array),curve_procedure(model.find(0x62545243),!array)};
                    if(!curves[0] || !curves[1] || !curves[2])return fail(50);
                    if(array)text="[/CIEBasedABC <<\n/DecodeABC ["+*curves[0]+"\n"+*curves[1]+"\n"+*curves[2]+"]\n/MatrixABC ["+ps_number((*r)[0])+" "+ps_number((*r)[1])+" "+ps_number((*r)[2])+" "+
                        ps_number((*g)[0])+" "+ps_number((*g)[1])+" "+ps_number((*g)[2])+" "+ps_number((*b)[0])+" "+ps_number((*b)[1])+" "+ps_number((*b)[2])+"]\n/WhitePoint "+vector(white)+"\n>>]\n";
                    else {
                        // CRD 的 MatrixABC 把 XYZ 轉回線性 RGB（原色矩陣的反矩陣，PostScript 行向量順序）。
                        const std::array<std::array<double,3>,3> m{{{(*r)[0],(*g)[0],(*b)[0]},{(*r)[1],(*g)[1],(*b)[1]},{(*r)[2],(*g)[2],(*b)[2]}}};
                        const double det=m[0][0]*(m[1][1]*m[2][2]-m[1][2]*m[2][1])-m[0][1]*(m[1][0]*m[2][2]-m[1][2]*m[2][0])+m[0][2]*(m[1][0]*m[2][1]-m[1][1]*m[2][0]);if(std::abs(det)<1e-12)return fail(icm_error_invalid_profile);
                        std::array<std::array<double,3>,3> inv{};for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j){const auto a=m[(j+1)%3][(i+1)%3]*m[(j+2)%3][(i+2)%3]-m[(j+1)%3][(i+2)%3]*m[(j+2)%3][(i+1)%3];inv[i][j]=a/det;}
                        std::string matrix;for(unsigned j=0;j<3;++j)for(unsigned i=0;i<3;++i){matrix+=ps_number(inv[i][j]);matrix+=' ';}matrix.pop_back();
                        text="<<\n/ColorRenderingType 1\n/WhitePoint "+vector(white)+"\n/MatrixABC ["+matrix+"]\n/EncodeABC ["+*curves[0]+"\n"+*curves[1]+"\n"+*curves[2]+"]\n>>\n";
                    }
                }else return fail(50);
            }
            if(binary)memory.store(GuestAddress{binary},32,0);std::vector<std::uint8_t> bytes(text.begin(),text.end());bytes.push_back(0);return output_bytes(buffer,size,bytes);
        }
        if(name=="GenerateCopyFilePaths"){
            // {pszPrinterName, pszDirectory, pSplClientInfo, dwLevel, pszSourceDir, pcchSourceDirSize, pszTargetDir, pcchTargetDirSize, dwFlags}：
            // 色彩設定檔的目標目錄為色彩目錄（字元數含 NUL），來源維持原值；回傳 Win32 錯誤碼。
            if(!arg(6) || !arg(7))return finish(87);const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(7)},32));const auto needed=static_cast<std::uint32_t>(icm_color_directory.size()+1);
            memory.store(GuestAddress{arg(7)},32,needed);if(capacity<needed)return finish(122);memory.write(GuestAddress{arg(6)},utf16_bytes(icm_color_directory));return finish(0);
        }
        if(name=="SpoolerCopyFileEvent"){
            // {pszPrinterName, pszKey, dwCopyFileEvent}：印表機 CopyFiles\<key> 的 Files 清單中的設定檔——新增事件（1 設定資料、3 新增連線、5 檔案變更）安裝並與印表機關聯，
            // 刪除事件（2 刪除印表機、4 刪除連線）解除關聯。沒有清單時沒有要處理的設定檔。
            if(!arg(0) || !arg(1))return fail(87);const auto printer=read_utf16(memory,GuestAddress{arg(0)}),key=read_utf16(memory,GuestAddress{arg(1)});const auto event=d(2);if(event<1 || event>5)return fail(87);
            const auto files=store.read_machine_list(u"SYSTEM\\CurrentControlSet\\Control\\Print\\Printers\\"+printer+u"\\CopyFiles\\"+key,u"Files").value_or(std::vector<std::u16string>{});
            for(const auto& file:files){const auto lower=icm_lower(file);if(!lower.ends_with(u".icm") && !lower.ends_with(u".icc"))continue;
                if(event==2 || event==4)(void)store.disassociate(file,printer);else if(store.install(icm_profile_path(icm_file_part(file))))(void)store.associate(file,printer,false);}
            return finish(1);
        }
        return fail(50);
    }catch(const MemoryFault&){return fail(998);}catch(const std::bad_alloc&){return fail(8);}catch(const std::invalid_argument&){return fail(icm_error_invalid_profile);}
}
}

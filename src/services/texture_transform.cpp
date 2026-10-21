#include "texture_transform.hpp"
#include "texture_image.hpp"
#include <DirectXTex.h>
#include <cmath>
#include <cstring>
#include <optional>

namespace x64arm::texture_image {
namespace {
void checked(HRESULT value){if(FAILED(value))throw Error(static_cast<std::uint32_t>(value),"紋理格式轉換或縮放失敗");}
void valid(bool value){if(!value)throw Error(0x80070057U,"紋理轉換範圍或濾波參數無效");}
struct BoxAxis {
    struct Range {unsigned first,last;std::size_t offset;};
    std::vector<Range> ranges;
    std::vector<double> weights;
};
std::optional<std::array<BoxAxis,2>> box_footprint(std::array<unsigned,2> source,std::array<unsigned,2> target){
    // 每軸最多保留 1 MiB 係數；極長的一維紋理沿用直接積分，不放大工作記憶體。
    for(unsigned axis=0;axis<2;++axis)
        if(std::uint64_t{target[axis]}*sizeof(BoxAxis::Range)+(std::uint64_t{source[axis]}+2ULL*target[axis])*sizeof(double)>1024U*1024U)return std::nullopt;
    std::array<BoxAxis,2> plan;
    for(unsigned axis=0;axis<2;++axis){
        auto& out=plan[axis];out.ranges.reserve(target[axis]);out.weights.reserve(source[axis]+2U*target[axis]);
        const double scale=double(source[axis])/target[axis];
        for(unsigned at=0;at<target[axis];++at){
            const double lo=at*scale,hi=(at+1)*scale;
            const auto first=static_cast<unsigned>(std::floor(lo)),last=std::min(source[axis],static_cast<unsigned>(std::ceil(hi)));
            out.ranges.push_back({first,last,out.weights.size()});
            for(unsigned i=first;i<last;++i)out.weights.push_back(std::min(hi,i+1.0)-std::max(lo,double(i)));
        }
    }
    return plan;
}
}
void decode_dds(std::span<const std::uint8_t> source,GraphicsResourceDesc& d,
    std::vector<std::vector<std::uint8_t>>& pixels){
    using namespace DirectX;
    TexMetadata metadata{};
    checked(GetMetadataFromDDSMemory(source.data(),source.size(),DDS_FLAGS_NONE,metadata));
    valid(metadata.dimension>=TEX_DIMENSION_TEXTURE1D && metadata.dimension<=TEX_DIMENSION_TEXTURE3D &&
        metadata.width && metadata.width<=16384 && metadata.height && metadata.height<=16384 &&
        metadata.depth && metadata.depth<=2048 && metadata.arraySize && metadata.arraySize<=2048 &&
        metadata.mipLevels && metadata.mipLevels<=15 &&
        (metadata.dimension!=TEX_DIMENSION_TEXTURE3D || metadata.arraySize==1));
    std::uint64_t total=0;
    for(size_t mip=0;mip<metadata.mipLevels;++mip){
        size_t row=0,slice=0;checked(ComputePitch(metadata.format,std::max(size_t{1},metadata.width>>mip),std::max(size_t{1},metadata.height>>mip),row,slice));
        total+=std::uint64_t{slice}*std::max(size_t{1},metadata.depth>>mip)*metadata.arraySize;
        valid(total<=512U*1024U*1024U);
    }
    ScratchImage decoded;checked(LoadFromDDSMemory(source.data(),source.size(),DDS_FLAGS_NONE,&metadata,decoded));
    d.dimension=static_cast<std::uint32_t>(metadata.dimension);d.width=static_cast<std::uint32_t>(metadata.width);
    d.height=static_cast<std::uint32_t>(metadata.height);d.depth=static_cast<std::uint32_t>(metadata.depth);
    d.layers=static_cast<std::uint32_t>(metadata.arraySize);d.levels=static_cast<std::uint32_t>(metadata.mipLevels);
    d.format=static_cast<std::uint32_t>(metadata.format);d.misc=metadata.IsCubemap()?4U:0U;
    pixels.clear();pixels.reserve(d.layers*d.levels);
    for(unsigned layer=0;layer<d.layers;++layer)for(unsigned mip=0;mip<d.levels;++mip){
        const auto pitch=graphics_format_pitch(d.format,std::max(1U,d.width>>mip),std::max(1U,d.height>>mip));
        const auto depth=std::max(1U,d.depth>>mip);auto& destination=pixels.emplace_back(std::size_t{pitch[0]}*pitch[1]*depth);
        for(unsigned z=0;z<depth;++z){const auto* image=decoded.GetImage(mip,layer,z);valid(image && image->rowPitch>=pitch[0] && image->slicePitch>=image->rowPitch*pitch[1]);
            auto* output=destination.data()+std::size_t{z}*pitch[1]*pitch[0];
            if(image->rowPitch==pitch[0])std::memcpy(output,image->pixels,std::size_t{pitch[0]}*pitch[1]);
            else for(unsigned y=0;y<pitch[1];++y)std::memcpy(output+std::size_t{y}*pitch[0],image->pixels+y*image->rowPitch,pitch[0]);
        }
    }
}
std::vector<std::uint8_t> transform(std::span<const std::uint8_t> source,
    std::uint32_t source_format,std::array<std::uint32_t,3> size,
    std::array<std::uint32_t,6> box,std::uint32_t target_format,
    std::array<std::uint32_t,3> target,std::uint32_t filter){
    using namespace DirectX;
    if(filter==UINT32_MAX)filter=4;
    if(!(filter&15))filter|=4;
    const auto mode=filter&15;
    valid(mode>=1 && mode<=5 && !(filter&~0x7f000fU));
    for(unsigned i=0;i<3;++i)valid(size[i] && target[i] && box[i]<box[i+3] && box[i+3]<=size[i]);
    const std::array cropped{box[3]-box[0],box[4]-box[1],box[5]-box[2]};
    if(mode==1)valid(cropped==target);
    valid(std::uint64_t{target[0]}*target[1]*target[2]<=32U*1024U*1024U);
    valid(std::uint64_t{size[0]}*size[1]*cropped[2]<=32U*1024U*1024U);
    // DirectXTex 的數值轉換需要具體型別；typeless 資源仍保留原格式，
    // 只在像素解碼／編碼時採用同家族的 UNORM，或浮點格式。
    const auto typed=[](std::uint32_t format){return MakeTypelessFLOAT(MakeTypelessUNORM(static_cast<DXGI_FORMAT>(format)));};
    const auto sf=typed(source_format),df=typed(target_format);
    size_t row{},slice{};checked(ComputePitch(sf,size[0],size[1],row,slice));valid(source.size()>=slice*size[2]);
    unsigned flags=mode<=2?TEX_FILTER_POINT:mode==3?TEX_FILTER_LINEAR:mode==4?TEX_FILTER_TRIANGLE:TEX_FILTER_BOX;
    flags|=(filter&0x70000U)>>12;
    const auto input_flags=static_cast<TEX_FILTER_FLAGS>((filter&0x200000U)?TEX_FILTER_SRGB_IN:0);
    const auto output_flags=static_cast<TEX_FILTER_FLAGS>(((filter&0x400000U)?TEX_FILTER_SRGB_OUT:0)|((filter&0x80000U)?TEX_FILTER_DITHER:0)|((filter&0x100000U)?TEX_FILTER_DITHER_DIFFUSION:0));
    const auto footprint=mode==5 && (cropped[0]!=target[0] || cropped[1]!=target[1])?
        box_footprint({cropped[0],cropped[1]},{target[0],target[1]}):std::nullopt;
    std::vector<std::vector<float>> slices;
    slices.reserve(cropped[2]);
    for(unsigned z=box[2];z<box[5];++z){
        Image input{size[0],size[1],sf,row,slice,const_cast<std::uint8_t*>(source.data()+slice*z)};
        ScratchImage decoded,converted,resized;
        if(IsCompressed(sf)){checked(Decompress(input,DXGI_FORMAT_UNKNOWN,decoded));input=*decoded.GetImage(0,0,0);}
        if(input.format!=DXGI_FORMAT_R32G32B32A32_FLOAT || input_flags){checked(Convert(input,DXGI_FORMAT_R32G32B32A32_FLOAT,input_flags,TEX_THRESHOLD_DEFAULT,converted));input=*converted.GetImage(0,0,0);}
        std::vector<float> pixels(static_cast<size_t>(cropped[0])*cropped[1]*4);
        if(!box[0] && input.rowPitch==static_cast<size_t>(cropped[0])*16)
            std::memcpy(pixels.data(),input.pixels+box[1]*input.rowPitch,pixels.size()*sizeof(float));
        else for(unsigned y=0;y<cropped[1];++y)std::memcpy(pixels.data()+static_cast<size_t>(y)*cropped[0]*4,input.pixels+(box[1]+y)*input.rowPitch+box[0]*16,static_cast<size_t>(cropped[0])*16);
        Image cropped_image{cropped[0],cropped[1],DXGI_FORMAT_R32G32B32A32_FLOAT,static_cast<size_t>(cropped[0])*16,pixels.size()*4,reinterpret_cast<std::uint8_t*>(pixels.data())};
        if(cropped[0]!=target[0] || cropped[1]!=target[1]){
            if(mode==5){
                // 非正方形 mip 尾端會出現 2×1 → 1×1；DirectXTex Resize 的
                // BOX 僅接受兩軸同時減半，故在共用浮點資料上計算實際面積權重。
                std::vector<float> filtered(static_cast<size_t>(target[0])*target[1]*4);
                const double sx=double(cropped[0])/target[0],sy=double(cropped[1])/target[1];
                if(footprint){
                    const auto& horizontal=(*footprint)[0];const auto& vertical=(*footprint)[1];
                    for(unsigned y=0;y<target[1];++y)for(unsigned x=0;x<target[0];++x){
                        const auto row_range=vertical.ranges[y],column=horizontal.ranges[x];
                        for(unsigned iy=row_range.first;iy<row_range.last;++iy)for(unsigned ix=column.first;ix<column.last;++ix){
                            // 保持原有 double 乘除、float 取整與累加順序。
                            const auto weight=static_cast<float>(horizontal.weights[column.offset+ix-column.first]*vertical.weights[row_range.offset+iy-row_range.first]/(sx*sy));
                            for(unsigned c=0;c<4;++c)filtered[(static_cast<size_t>(y)*target[0]+x)*4+c]+=pixels[(static_cast<size_t>(iy)*cropped[0]+ix)*4+c]*weight;
                        }
                    }
                }else{
                for(unsigned y=0;y<target[1];++y)for(unsigned x=0;x<target[0];++x){
                    const double left=x*sx,right=(x+1)*sx,top=y*sy,bottom=(y+1)*sy;
                    for(unsigned iy=static_cast<unsigned>(std::floor(top));iy<std::min(cropped[1],static_cast<unsigned>(std::ceil(bottom)));++iy)
                        for(unsigned ix=static_cast<unsigned>(std::floor(left));ix<std::min(cropped[0],static_cast<unsigned>(std::ceil(right)));++ix){
                            const auto weight=static_cast<float>((std::min(right,ix+1.0)-std::max(left,double(ix)))*(std::min(bottom,iy+1.0)-std::max(top,double(iy)))/(sx*sy));
                            for(unsigned c=0;c<4;++c)filtered[(static_cast<size_t>(y)*target[0]+x)*4+c]+=pixels[(static_cast<size_t>(iy)*cropped[0]+ix)*4+c]*weight;
                        }
                }
                }
                pixels=std::move(filtered);
            }else{
            checked(Resize(cropped_image,target[0],target[1],static_cast<TEX_FILTER_FLAGS>(flags),resized));
            const auto& image=*resized.GetImage(0,0,0);pixels.resize(static_cast<size_t>(target[0])*target[1]*4);
            for(unsigned y=0;y<target[1];++y)std::memcpy(pixels.data()+static_cast<size_t>(y)*target[0]*4,image.pixels+y*image.rowPitch,static_cast<size_t>(target[0])*16);
            }
        }
        slices.push_back(std::move(pixels));
    }
    std::vector<std::uint8_t> result;
    const auto edge=[&](int z){const auto n=static_cast<int>(cropped[2]);if(filter&0x40000U){z%=2*n;if(z<0)z+=2*n;return z<n?z:2*n-1-z;}return std::clamp(z,0,n-1);};
    for(unsigned z=0;z<target[2];++z){
        std::vector<float> pixels(static_cast<size_t>(target[0])*target[1]*4);
        const double scale=static_cast<double>(cropped[2])/target[2],center=(z+0.5)*scale-0.5;
        const auto blend=[&](int layer,double weight){const auto& values=slices.at(static_cast<size_t>(edge(layer)));for(size_t i=0;i<pixels.size();++i)pixels[i]+=values[i]*static_cast<float>(weight);};
        if(mode<=2 || cropped[2]==target[2])blend(static_cast<int>(std::floor(center+0.5)),1);
        else if(mode==5){const double lo=z*scale,hi=(z+1)*scale;for(int i=static_cast<int>(std::floor(lo));i<static_cast<int>(std::ceil(hi));++i)blend(i,(std::min(hi,i+1.0)-std::max(lo,static_cast<double>(i)))/scale);}
        else {const double radius=mode==4?std::max(1.0,scale):1.0;double sum=0;for(int i=static_cast<int>(std::ceil(center-radius));i<=static_cast<int>(std::floor(center+radius));++i)sum+=std::max(0.0,1-std::abs(i-center)/radius);for(int i=static_cast<int>(std::ceil(center-radius));i<=static_cast<int>(std::floor(center+radius));++i)blend(i,std::max(0.0,1-std::abs(i-center)/radius)/sum);}
        Image input{target[0],target[1],DXGI_FORMAT_R32G32B32A32_FLOAT,static_cast<size_t>(target[0])*16,pixels.size()*4,reinterpret_cast<std::uint8_t*>(pixels.data())};
        ScratchImage encoded;
        if(IsCompressed(df))checked(Compress(input,df,static_cast<TEX_COMPRESS_FLAGS>((filter&0x400000U)?TEX_COMPRESS_SRGB_OUT:0),TEX_THRESHOLD_DEFAULT,encoded));
        else if(df!=input.format || output_flags)checked(Convert(input,df,output_flags,TEX_THRESHOLD_DEFAULT,encoded));
        if(encoded.GetImageCount())input=*encoded.GetImage(0,0,0);
        result.insert(result.end(),input.pixels,input.pixels+input.slicePitch);
    }
    return result;
}
std::vector<std::uint8_t> normal_map(std::span<const std::uint8_t> source,
    std::uint32_t source_format,std::uint32_t width,std::uint32_t height,
    std::uint32_t target_format,std::uint32_t flags,std::uint32_t channel,float amplitude){
    using namespace DirectX;
    valid(width && height && std::isfinite(amplitude) && !(flags&~0x1b0000U));
    unsigned mode=channel==1?CNMAP_CHANNEL_RED:channel==2?CNMAP_CHANNEL_BLUE:channel==4?CNMAP_CHANNEL_GREEN:channel==8?CNMAP_CHANNEL_ALPHA:channel==16?CNMAP_CHANNEL_LUMINANCE:0;
    valid(mode!=0);mode|=(flags&0x30000U)>>4;if(flags&0x80000U)mode|=CNMAP_INVERT_SIGN;if(flags&0x100000U)mode|=CNMAP_COMPUTE_OCCLUSION;
    const std::array size{width,height,1U};const std::array box{0U,0U,0U,width,height,1U};
    auto rgba=transform(source,source_format,size,box,2,size,1);
    Image input{width,height,DXGI_FORMAT_R32G32B32A32_FLOAT,std::size_t(width)*16,rgba.size(),rgba.data()};
    ScratchImage output;checked(ComputeNormalMap(input,static_cast<CNMAP_FLAGS>(mode),amplitude,DXGI_FORMAT_R32G32B32A32_FLOAT,output));
    const auto* image=output.GetImage(0,0,0);valid(image!=nullptr);
    return transform(std::span(image->pixels,image->slicePitch),2,size,box,target_format,size,1);
}
}

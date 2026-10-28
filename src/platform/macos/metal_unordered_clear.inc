// UAV 清除值的元素位元樣式：依檢視格式把清除值轉成一個元素的位元組（size 為元素大小）。只讀資源描述（建立後不變），
// 命令執行緒代理在呼叫端以它判定可否清除，再把清除排入編碼執行緒。格式或值不相容時回傳 false。
bool metal_unordered_clear_pattern(const GraphicsView& view,const std::array<std::uint32_t,4>& values,bool floating,std::array<std::uint8_t,16>& pattern,unsigned& size){
    if(!view.resource)return false;
    const auto& desc=view.resource->desc;const auto format=view.format?view.format:desc.format;
    const bool untyped=desc.dimension==1 && (!format || (view.flags&1));
    if(floating && untyped)return false;
    unsigned channels=1,bits=32,kind=0; // 0 整數、1 浮點、2 UNORM、3 SNORM。
    if(!untyped)switch(format){
    case 2:channels=4;kind=1;break;case 3:case 4:channels=4;break;
    case 10:channels=4;bits=16;kind=1;break;case 11:channels=4;bits=16;kind=2;break;case 12:case 14:channels=4;bits=16;break;case 13:channels=4;bits=16;kind=3;break;
    case 16:channels=2;kind=1;break;case 17:case 18:channels=2;break;
    case 24:channels=4;bits=10;kind=2;break;case 26:channels=3;bits=11;kind=1;break;
    case 28:case 87:case 88:channels=4;bits=8;kind=2;break;case 30:case 32:channels=4;bits=8;break;case 31:channels=4;bits=8;kind=3;break;
    case 34:channels=2;bits=16;kind=1;break;case 35:channels=2;bits=16;kind=2;break;case 36:case 38:channels=2;bits=16;break;case 37:channels=2;bits=16;kind=3;break;
    case 41:kind=1;break;case 42:case 43:break;
    case 49:channels=2;bits=8;kind=2;break;case 50:case 52:channels=2;bits=8;break;case 51:channels=2;bits=8;kind=3;break;
    case 54:bits=16;kind=1;break;case 56:bits=16;kind=2;break;case 57:case 59:bits=16;break;case 58:bits=16;kind=3;break;
    case 61:bits=8;kind=2;break;case 62:case 64:bits=8;break;case 63:bits=8;kind=3;break;
    default:return false;
    }
    if(floating && !kind)return false;
    pattern={};size=untyped?4U:graphics_format_bytes(format);
    for(unsigned channel=0,offset=0;channel<channels;++channel){
        const unsigned width=format==24 && channel==3?2U:format==26 && channel==2?10U:bits;
        auto value=values[channel];
        if(floating){
            auto number=std::bit_cast<float>(value);
            if(kind==1){if(format==26)value=pack_unsigned_float(number,width-5);else if(bits==16){const __fp16 half=static_cast<__fp16>(number);std::uint16_t packed=0;std::memcpy(&packed,&half,2);value=packed;}}
            else {if(std::isnan(number))number=0;const auto high=(1U<<(kind==3?width-1:width))-1U;const auto scaled=std::round(static_cast<double>(std::clamp(number,kind==3?-1.0F:0.0F,1.0F))*high);value=static_cast<std::uint32_t>(static_cast<std::int32_t>(scaled));}
        }
        if(format==24 || format==26){std::uint32_t packed=0;std::memcpy(&packed,pattern.data(),4);packed|=(value&((1U<<width)-1U))<<offset;std::memcpy(pattern.data(),&packed,4);offset+=width;}
        else {const auto lane=(format==87 || format==88) && channel<3?2U-channel:channel;std::memcpy(pattern.data()+lane*(bits/8),&value,bits/8);}
    }
    if(format==88)pattern[3]=255;
    return desc.dimension!=1 || (view.count && view.count%size==0);
}
constexpr std::size_t metal_clear_source_limit=std::size_t{4}<<20;
// X64ARM_METAL_GPU_UAV_CLEAR=0：回到 CPU 組像素再上傳的舊路徑（紋理寫入前等 GPU 用完目標），對照用。
bool metal_gpu_unordered_clear(){static const bool value=[] {const auto* text=std::getenv("X64ARM_METAL_GPU_UAV_CLEAR");return !text || std::string_view(text)!="0";}();return value;}
// 清除來源：依（樣式, 元素大小）保留一份以樣式重複填滿的共享緩衝，只在需要更大時重建（舊緩衝由已編碼的命令緩衝保留）。
// 上限 4 MiB，更大的目標分段複製；最多保留 8 種樣式，淘汰最久未用者。
id<MTLBuffer> MetalGl::metal_clear_source(const std::array<std::uint8_t,16>& pattern,unsigned size,std::size_t bytes){
    bytes=std::min<std::size_t>(std::max<std::size_t>(bytes,size),metal_clear_source_limit);bytes=(bytes+size-1)/size*size;
    ClearSource* slot=nullptr;
    for(auto& entry:clear_sources_)if(entry.size==size && entry.pattern==pattern){slot=&entry;break;}
    if(slot && slot->buffer.length>=bytes){slot->used=++clear_source_clock_;return slot->buffer;}
    if(!slot){
        if(clear_sources_.size()<8)slot=&clear_sources_.emplace_back();
        else slot=&*std::min_element(clear_sources_.begin(),clear_sources_.end(),[](const auto& a,const auto& b){return a.used<b.used;});
    }
    // 依需求成長到 64 KiB 倍數（仍為元素大小的倍數），避免逐次加大。
    std::size_t length=std::min<std::size_t>(std::max<std::size_t>(bytes,std::size_t{64}<<10),metal_clear_source_limit);length=length/size*size;
    id<MTLBuffer> buffer=[device_ newBufferWithLength:length options:MTLResourceStorageModeShared];if(!buffer)throw std::bad_alloc{};
    auto* out=static_cast<std::uint8_t*>(buffer.contents);for(std::size_t at=0;at<length;at+=size)std::memcpy(out+at,pattern.data(),size);
    slot->pattern=pattern;slot->size=size;slot->buffer=buffer;slot->used=++clear_source_clock_;return buffer;
}
bool MetalGl::graphics_clear_unordered(const GraphicsView& view,std::array<std::uint32_t,4> values,bool floating){@autoreleasepool{
    std::array<std::uint8_t,16> pattern{};unsigned size=0;
    if(!metal_unordered_clear_pattern(view,values,floating,pattern,size))return false;
    const auto& desc=view.resource->desc;
    if(metal_gpu_unordered_clear()){
        auto& r=metal_resource(view.resource);
        const bool uniform=std::all_of(pattern.begin(),pattern.begin()+size,[&](std::uint8_t b){return b==pattern[0];});
        if(r.buffer && view.first%4==0 && view.count%4==0 && std::uint64_t{view.first}+view.count<=r.buffer.length){
            // 緩衝：依命令順序以 blit 填值（單一位元組樣式）或從清除來源分段複製。
            begin_commands(true,"clear_unordered_buffer");auto e=[commands_ blitCommandEncoder];MetalEncoderScope scope{e};
            if(uniform)[e fillBuffer:r.buffer range:NSMakeRange(view.first,view.count) value:pattern[0]];
            else{
                auto source=metal_clear_source(pattern,size,view.count);const auto chunk=source.length/size*size;
                for(std::uint64_t at=0;at<view.count;at+=chunk)[e copyFromBuffer:source sourceOffset:0 toBuffer:r.buffer destinationOffset:view.first+at size:std::min<std::uint64_t>(chunk,view.count-at)];
            }
            scope.end();r.constant_bytes_valid=false;graphics_pending_.push_back(view.resource);return true;
        }
        if(r.texture && desc.dimension!=1 && r.texture.sampleCount==1 && !metal_depth_format(r.texture.pixelFormat) && !(desc.format>=5 && desc.format<=8)){
            // 紋理：清除來源視為列距＝寬×元素大小的影像，依列分段 blit 到每個切片（3D 為深度範圍），不等 GPU 用完目標。
            const auto w=std::max(1U,desc.width>>view.level),h=std::max(1U,desc.height>>view.level),z=std::max(1U,desc.depth>>view.level);
            const std::size_t row=std::size_t{w}*size;
            if(row<=metal_clear_source_limit){
                auto source=metal_clear_source(pattern,size,row*h);const auto rows=std::max<std::size_t>(1,std::min<std::size_t>(h,source.length/row));
                begin_commands(true,"clear_unordered_texture");auto e=[commands_ blitCommandEncoder];MetalEncoderScope scope{e};
                const auto fill=[&](unsigned slice,unsigned depth){
                    for(unsigned y=0;y<h;y+=static_cast<unsigned>(rows)){
                        const auto band=static_cast<unsigned>(std::min<std::size_t>(rows,h-y));
                        [e copyFromBuffer:source sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:desc.dimension==4?row*band:0 sourceSize:MTLSizeMake(w,band,1)
                                toTexture:r.texture destinationSlice:slice destinationLevel:view.level destinationOrigin:MTLOriginMake(0,y,depth)];
                    }
                };
                if(desc.dimension==4){for(unsigned d=view.slice;d<std::min(z,view.slice+view.slices);++d)fill(0,d);}
                else for(unsigned slice=0;slice<view.slices;++slice)fill(view.slice+slice,0);
                scope.end();graphics_pending_.push_back(view.resource);return true;
            }
        }
    }
    if(desc.dimension==1){
        std::vector<std::uint8_t> pixels(view.count);for(std::size_t at=0;at<pixels.size();at+=size)std::copy_n(pattern.begin(),size,pixels.begin()+static_cast<std::ptrdiff_t>(at));
        graphics_write(view,pixels,0,0);
    }else{
        const auto w=std::max(1U,desc.width>>view.level),h=std::max(1U,desc.height>>view.level),z=std::max(1U,desc.depth>>view.level);
        const auto depth=desc.dimension==4?view.slices:z;
        std::vector<std::uint8_t> pixels(std::size_t{w}*h*depth*size);for(std::size_t at=0;at<pixels.size();at+=size)std::copy_n(pattern.begin(),size,pixels.begin()+static_cast<std::ptrdiff_t>(at));
        if(desc.dimension==4){auto region=view;region.slice=0;graphics_write_region(region,pixels,w*size,w*h*size,{0,0,view.slice,w,h,view.slice+view.slices});}
        else for(unsigned slice=0;slice<view.slices;++slice){auto layer=view;layer.slice+=slice;graphics_write(layer,pixels,w*size,w*h*size);}
    }
    return true;
}}

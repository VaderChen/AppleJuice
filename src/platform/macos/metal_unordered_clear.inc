bool MetalGl::graphics_clear_unordered(const GraphicsView& view,std::array<std::uint32_t,4> values,bool floating){@autoreleasepool{
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
    std::array<std::uint8_t,16> pattern{};const unsigned size=untyped?4U:graphics_format_bytes(format);
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
    if(desc.dimension==1){
        if(!view.count || view.count%size)return false;
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

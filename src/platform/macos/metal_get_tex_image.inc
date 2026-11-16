// Metal 後端的 glGetTexImage（opengl_metal.mm 引入）：等 GPU 寫入完成後讀回 GL_TEXTURE_1D／2D 指定層級，依儲存的 Metal 像素格式
// 解成 RGBA（缺少的分量為 0、0、0、1，與規格把紋理分量對應到 R、G、B、A 的規則相同；RGB 內部格式的 alpha 已固定為 1），
// 再交給 pack_pixels（metal_pixel_transfer.inc，與 glReadPixels 共用）依 format／type 與 GL_PACK_* 寫入；依規格不套用像素傳輸。深度紋理只能以 GL_DEPTH_COMPONENT 讀回。
std::uint64_t MetalGl::get_tex_image(std::span<const std::uint64_t> a,std::span<std::uint8_t> output){
    const auto target=static_cast<unsigned>(a[0]),level=static_cast<unsigned>(a[1]),format=static_cast<unsigned>(a[2]),type=static_cast<unsigned>(a[3]);
    auto* bound=target==0xde1 || target==0xde0?bound_texture(target):nullptr;if(!bound){error(0x500);return 0;}
    if(level>14){error(0x501);return 0;}
    // GL_RED 0x1903、GREEN、BLUE、ALPHA、GL_LUMINANCE 0x1909、GL_DEPTH_COMPONENT 0x1902、GL_LUMINANCE_ALPHA 0x190A、GL_RG 0x8227、GL_RGB／BGR、GL_RGBA／BGRA。
    if(!format_components(format) || format==0x1900 || format==0x1901){error(0x500);return 0;}
    // 型別：BYTE～FLOAT 與 GL 1.2 包裝型別（分量數須相符）；GL_BITMAP 只用於索引格式。
    if(type==0x1a00){error(0x500);return 0;}if(const auto e=pixel_format_error(format,type)){error(e);return 0;}
    auto& t=*bound;if(!t.native || !t.formats.contains(level) || level>=t.native.mipmapLevelCount)return 0;
    const auto pixel_format=t.native.pixelFormat;if((format==0x1902)!=(pixel_format==MTLPixelFormatDepth32Float)){error(0x502);return 0;}
    unsigned texel=0;
    switch(pixel_format){
    case MTLPixelFormatR8Unorm:texel=1;break;case MTLPixelFormatRG8Unorm:case MTLPixelFormatR16Unorm:case MTLPixelFormatR16Float:texel=2;break;
    case MTLPixelFormatRGBA8Unorm:case MTLPixelFormatRG16Unorm:case MTLPixelFormatRG11B10Float:case MTLPixelFormatR32Float:case MTLPixelFormatRG16Float:case MTLPixelFormatDepth32Float:texel=4;break;
    case MTLPixelFormatRGBA16Unorm:case MTLPixelFormatRG32Float:case MTLPixelFormatRGBA16Float:texel=8;break;case MTLPixelFormatRGBA32Float:texel=16;break;
    default:return 0;}
    flush(true);
    const auto width=static_cast<unsigned>(std::max<NSUInteger>(1,t.native.width>>level)),height=static_cast<unsigned>(std::max<NSUInteger>(1,t.native.height>>level));
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(width)*height*texel);
    [t.native getBytes:raw.data() bytesPerRow:static_cast<NSUInteger>(width)*texel fromRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:level];
    const auto decode=[&](const std::uint8_t* p){
        std::array<float,4> v{0,0,0,1};const auto u16=[&](unsigned i){std::uint16_t x;std::memcpy(&x,p+i*2,2);return x/65535.F;};
        const auto f16=[&](unsigned i){__fp16 x;std::memcpy(&x,p+i*2,2);return static_cast<float>(x);};const auto f32=[&](unsigned i){float x;std::memcpy(&x,p+i*4,4);return x;};
        switch(pixel_format){
        case MTLPixelFormatRGBA8Unorm:for(unsigned c=0;c<4;++c)v[c]=p[c]/255.F;break;
        case MTLPixelFormatR8Unorm:v[0]=p[0]/255.F;break;case MTLPixelFormatRG8Unorm:v[0]=p[0]/255.F;v[1]=p[1]/255.F;break;
        case MTLPixelFormatR16Unorm:v[0]=u16(0);break;case MTLPixelFormatRG16Unorm:v[0]=u16(0);v[1]=u16(1);break;case MTLPixelFormatRGBA16Unorm:for(unsigned c=0;c<4;++c)v[c]=u16(c);break;
        case MTLPixelFormatRG11B10Float:{std::uint32_t word;std::memcpy(&word,p,4);for(unsigned c=0;c<3;++c)v[c]=unpack_rg11b10(word,c);break;}
        case MTLPixelFormatR16Float:v[0]=f16(0);break;case MTLPixelFormatRG16Float:v[0]=f16(0);v[1]=f16(1);break;case MTLPixelFormatRGBA16Float:for(unsigned c=0;c<4;++c)v[c]=f16(c);break;
        case MTLPixelFormatR32Float:case MTLPixelFormatDepth32Float:v[0]=f32(0);break;case MTLPixelFormatRG32Float:v[0]=f32(0);v[1]=f32(1);break;case MTLPixelFormatRGBA32Float:for(unsigned c=0;c<4;++c)v[c]=f32(c);break;
        default:break;}
        return v;
    };
    std::vector<float> values(static_cast<std::size_t>(width)*height*4);
    for(std::size_t i=0;i<static_cast<std::size_t>(width)*height;++i){const auto value=decode(raw.data()+i*texel);std::copy(value.begin(),value.end(),values.begin()+static_cast<std::ptrdiff_t>(i*4));}
    return pack_pixels(values,4,width,height,format,type,false,output);
}

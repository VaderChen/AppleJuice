// Metal 後端的點畫線與多邊形點畫（opengl_metal.mm 引入）。
// glPolygonStipple 依 GL_UNPACK_* 解出 32×32 位元（與 glBitmap 共用解碼），片段著色器以 GL 視窗座標（左下原點）取 x、y 的低 5 位元查表；
// glLineStipple 在線段組裝時以主軸像素數累計每個頂點的計數（GL_LINES 每段重新開始，連續線與封閉線累加），片段著色器依 factor 與 16 位元樣式判斷。
// glClear、glBitmap 的片段不點畫；POLYGON_MODE 為 LINE 時的邊與選取／回饋模式不受影響。
std::uint64_t MetalGl::stipple(GlOperation op,std::span<const std::uint64_t> a,std::span<const std::uint8_t> data,std::span<std::uint8_t> output){
    if(op==GlOperation::LineStipple){
        // factor 依規格夾在 1～256，樣式取低 16 位元。
        state_.line_stipple_factor=static_cast<unsigned>(std::clamp(std::bit_cast<std::int32_t>(static_cast<unsigned>(a[0])),1,256));
        state_.line_stipple_pattern=static_cast<unsigned>(a[1])&0xffffU;return 0;
    }
    if(op==GlOperation::PolygonStipple){
        const auto bits=bitmap_bits({},data,32,32);
        for(unsigned y=0;y<32;++y){std::uint32_t row=0;for(unsigned x=0;x<32;++x)if(bits[y*32+x])row|=1U<<x;state_.polygon_stipple[y]=row;}
        return 0;
    }
    // GetPolygonStipple：依 GL_PACK_*（LSB_FIRST、列長、略過、對齊）寫入位元；其他位元保留服務層預先讀入的客體內容。
    const auto value=[&](unsigned name){const auto it=pixel_.find(name);return it==pixel_.end()?0:it->second;};
    const bool lsb=value(0xd01)!=0;const std::size_t row=value(0xd02)>0?static_cast<std::size_t>(value(0xd02)):32,skip_rows=static_cast<std::size_t>(std::max(0,value(0xd03))),skip_pixels=static_cast<std::size_t>(std::max(0,value(0xd04)));
    const std::size_t align=static_cast<std::size_t>(std::max(1,value(0xd05)));const std::size_t stride=((row+7)/8+align-1)/align*align;
    for(std::size_t y=0;y<32;++y)for(std::size_t x=0;x<32;++x){
        const auto bit=skip_pixels+x;const auto at=(skip_rows+y)*stride+bit/8;if(at>=output.size())throw std::invalid_argument("polygon stipple bounds");
        const auto mask=static_cast<std::uint8_t>(lsb?1U<<(bit%8):0x80U>>(bit%8));
        if((state_.polygon_stipple[y]>>x)&1U)output[at]|=mask;else output[at]&=static_cast<std::uint8_t>(~mask);
    }
    return output.size();
}
void MetalGl::line_stipple_counters(std::vector<Vertex>& pairs,bool independent){
    // 每對頂點是一段；以視窗座標的主軸長度（max(|dx|,|dy|)）作為片段數，計數放在 shininess.y 供著色器線性內插。
    const auto& mv=matrices_[0].back();const auto& projection=matrices_[1].back();const auto& v=state_.viewport;
    const auto window=[&](const Vertex& vertex){const auto clip=transform(projection,transform(mv,vertex.position));const float w=clip[3]!=0?clip[3]:1;
        return std::array<float,2>{(clip[0]/w+1)*static_cast<float>(v[2])/2+static_cast<float>(v[0]),(clip[1]/w+1)*static_cast<float>(v[3])/2+static_cast<float>(v[1])};};
    float counter=0;
    for(std::size_t i=0;i+1<pairs.size();i+=2){
        const auto a=window(pairs[i]),b=window(pairs[i+1]);const float length=std::max(std::abs(b[0]-a[0]),std::abs(b[1]-a[1]));
        if(independent)counter=0;
        pairs[i].shininess[1]=counter;pairs[i+1].shininess[1]=counter+length;counter+=length;
    }
}
// 片段著色器 buffer(3)：[0] 位元 0＝多邊形點畫、位元 1＝點畫線，[1] 畫面高度，[2] 線樣式，[3] 線 factor，[4..35] 多邊形 32 列。
std::array<std::uint32_t,36> MetalGl::stipple_uniform(MTLPrimitiveType primitive,bool clearing)const{
    std::array<std::uint32_t,36> result{};
    const bool polygon=!clearing && primitive==MTLPrimitiveTypeTriangle && gl_.polygon_mode!=0x1b01 && state_.enabled.contains(0xb42);
    const bool line=!clearing && (primitive==MTLPrimitiveTypeLine || primitive==MTLPrimitiveTypeLineStrip) && state_.enabled.contains(0xb24);
    result[0]=(polygon?1U:0U)|(line?2U:0U);result[1]=height_;result[2]=state_.line_stipple_pattern;result[3]=state_.line_stipple_factor;
    std::copy(state_.polygon_stipple.begin(),state_.polygon_stipple.end(),result.begin()+4);
    return result;
}

// Metal 後端的像素傳輸、glDrawPixels 與 glCopyPixels（opengl_metal.mm 引入）。
// 狀態：glPixelTransfer 的縮放／偏移、索引位移與 MAP_COLOR／MAP_STENCIL 屬 GL_PIXEL_MODE_BIT；十張像素表（預設各 1 項、值 0）不屬任何屬性群組。
// 影像依 GL_UNPACK_* 解包並換成浮點：色彩分量先展開成 RGBA（缺少的 R、G、B 為 0，A 為 1），再做縮放／偏移、MAP_COLOR 查表與 [0,1] 夾限；
// 色彩索引經位移後以 I_TO_R／G／B／A 換成 RGBA（本後端為 RGBA 像素格式）；模板索引經位移與 MAP_STENCIL 查 S_TO_S；深度經縮放／偏移後夾限。
// 繪製時把影像放進暫存紋理，以光柵位置與 glPixelZoom 換算的視窗矩形畫一個四邊形，片段著色器依視窗座標取影像像素：
// 色彩影像取代片段色彩，之後照常經過紋理環境、霧化、alpha／模板／深度測試與混色；深度影像輸出片段深度並帶光柵色彩；
// 模板影像只受剪裁測試與模板寫入遮罩限制，直接寫入模板值。glCopyPixels 讀出 framebuffer 區域後走同一條路徑，glReadPixels 也套用同一組傳輸。
namespace {
constexpr std::size_t max_pixel_map=256;
constexpr std::uint64_t canonical_pixels=0x43414e4f4e504958ULL;
// 包裝型別的分量位元數（依格式分量順序）；rev 為 true 時第一個分量位於最低位元，否則位於最高位元。
struct PackedLayout {unsigned bytes{},count{};std::array<unsigned,4> bits{};bool rev{};};
std::optional<PackedLayout> packed_layout(unsigned type){
    switch(type){
    case 0x8032:return PackedLayout{1,3,{3,3,2,0},false};case 0x8362:return PackedLayout{1,3,{3,3,2,0},true};
    case 0x8363:return PackedLayout{2,3,{5,6,5,0},false};case 0x8364:return PackedLayout{2,3,{5,6,5,0},true};
    case 0x8033:return PackedLayout{2,4,{4,4,4,4},false};case 0x8365:return PackedLayout{2,4,{4,4,4,4},true};
    case 0x8034:return PackedLayout{2,4,{5,5,5,1},false};case 0x8366:return PackedLayout{2,4,{5,5,5,1},true};
    case 0x8035:return PackedLayout{4,4,{8,8,8,8},false};case 0x8367:return PackedLayout{4,4,{8,8,8,8},true};
    case 0x8036:return PackedLayout{4,4,{10,10,10,2},false};case 0x8368:return PackedLayout{4,4,{10,10,10,2},true};
    default:return std::nullopt;}
}
unsigned format_components(unsigned format){
    switch(format){case 0x1900:case 0x1901:case 0x1902:case 0x1903:case 0x1904:case 0x1905:case 0x1906:case 0x1909:return 1;case 0x190a:case 0x8227:return 2;
    case 0x1907:case 0x80e0:return 3;case 0x1908:case 0x80e1:return 4;default:return 0;}
}
unsigned scalar_size(unsigned type){switch(type){case 0x1400:case 0x1401:return 1;case 0x1402:case 0x1403:return 2;case 0x1404:case 0x1405:case 0x1406:return 4;default:return 0;}}
// 格式與型別的組合檢查：0 表示可用，否則為 GL 錯誤碼。GL_BITMAP 只用於索引格式；包裝型別的分量數須與 RGB 或 RGBA／BGRA 相符。
unsigned pixel_format_error(unsigned format,unsigned type){
    const auto components=format_components(format);if(!components)return 0x500;
    const bool index=format==0x1900 || format==0x1901;
    if(type==0x1a00)return index?0:0x500;
    if(scalar_size(type))return 0;
    const auto packed=packed_layout(type);if(!packed)return 0x500;
    if(packed->count!=components)return 0x502;
    return (components==3?format==0x1907:(format==0x1908 || format==0x80e1))?0:0x502;
}
// 色彩與深度元素依 GL 4.2 的正規化規則換成浮點（有號型別以最大值相除並夾到 -1）；索引元素保留整數值（浮點取整數部分）。
double normalized_element(std::uint64_t raw,unsigned type){
    switch(type){
    case 0x1401:return static_cast<double>(raw)/255.0;case 0x1400:return std::max(-1.0,static_cast<std::int8_t>(raw)/127.0);
    case 0x1403:return static_cast<double>(raw)/65535.0;case 0x1402:return std::max(-1.0,static_cast<std::int16_t>(raw)/32767.0);
    case 0x1405:return static_cast<double>(raw)/4294967295.0;case 0x1404:return std::max(-1.0,static_cast<std::int32_t>(raw)/2147483647.0);
    default:return std::bit_cast<float>(static_cast<std::uint32_t>(raw));}
}
double index_element(std::uint64_t raw,unsigned type){
    switch(type){
    case 0x1400:return static_cast<std::int8_t>(raw);case 0x1402:return static_cast<std::int16_t>(raw);case 0x1404:return static_cast<std::int32_t>(raw);
    case 0x1406:return std::trunc(static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(raw))));default:return static_cast<double>(raw);}
}
std::array<float,4> expand_rgba(unsigned format,const std::array<double,4>& c){
    const auto f=[&](unsigned i){return static_cast<float>(c[i]);};
    switch(format){
    case 0x1903:return {f(0),0,0,1};case 0x1904:return {0,f(0),0,1};case 0x1905:return {0,0,f(0),1};case 0x1906:return {0,0,0,f(0)};
    case 0x1909:return {f(0),f(0),f(0),1};case 0x190a:return {f(0),f(0),f(0),f(1)};case 0x8227:return {f(0),f(1),0,1};
    case 0x1907:return {f(0),f(1),f(2),1};case 0x80e0:return {f(2),f(1),f(0),1};case 0x80e1:return {f(2),f(1),f(0),f(3)};default:return {f(0),f(1),f(2),f(3)};}
}
}
MetalGl::PixelStore MetalGl::pixel_store(bool pack,unsigned width,bool canonical)const{
    if(canonical)return {false,false,width,0,0,1};
    const auto base=pack?0xd00U:0xcf0U;const auto value=[&](unsigned i){const auto it=pixel_.find(base+i);return it==pixel_.end()?(i==5?4:0):it->second;};
    return {value(0)!=0,value(1)!=0,value(2)>0?static_cast<std::size_t>(value(2)):width,static_cast<std::size_t>(std::max(0,value(3))),static_cast<std::size_t>(std::max(0,value(4))),static_cast<std::size_t>(std::max(1,value(5)))};
}
// 每像素群組的位元組數與列距（GL_BITMAP 以位元計算列寬）；與服務層 pixel_bytes 的大小計算一致。
MetalGl::PixelGeometry MetalGl::pixel_geometry(const PixelStore& store,unsigned format,unsigned type){
    PixelGeometry g;g.bitmap=type==0x1a00;const auto packed=packed_layout(type);
    g.element=packed?packed->bytes:scalar_size(type);g.group=packed?packed->bytes:g.element*format_components(format);
    g.stride=g.bitmap?((store.row+7)/8+store.align-1)/store.align*store.align:(store.row*g.group+store.align-1)/store.align*store.align;
    return g;
}
bool MetalGl::color_transfer_active()const{const auto& t=state_.transfer;return t.map_color || t.scale!=std::array<float,4>{1,1,1,1} || t.bias!=std::array<float,4>{};}
void MetalGl::color_transfer(std::array<float,4>& rgba)const{
    const auto& t=state_.transfer;
    for(unsigned i=0;i<4;++i){auto value=rgba[i]*t.scale[i]+t.bias[i];
        // RGBA→RGBA 查表：夾到 [0,1] 後乘上表大小減一並四捨五入成索引。
        if(t.map_color){const auto& map=pixel_maps_[6+i];value=map[static_cast<std::size_t>(std::lround(std::clamp(value,0.F,1.F)*static_cast<float>(map.size()-1)))];}
        rgba[i]=std::clamp(value,0.F,1.F);}
}
std::int64_t MetalGl::index_transfer(std::int64_t index)const{
    const auto& t=state_.transfer;const auto shifted=t.index_shift>=0?index*(std::int64_t{1}<<std::min(t.index_shift,31)):index>>std::min(-t.index_shift,63);
    return shifted+t.index_offset;
}
std::array<float,4> MetalGl::index_to_rgba(std::int64_t index)const{
    // 索引表大小為 2 的冪次，以低位元遮罩取項目。
    std::array<float,4> rgba{};for(unsigned i=0;i<4;++i){const auto& map=pixel_maps_[2+i];rgba[i]=std::clamp(map[static_cast<std::size_t>(static_cast<std::uint64_t>(index)&(map.size()-1))],0.F,1.F);}
    return rgba;
}
std::int64_t MetalGl::stencil_transfer(std::int64_t index)const{
    auto value=index_transfer(index);
    if(state_.transfer.map_stencil){const auto& map=pixel_maps_[1];value=std::llround(map[static_cast<std::size_t>(static_cast<std::uint64_t>(value)&(map.size()-1))]);}
    return value;
}
float MetalGl::depth_transfer(float depth)const{return std::clamp(depth*state_.transfer.depth_scale+state_.transfer.depth_bias,0.F,1.F);}
std::uint64_t MetalGl::pixel_transfer(GlOperation op,std::span<const std::uint64_t> a,std::span<const std::uint8_t> data,std::span<std::uint8_t> output){
    const auto pname=static_cast<unsigned>(a[0]);
    if(op==GlOperation::PixelTransfer){
        const auto value=std::bit_cast<double>(a[1]);auto& t=state_.transfer;
        const auto integer=[&]{return static_cast<int>(std::clamp(std::round(value),-2147483648.0,2147483647.0));};
        switch(pname){
        case 0xd10:t.map_color=value!=0;break;case 0xd11:t.map_stencil=value!=0;break;case 0xd12:t.index_shift=integer();break;case 0xd13:t.index_offset=integer();break;
        case 0xd14:case 0xd18:case 0xd1a:case 0xd1c:t.scale[pname==0xd14?0:(pname-0xd16)/2]=static_cast<float>(value);break;
        case 0xd15:case 0xd19:case 0xd1b:case 0xd1d:t.bias[pname==0xd15?0:(pname-0xd17)/2]=static_cast<float>(value);break;
        case 0xd1e:t.depth_scale=static_cast<float>(value);break;case 0xd1f:t.depth_bias=static_cast<float>(value);break;
        default:error(0x500);break;}
        return 0;
    }
    if(pname<0xc70 || pname>0xc79){error(0x500);return 0;}
    // 0xc70 I_TO_I、0xc71 S_TO_S、0xc72～0xc75 I_TO_R～A、0xc76～0xc79 R_TO_R～A_TO_A；後八張是色彩表。
    auto& map=pixel_maps_[pname-0xc70];const bool color=pname>=0xc72;const auto element=static_cast<unsigned>(a[op==GlOperation::PixelMap?2:1]);
    const unsigned width=element==0x1403?2U:4U;
    if(op==GlOperation::PixelMap){
        // 索引來源的表（I_TO_*、S_TO_S）大小須為 2 的冪次；色彩表的值夾到 [0,1]，整數版本依型別最大值換算。
        const auto size=static_cast<std::size_t>(a[1]);
        if(size<1 || size>max_pixel_map || (pname<=0xc75 && (size&(size-1)))){error(0x501);return 0;}
        if(data.size()<size*width)throw std::invalid_argument("pixel map bounds");
        std::vector<float> values(size);
        for(std::size_t i=0;i<size;++i){
            std::uint32_t raw=0;std::memcpy(&raw,data.data()+i*width,width);
            const double value=element==0x1406?static_cast<double>(std::bit_cast<float>(raw)):element==0x1405?(color?raw/4294967295.0:raw):(color?raw/65535.0:raw);
            values[i]=static_cast<float>(color?std::clamp(value,0.0,1.0):value);
        }
        map=std::move(values);return 0;
    }
    // GetPixelMap：整數版本的色彩表依型別最大值換算，索引表直接取整數。
    const auto bytes=map.size()*width;if(output.size()<bytes)throw std::invalid_argument("get pixel map bounds");
    for(std::size_t i=0;i<map.size();++i){const double value=map[i];
        if(element==0x1406){const float x=map[i];std::memcpy(output.data()+i*4,&x,4);}
        else if(element==0x1405){const auto x=static_cast<std::uint32_t>(color?std::llround(std::clamp(value,0.0,1.0)*4294967295.0):static_cast<std::int64_t>(value));std::memcpy(output.data()+i*4,&x,4);}
        else{const auto x=static_cast<std::uint16_t>(color?std::llround(std::clamp(value,0.0,1.0)*65535.0):static_cast<std::int64_t>(value));std::memcpy(output.data()+i*2,&x,2);}
    }
    return bytes;
}
std::optional<std::vector<float>> MetalGl::pixel_transfer_query(unsigned pname)const{
    const auto& t=state_.transfer;
    switch(pname){
    case 0xd10:return std::vector<float>{t.map_color?1.F:0.F};case 0xd11:return std::vector<float>{t.map_stencil?1.F:0.F};
    case 0xd12:return std::vector<float>{static_cast<float>(t.index_shift)};case 0xd13:return std::vector<float>{static_cast<float>(t.index_offset)};
    case 0xd14:case 0xd18:case 0xd1a:case 0xd1c:return std::vector<float>{t.scale[pname==0xd14?0:(pname-0xd16)/2]};
    case 0xd15:case 0xd19:case 0xd1b:case 0xd1d:return std::vector<float>{t.bias[pname==0xd15?0:(pname-0xd17)/2]};
    case 0xd1e:return std::vector<float>{t.depth_scale};case 0xd1f:return std::vector<float>{t.depth_bias};
    case 0xd34:return std::vector<float>{static_cast<float>(max_pixel_map)};
    default:break;}
    if(pname>=0xcb0 && pname<=0xcb9)return std::vector<float>{static_cast<float>(pixel_maps_[pname-0xcb0].size())};
    return std::nullopt;
}
std::optional<MetalGl::PixelImage> MetalGl::unpack_image(unsigned width,unsigned height,unsigned format,unsigned type,std::span<const std::uint8_t> data,bool canonical){
    if(const auto e=pixel_format_error(format,type)){error(e);return std::nullopt;}
    const auto store=pixel_store(false,width,canonical);const auto g=pixel_geometry(store,format,type);
    const unsigned components=format_components(format);const auto packed=packed_layout(type);const bool index=format==0x1900 || format==0x1901;
    PixelImage image{width,height,format==0x1902?2U:format==0x1901?3U:1U,{}};const unsigned channels=image.kind==1?4U:1U;
    image.values.resize(static_cast<std::size_t>(width)*height*channels);
    const auto load=[&](std::size_t at,std::size_t size)->std::uint64_t{
        if(at>data.size() || size>data.size()-at)throw std::invalid_argument("draw pixels bounds");std::uint64_t raw=0;std::memcpy(&raw,data.data()+at,size);
        if(store.swap && size==2)raw=__builtin_bswap16(static_cast<std::uint16_t>(raw));else if(store.swap && size==4)raw=__builtin_bswap32(static_cast<std::uint32_t>(raw));
        return raw;};
    for(std::size_t y=0;y<height;++y)for(std::size_t x=0;x<width;++x){
        std::array<double,4> c{};
        if(g.bitmap){const auto bit=store.skip_pixels+x;c[0]=static_cast<double>((load((store.skip_rows+y)*g.stride+bit/8,1)>>(store.lsb?bit%8:7-bit%8))&1U);}
        else{const auto base=(store.skip_rows+y)*g.stride+(store.skip_pixels+x)*g.group;
            if(packed){const auto raw=load(base,packed->bytes);unsigned shift=packed->rev?0U:packed->bytes*8;
                for(unsigned i=0;i<packed->count;++i){const auto bits=packed->bits[i];if(!packed->rev)shift-=bits;const auto mask=(std::uint64_t{1}<<bits)-1;c[i]=static_cast<double>((raw>>shift)&mask)/static_cast<double>(mask);if(packed->rev)shift+=bits;}}
            else for(unsigned i=0;i<components;++i){const auto raw=load(base+i*g.element,g.element);c[i]=index?index_element(raw,type):normalized_element(raw,type);}}
        auto* out=image.values.data()+(y*width+x)*channels;
        if(format==0x1901){out[0]=static_cast<float>(stencil_transfer(static_cast<std::int64_t>(c[0]))&255);continue;}
        if(format==0x1902){out[0]=depth_transfer(static_cast<float>(c[0]));continue;}
        auto rgba=format==0x1900?index_to_rgba(index_transfer(static_cast<std::int64_t>(c[0]))):expand_rgba(format,c);
        if(format!=0x1900)color_transfer(rgba);
        std::copy(rgba.begin(),rgba.end(),out);
    }
    return image;
}
// 顯示清單記錄 glDrawPixels 時改存緊密排列（對齊 1、不交換位元組、GL_BITMAP 為 MSB 先）的資料，回放不受之後的解包設定影響；args[4] 為 canonical 標記。
std::pair<std::vector<std::uint64_t>,std::vector<std::uint8_t>> MetalGl::pixels_record(std::span<const std::uint64_t> a,std::span<const std::uint8_t> data){
    const auto width=std::bit_cast<std::int32_t>(static_cast<unsigned>(a[0])),height=std::bit_cast<std::int32_t>(static_cast<unsigned>(a[1]));
    const auto format=static_cast<unsigned>(a[2]),type=static_cast<unsigned>(a[3]);
    if(width<=0 || height<=0 || data.empty() || pixel_format_error(format,type))return {{a.begin(),a.end()},{data.begin(),data.end()}};
    const auto store=pixel_store(false,static_cast<unsigned>(width),false);const auto g=pixel_geometry(store,format,type);
    const auto canonical=pixel_store(false,static_cast<unsigned>(width),true);const auto c=pixel_geometry(canonical,format,type);
    std::vector<std::uint8_t> packed(c.stride*static_cast<std::size_t>(height));
    for(std::size_t y=0;y<static_cast<std::size_t>(height);++y){
        if(g.bitmap){for(std::size_t x=0;x<static_cast<std::size_t>(width);++x){const auto bit=store.skip_pixels+x;const auto at=(store.skip_rows+y)*g.stride+bit/8;if(at>=data.size())throw std::invalid_argument("draw pixels bounds");
            if((data[at]>>(store.lsb?bit%8:7-bit%8))&1U)packed[y*c.stride+x/8]|=static_cast<std::uint8_t>(0x80U>>(x%8));}continue;}
        const auto from=(store.skip_rows+y)*g.stride+store.skip_pixels*g.group,count=static_cast<std::size_t>(width)*g.group;
        if(from>data.size() || count>data.size()-from)throw std::invalid_argument("draw pixels bounds");
        auto* row=packed.data()+y*c.stride;std::memcpy(row,data.data()+from,count);
        if(store.swap && g.element>1)for(std::size_t i=0;i+g.element<=count;i+=g.element)std::reverse(row+i,row+i+g.element);
    }
    return {{a[0],a[1],a[2],a[3],canonical_pixels},std::move(packed)};
}
// glBitmap／glDrawPixels／glCopyPixels 共用：在 GL 視窗座標直接產生片段。視埠暫改為整個繪製表面（點陣與像素矩形不受視埠裁切），
// 模型視圖與投影換成把剪裁 z 固定為光柵深度、眼座標 z 為光柵距離（霧化座標）的常數矩陣，紋理矩陣為單位矩陣，片段帶光柵色彩與紋理座標；
// 暫停光照、剔除、多邊形偏移、點畫、使用者剪裁面與紋理座標產生。emit 以 clip(x,y) 把視窗座標換成剪裁座標後送出三角形頂點。
template<class Emit> void MetalGl::window_space(Emit&& emit){
    struct Saved {MetalGl& gl;std::array<Matrix,3> matrices;std::set<unsigned> enabled;Vec color,texcoord;std::array<int,4> viewport;unsigned texgen;
        ~Saved(){for(std::size_t i=0;i<3;++i)gl.matrices_[i].back()=matrices[i];gl.state_.enabled=std::move(enabled);gl.state_.color=color;gl.state_.texcoord=texcoord;gl.state_.viewport=viewport;gl.state_.texgen_mask=texgen;gl.begun_=false;gl.vertices_.clear();}
    } saved{*this,{matrices_[0].back(),matrices_[1].back(),matrices_[2].back()},state_.enabled,state_.color,state_.texcoord,state_.viewport,state_.texgen_mask};
    const auto& range=state_.depth_range;const auto z=static_cast<float>(range[1]!=range[0]?(state_.raster_position[2]-(range[0]+range[1])/2)*2/(range[1]-range[0]):0);
    auto modelview=identity(),projection=identity();modelview[10]=0;modelview[14]=static_cast<float>(state_.raster_distance);projection[10]=0;projection[14]=z;
    matrices_[0].back()=modelview;matrices_[1].back()=projection;matrices_[2].back()=identity();
    for(const unsigned cap:{0xb50U,0xb44U,0x8037U,0xb24U,0xb42U,0x3000U,0x3001U,0x3002U,0x3003U,0x3004U,0x3005U,0x3006U,0x3007U})state_.enabled.erase(cap);
    state_.texgen_mask=0;state_.viewport={0,0,static_cast<int>(width_),static_cast<int>(height_)};state_.color=state_.raster_color;state_.texcoord=state_.raster_texcoord;
    const double surface_width=width_,surface_height=height_;
    const auto clip=[&](double wx,double wy){return Vec{static_cast<float>(wx*2/surface_width-1),static_cast<float>(wy*2/surface_height-1),0,1};};
    begun_=true;primitive_=4;vertices_.clear();emit(clip);begun_=false;if(!vertices_.empty())end_primitive();
}
void MetalGl::draw_pixel_image(const PixelImage& image){
    const auto zx=state_.pixel_zoom[0],zy=state_.pixel_zoom[1];if(zx==0 || zy==0 || !width_ || !height_ || !image.width || !image.height)return;
    if(image.width>16384 || image.height>16384){error(0x505);return;}
    const unsigned channels=image.kind==1?4U:1U;
    auto desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:image.kind==1?MTLPixelFormatRGBA32Float:MTLPixelFormatR32Float width:image.width height:image.height mipmapped:NO];
    desc.storageMode=MTLStorageModeShared;desc.usage=MTLTextureUsageShaderRead;
    auto lease=reserve_gpu_memory(static_cast<std::size_t>(image.width)*image.height*channels*4);auto texture=[device_ newTextureWithDescriptor:desc];if(!texture)throw std::bad_alloc{};
    [texture replaceRegion:MTLRegionMake2D(0,0,image.width,image.height) mipmapLevel:0 withBytes:image.values.data() bytesPerRow:static_cast<NSUInteger>(image.width)*channels*4];
    const auto xr=state_.raster_position[0],yr=state_.raster_position[1];
    pixel_draw_={image.kind,texture,{xr,yr,zx,zy,static_cast<float>(image.width),static_cast<float>(image.height),static_cast<float>(height_),0}};
    struct Reset {PixelDraw& draw;~Reset(){draw={};}} reset{pixel_draw_};
    window_space([&](const auto& clip){
        const double x0=xr,y0=yr,x1=xr+static_cast<double>(zx)*image.width,y1=yr+static_cast<double>(zy)*image.height;
        const auto a0=clip(x0,y0),a1=clip(x1,y0),a2=clip(x1,y1),a3=clip(x0,y1);vertex(a0);vertex(a1);vertex(a2);vertex(a0);vertex(a2);vertex(a3);
    });
    pending_leases_.push_back(std::move(lease));
}
void MetalGl::draw_pixels(std::span<const std::uint64_t> a,std::span<const std::uint8_t> data){
    const auto width=std::bit_cast<std::int32_t>(static_cast<unsigned>(a[0])),height=std::bit_cast<std::int32_t>(static_cast<unsigned>(a[1]));
    const auto format=static_cast<unsigned>(a[2]),type=static_cast<unsigned>(a[3]);
    if(width<0 || height<0){error(0x501);return;}if(const auto e=pixel_format_error(format,type)){error(e);return;}if(begun_){error(0x502);return;}
    if(!state_.raster_valid)return;
    // 選取模式產生命中，回饋模式輸出 GL_DRAW_PIXEL_TOKEN 與光柵頂點。
    if(state_.render_mode!=0x1c00){raster_feedback(0x0705);return;}
    if(!width || !height || data.empty())return;
    const auto image=unpack_image(static_cast<unsigned>(width),static_cast<unsigned>(height),format,type,data,a.size()>4 && a[4]==canonical_pixels);
    if(image)draw_pixel_image(*image);
}
void MetalGl::copy_pixels(std::span<const std::uint64_t> a){
    const auto n=[&](unsigned i){return std::bit_cast<std::int32_t>(static_cast<unsigned>(a[i]));};const auto type=static_cast<unsigned>(a[4]);
    if(n(2)<0 || n(3)<0){error(0x501);return;}if(type<0x1800 || type>0x1802){error(0x500);return;}if(begun_){error(0x502);return;}
    if(!state_.raster_valid)return;
    if(state_.render_mode!=0x1c00){raster_feedback(0x0706);return;}
    if(!n(2) || !n(3))return;
    const auto width=static_cast<unsigned>(n(2)),height=static_cast<unsigned>(n(3));
    // GL_COLOR 0x1800、GL_DEPTH 0x1801、GL_STENCIL 0x1802：先完整讀出來源區域，重疊的目的區域不影響讀到的值。
    PixelImage image{width,height,type==0x1800?1U:type==0x1801?2U:3U,{}};
    if(type==0x1800){image.values=read_color(n(0),n(1),width,height);
        if(color_transfer_active())for(std::size_t i=0;i<image.values.size();i+=4){std::array<float,4> rgba{image.values[i],image.values[i+1],image.values[i+2],image.values[i+3]};color_transfer(rgba);std::copy(rgba.begin(),rgba.end(),image.values.begin()+static_cast<std::ptrdiff_t>(i));}}
    else{image.values=read_depth_stencil(n(0),n(1),width,height,type==0x1802);for(auto& v:image.values)v=type==0x1801?depth_transfer(v):static_cast<float>(stencil_transfer(static_cast<std::int64_t>(v))&255);}
    draw_pixel_image(image);
}
// 讀回目前讀取緩衝（前／後）的色彩為 RGBA 浮點，GL 列 0 在最下方；超出繪製表面的部分為 0。
std::vector<float> MetalGl::read_color(std::int32_t x,std::int32_t y,unsigned width,unsigned height){
    std::vector<float> values(static_cast<std::size_t>(width)*height*4,0.F);
    const auto x0=std::max<std::int64_t>(x,0),y0=std::max<std::int64_t>(y,0),x1=std::min<std::int64_t>(std::int64_t{x}+width,width_),y1=std::min<std::int64_t>(std::int64_t{y}+height,height_);
    if(x0>=x1 || y0>=y1)return values;
    flush(true);const auto w=static_cast<std::size_t>(x1-x0),h=static_cast<std::size_t>(y1-y0);std::vector<std::uint8_t> bgra(w*h*4);
    [(state_.read_buffer==0x404?front_:color_) getBytes:bgra.data() bytesPerRow:w*4 fromRegion:MTLRegionMake2D(static_cast<NSUInteger>(x0),static_cast<NSUInteger>(height_-y1),w,h) mipmapLevel:0];
    for(std::size_t row=0;row<h;++row)for(std::size_t col=0;col<w;++col){
        const auto* src=bgra.data()+((h-1-row)*w+col)*4;auto* dst=values.data()+((static_cast<std::size_t>(y0-y)+row)*width+static_cast<std::size_t>(x0-x)+col)*4;
        dst[0]=src[2]/255.F;dst[1]=src[1]/255.F;dst[2]=src[0]/255.F;dst[3]=src[3]/255.F;
    }
    return values;
}
// 讀回深度（[0,1] 浮點）或模板值；深度模板合併格式經 blit 分別取出平面。
std::vector<float> MetalGl::read_depth_stencil(std::int32_t x,std::int32_t y,unsigned width,unsigned height,bool stencil){
    std::vector<float> values(static_cast<std::size_t>(width)*height,0.F);
    const auto x0=std::max<std::int64_t>(x,0),y0=std::max<std::int64_t>(y,0),x1=std::min<std::int64_t>(std::int64_t{x}+width,width_),y1=std::min<std::int64_t>(std::int64_t{y}+height,height_);
    if(x0>=x1 || y0>=y1 || !depth_)return values;
    const auto w=static_cast<std::size_t>(x1-x0),h=static_cast<std::size_t>(y1-y0);const std::size_t element=stencil?1:4,pitch=(w*element+255)&~std::size_t{255};
    auto lease=reserve_gpu_memory(pitch*h);auto buffer=[device_ newBufferWithLength:pitch*h options:MTLResourceStorageModeShared];if(!buffer)throw std::bad_alloc{};
    begin_commands(true,"copy_texture_buffer");auto blit=[commands_ blitCommandEncoder];MetalEncoderScope scope{blit};
    [blit copyFromTexture:depth_ sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(static_cast<NSUInteger>(x0),static_cast<NSUInteger>(height_-y1),0) sourceSize:MTLSizeMake(w,h,1)
        toBuffer:buffer destinationOffset:0 destinationBytesPerRow:pitch destinationBytesPerImage:pitch*h options:stencil?MTLBlitOptionStencilFromDepthStencil:MTLBlitOptionDepthFromDepthStencil];
    scope.end();flush(true);
    const auto* bytes=static_cast<const std::uint8_t*>(buffer.contents);
    for(std::size_t row=0;row<h;++row)for(std::size_t col=0;col<w;++col){
        const auto* src=bytes+(h-1-row)*pitch+col*element;auto& dst=values[(static_cast<std::size_t>(y0-y)+row)*width+static_cast<std::size_t>(x0-x)+col];
        if(stencil)dst=src[0];else std::memcpy(&dst,src,4);
    }
    return values;
}
// 依 format／type 與 GL_PACK_* 寫出像素（ReadPixels 與 GetTexImage 共用）。channels 為 4 時 values 為 RGBA，1 時為深度或索引；
// luminance_sum 為 true 時 LUMINANCE 取 R+G+B 並夾限（ReadPixels），否則取 R（GetTexImage）。留白位元組保留呼叫端預先讀入的內容；回傳寫入範圍的位元組數。
std::uint64_t MetalGl::pack_pixels(std::span<const float> values,unsigned channels,unsigned width,unsigned height,unsigned format,unsigned type,bool luminance_sum,std::span<std::uint8_t> output){
    const auto components=format_components(format);const auto packed=packed_layout(type);const bool index=format==0x1900 || format==0x1901;
    const auto store=pixel_store(true,width,false);const auto g=pixel_geometry(store,format,type);
    const auto start=store.skip_rows*g.stride+(g.bitmap?0:store.skip_pixels*g.group);
    const auto total=g.bitmap?(store.skip_rows+height-1)*g.stride+(store.skip_pixels+width+7)/8:start+(static_cast<std::size_t>(height)-1)*g.stride+static_cast<std::size_t>(width)*g.group;
    if(total>output.size())throw std::invalid_argument("pack pixels bounds");
    const auto put=[&](std::size_t at,std::size_t size,std::uint64_t raw){
        if(store.swap && size==2)raw=__builtin_bswap16(static_cast<std::uint16_t>(raw));else if(store.swap && size==4)raw=__builtin_bswap32(static_cast<std::uint32_t>(raw));
        std::memcpy(output.data()+at,&raw,size);};
    const auto encode=[&](float value)->std::uint64_t{
        if(index){const auto integer=static_cast<std::int64_t>(value);return type==0x1406?std::bit_cast<std::uint32_t>(static_cast<float>(integer)):static_cast<std::uint64_t>(integer);}
        const auto scaled=[&](double low,double high,double scale){return std::llround(std::clamp(static_cast<double>(value),low,high)*scale);};
        switch(type){
        case 0x1401:return static_cast<std::uint8_t>(scaled(0,1,255));case 0x1400:return static_cast<std::uint8_t>(static_cast<std::int8_t>(scaled(-1,1,127)));
        case 0x1403:return static_cast<std::uint16_t>(scaled(0,1,65535));case 0x1402:return static_cast<std::uint16_t>(static_cast<std::int16_t>(scaled(-1,1,32767)));
        case 0x1405:return static_cast<std::uint32_t>(scaled(0,1,4294967295.0));case 0x1404:return static_cast<std::uint32_t>(static_cast<std::int32_t>(scaled(-1,1,2147483647.0)));
        default:return std::bit_cast<std::uint32_t>(value);}};
    for(std::size_t y=0;y<height;++y)for(std::size_t x=0;x<width;++x){
        const auto* v=values.data()+(y*width+x)*channels;
        if(g.bitmap){const auto bit=store.skip_pixels+x;const auto at=(store.skip_rows+y)*g.stride+bit/8;const auto mask=static_cast<std::uint8_t>(store.lsb?1U<<(bit%8):0x80U>>(bit%8));
            if(static_cast<std::int64_t>(v[0])&1)output[at]|=mask;else output[at]&=static_cast<std::uint8_t>(~mask);continue;}
        std::array<float,4> c{v[0],0,0,0};
        if(channels==4)switch(format){
        case 0x1904:c[0]=v[1];break;case 0x1905:c[0]=v[2];break;case 0x1906:c[0]=v[3];break;
        case 0x1909:case 0x190a:c[0]=luminance_sum?std::clamp(v[0]+v[1]+v[2],0.F,1.F):v[0];c[1]=v[3];break;
        case 0x8227:c[1]=v[1];break;case 0x1907:case 0x1908:c={v[0],v[1],v[2],v[3]};break;case 0x80e0:case 0x80e1:c={v[2],v[1],v[0],v[3]};break;default:break;}
        const auto at=start+y*g.stride+x*g.group;
        if(packed){std::uint64_t word=0;unsigned shift=packed->rev?0U:packed->bytes*8;
            for(unsigned i=0;i<packed->count;++i){const auto bits=packed->bits[i];if(!packed->rev)shift-=bits;const auto max=(std::uint64_t{1}<<bits)-1;
                word|=static_cast<std::uint64_t>(std::llround(std::clamp(c[i],0.F,1.F)*static_cast<float>(max)))<<shift;if(packed->rev)shift+=bits;}
            put(at,packed->bytes,word);}
        else for(unsigned i=0;i<components;++i)put(at+i*g.element,g.element,encode(c[i]));
    }
    return total;
}
// glReadPixels（預設 framebuffer）：RGBA／RGB 的 UNSIGNED_BYTE 且未啟用像素傳輸時直接轉換；其餘格式先讀成浮點、套用像素傳輸後依 GL_PACK_* 寫出。
void MetalGl::read_pixels(std::span<const std::uint64_t> a,std::span<std::uint8_t> output){
    const auto u=[&](unsigned i){return static_cast<unsigned>(a[i]);};const auto n=[&](unsigned i){return std::bit_cast<std::int32_t>(u(i));};
    const auto format=u(4),type=u(5);
    if(n(2)<0 || n(3)<0){error(0x501);return;}if(const auto e=pixel_format_error(format,type)){error(e);return;}
    // 本後端為 RGBA 像素格式，沒有色彩索引可讀。
    if(format==0x1900){error(0x502);return;}
    if(!u(2) || !u(3))return;
    if((format==0x1908 || format==0x1907) && type==0x1401 && !color_transfer_active()){
        flush(true);const unsigned components=format==0x1908?4:3;const auto [stride,offset]=pixel_layout(u(2),components,true);
        if(offset+(u(3)-1)*stride+std::size_t{u(2)}*components>output.size())throw std::invalid_argument("read pixels bounds");
        std::vector<std::uint8_t> row(static_cast<std::size_t>(width_)*4);
        for(unsigned y=0;y<u(3);++y){const auto src_y=static_cast<std::int64_t>(n(1))+y;if(src_y<0 || src_y>=height_)continue;
            [(state_.read_buffer==0x404?front_:color_) getBytes:row.data() bytesPerRow:width_*4 fromRegion:MTLRegionMake2D(0,static_cast<NSUInteger>(height_-1-src_y),width_,1) mipmapLevel:0];
            for(unsigned x=0;x<u(2);++x){const auto src_x=static_cast<std::int64_t>(n(0))+x;if(src_x<0 || src_x>=width_)continue;const auto at=offset+y*stride+x*components,src=static_cast<std::size_t>(src_x)*4;
                output[at]=row[src+2];output[at+1]=row[src+1];output[at+2]=row[src];if(components==4)output[at+3]=row[src+3];}}
        return;
    }
    std::vector<float> values;unsigned channels=1;
    if(format==0x1902){values=read_depth_stencil(n(0),n(1),u(2),u(3),false);for(auto& v:values)v=depth_transfer(v);}
    else if(format==0x1901){values=read_depth_stencil(n(0),n(1),u(2),u(3),true);for(auto& v:values)v=static_cast<float>(stencil_transfer(static_cast<std::int64_t>(v)));}
    else{channels=4;values=read_color(n(0),n(1),u(2),u(3));
        if(color_transfer_active())for(std::size_t i=0;i<values.size();i+=4){std::array<float,4> rgba{values[i],values[i+1],values[i+2],values[i+3]};color_transfer(rgba);std::copy(rgba.begin(),rgba.end(),values.begin()+static_cast<std::ptrdiff_t>(i));}}
    pack_pixels(values,channels,u(2),u(3),format,type,true,output);
}
void MetalGl::pixel_depth_stencil_state(MTLDepthStencilDescriptor* depth)const{
    // 深度不測試也不寫入。模板影像直接以片段輸出的模板值取代，只受寫入遮罩限制；GL_RETURN（kind 4）不做模板測試。
    depth.depthCompareFunction=MTLCompareFunctionAlways;depth.depthWriteEnabled=NO;
    if(pixel_draw_.kind!=3){depth.frontFaceStencil=nil;depth.backFaceStencil=nil;return;}
    auto stencil=make_metal_stencil(MTLCompareFunctionAlways,MTLStencilOperationReplace,MTLStencilOperationReplace,MTLStencilOperationReplace,255,state_.stencil.write_mask&255U);
    depth.frontFaceStencil=stencil;depth.backFaceStencil=stencil;
}

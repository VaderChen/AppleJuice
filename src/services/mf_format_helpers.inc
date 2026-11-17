// 媒體型別與格式結構互轉（由 windows_media_foundation.cpp 在匯出鏈之前引入）：
// VIDEOINFOHEADER／VIDEOINFOHEADER2、MPEG1VIDEOINFO／MPEG2VIDEOINFO、MFVIDEOFORMAT、AM_MEDIA_TYPE 與 WAVEFORMATEX。
// MFInitMediaTypeFrom*、MFInitAMMediaTypeFromMFMediaType、IMFMediaType::GetRepresentation 與 IMFVideoMediaType::GetVideoFormat 共用同一組對應規則：
// - 子型別：BITMAPINFOHEADER 的 BI_RGB／BI_BITFIELDS 依位元深度對應 D3DFORMAT 子型別，其他壓縮碼即 FOURCC；DirectShow 的 RGB 子型別與 MF 子型別互轉。
// - 步幅：RGB 且 biHeight 為正（由下而上）時 MF_MT_DEFAULT_STRIDE 為負；YUV 一律為正。未壓縮格式另設樣本大小、固定大小與全部獨立。
// - BITMAPINFOHEADER 之後的資料：8 位元以下 RGB 的調色盤存成 MF_MT_PALETTE，其餘存成 MF_MT_USER_DATA；BI_BITFIELDS 的三個遮罩不另存。
// - VIDEOINFOHEADER2：交錯旗標對應 MF_MT_INTERLACE_MODE，畫面長寬比換算成像素長寬比，AMCONTROL_COLORINFO_PRESENT 的色彩資訊對應各色彩屬性。
// - MFVIDEOFORMAT（176 位元組）之後依序是調色盤（PaletteEntries 筆）與 MF_MT_USER_DATA。
const Guid fmt_video_info=mf::guid(0x05589f80,0xc356,0x11ce,{0xbf,0x01,0x00,0xaa,0x00,0x55,0x59,0x5a}),
    fmt_wave=mf::guid(0x05589f81,0xc356,0x11ce,{0xbf,0x01,0x00,0xaa,0x00,0x55,0x59,0x5a}),
    fmt_mpeg1=mf::guid(0x05589f82,0xc356,0x11ce,{0xbf,0x01,0x00,0xaa,0x00,0x55,0x59,0x5a}),
    fmt_video_info2=mf::guid(0xf72a76a0,0xeb0a,0x11d0,{0xac,0xe4,0x00,0x00,0xc0,0xcc,0x16,0xba}),
    fmt_mpeg2=mf::guid(0xe06d80e3,0xdb46,0x11cf,{0xb4,0xd1,0x00,0x80,0x5f,0x6c,0xbb,0xea}),
    fmt_mf_video=mf::guid(0xaed4ab2d,0x7326,0x43cb,{0x94,0x64,0xc8,0x79,0xca,0xb9,0xc4,0x3d}),
    fmt_am_representation=mf::guid(0xe2e42ad2,0x132c,0x491e,{0xa2,0x68,0x3c,0x7c,0x2d,0xca,0x18,0x1f}),
    fmt_mpeg2_subtype=mf::guid(0xe06d8026,0xdb46,0x11cf,{0xb4,0xd1,0x00,0x80,0x5f,0x6c,0xbb,0xea}),
    fmt_dshow_argb32=mf::guid(0x773c9ac0,0x3274,0x11d0,{0xb7,0x24,0x00,0xaa,0x00,0x6c,0x1a,0x01});
const Guid key_chroma=mf::guid(0x65df2370,0xc773,0x4c33,{0xaa,0x64,0x84,0x3e,0x06,0x8e,0xfb,0x0c}),
    key_transfer=mf::guid(0x5fb0fce9,0xbe5c,0x4935,{0xa8,0x11,0xec,0x83,0x8f,0x8e,0xed,0x93}),
    key_primaries=mf::guid(0xdbfbe4d7,0x0740,0x4ee0,{0x81,0x92,0x85,0x0a,0xb0,0xe2,0x19,0x35}),
    key_matrix=mf::guid(0x3e23d450,0x2c75,0x4d25,{0xa0,0x0e,0xb9,0x16,0x70,0xd1,0x23,0x27}),
    key_lighting=mf::guid(0x53a0529c,0x890b,0x4216,{0x8b,0xf9,0x59,0x93,0x67,0xad,0x6d,0x20}),
    key_nominal=mf::guid(0xc21b8ee5,0xb956,0x4071,{0x8d,0xaf,0x32,0x5e,0xdf,0x5c,0xab,0x11}),
    key_geometric=mf::guid(0x66758743,0x7e5f,0x400d,{0x98,0x0a,0xaa,0x85,0x96,0xc8,0x56,0x96}),
    key_minimum=mf::guid(0xd7388766,0x18fe,0x48c6,{0xa1,0x77,0xee,0x89,0x48,0x67,0xc8,0xc4}),
    key_pan_scan=mf::guid(0x79614dde,0x9187,0x48fb,{0xb8,0xc7,0x4d,0x52,0x68,0x9d,0xe6,0x49}),
    key_pan_scan_enabled=mf::guid(0x4b7f6bc3,0x8b13,0x40b2,{0xa9,0x93,0xab,0xf6,0x30,0xb8,0x20,0x4e}),
    key_pad=mf::guid(0x4d0e73e5,0x80ea,0x4354,{0xa9,0xd0,0x11,0x76,0xce,0xb0,0x28,0xea}),
    key_content_hint=mf::guid(0x68aca3cc,0x22d0,0x44e6,{0x85,0xf8,0x28,0x16,0x71,0x97,0xfa,0x38}),
    key_drm=mf::guid(0x8772f323,0x355a,0x4cc7,{0xbb,0x78,0x6d,0x61,0xa0,0x48,0xae,0x82}),
    key_error_rate=mf::guid(0x799cabd6,0x3508,0x4db4,{0xa3,0xc7,0x56,0x9c,0xd5,0x33,0xde,0xb1}),
    key_keyframe=mf::guid(0xc16eb52b,0x73a1,0x476f,{0x8d,0x62,0x83,0x9d,0x6a,0x02,0x06,0x52}),
    key_palette=mf::guid(0x6d283f42,0x9846,0x4410,{0xaf,0xd9,0x65,0x4d,0x50,0x3b,0x1a,0x54}),
    key_start_time_code=mf::guid(0x91f67885,0x4333,0x4280,{0x97,0xcd,0xbd,0x5a,0x6c,0x03,0xa0,0x6e}),
    key_mpeg2_flags=mf::guid(0x31e3991d,0xf701,0x4b2f,{0xb4,0x26,0x8a,0xe3,0xbd,0xa9,0xe0,0x4b}),
    key_am_format=mf::guid(0x73d1072d,0x1870,0x4174,{0xa0,0x63,0x29,0xff,0x4f,0xf6,0xc1,0x1e});
constexpr std::uint32_t unsupported_representation=0xc00d36b7U,invalid_media_type=0xc00d36b4U;
const auto fmt_le=[&](std::span<const std::uint8_t> bytes,std::size_t at,unsigned width)->std::uint64_t{
    require(at<=bytes.size() && width<=bytes.size()-at);std::uint64_t value=0;for(unsigned n=0;n<width;++n)value|=static_cast<std::uint64_t>(bytes[at+n])<<(8*n);return value;};
const auto fmt_put=[](std::vector<std::uint8_t>& bytes,std::size_t at,std::uint64_t value,unsigned width){for(unsigned n=0;n<width;++n)bytes[at+n]=static_cast<std::uint8_t>(value>>(8*n));};
const auto media_guid=[](std::uint32_t code){return mf::guid(code,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71});};
// 子型別的 D3DFORMAT／FOURCC 碼；DirectShow RGB 子型別也對應到 D3DFORMAT（與 MFGetStrideForBitmapInfoHeader 相同規則），其他 GUID 為 0。
const auto media_code=[](const Guid& id){return static_cast<std::uint32_t>(mf_image_guid_format(id.data()));};
const auto from_dshow=[&](const Guid& id){if(id==fmt_dshow_argb32)return media_guid(21);const auto code=media_code(id);return code && id!=media_guid(code)?media_guid(code):id;};
const auto to_dshow=[&](const Guid& id){
    const auto code=media_code(id);if(id!=media_guid(code))return id;
    if(code==21)return fmt_dshow_argb32;
    static constexpr std::array<std::pair<std::uint32_t,std::uint32_t>,7> rgb{{{118,0xe436eb78U},{MF_IMAGE_FOURCC('4','P','x','x'),0xe436eb79U},{41,0xe436eb7aU},{23,0xe436eb7bU},{24,0xe436eb7cU},{20,0xe436eb7dU},{22,0xe436eb7eU}}};
    for(const auto& [format,dshow]:rgb)if(format==code)return mf::guid(dshow,0x524f,0x11ce,{0x9f,0x53,0x00,0x20,0xaf,0x0b,0xa7,0x70});
    return id;};
const auto attribute=[](const Object& item,const Guid& key)->const Value*{const auto found=item.attributes.find(key);return found==item.attributes.end()?nullptr:&found->second;};
const auto attribute_scalar=[&](const Object& item,const Guid& key,std::uint64_t fallback=0){const auto* value=attribute(item,key);return value && (value->type==19 || value->type==21)?value->scalar:fallback;};
const auto attribute_blob=[&](const Object& item,const Guid& key){const auto* value=attribute(item,key);return value && value->type==0x1011?value->blob:std::vector<std::uint8_t>{};};
const auto put_nonzero=[&](Object& item,const Guid& key,std::uint64_t value){if(value)set(item,key,value32(static_cast<std::uint32_t>(value)));};
// MFAverageTimePerFrameToFrameRate：常見幀率（NTSC 1000/1001 系列與整數幀率）誤差在 1 個 100 ns 內時回傳標準分數，否則為 10000000/平均時間（約分）。
const auto frame_rate_of=[&](std::uint64_t average)->std::pair<std::uint32_t,std::uint32_t>{
    static constexpr std::array<std::array<std::uint32_t,2>,13> rates{{{60000,1001},{30000,1001},{24000,1001},{120,1},{100,1},{60,1},{50,1},{48,1},{30,1},{25,1},{24,1},{15,1},{10,1}}};
    for(const auto& [n,dd]:rates){const auto exact=(UINT64_C(10000000)*dd+n/2)/n;if(average+1>=exact && average<=exact+1)return {n,dd};}
    require(average!=0);std::uint64_t n=10000000,dd=average;const auto common=std::gcd(n,dd);n/=common;dd/=common;
    while(dd>UINT32_MAX){n=(n+1)/2;dd=(dd+1)/2;}return {static_cast<std::uint32_t>(n),static_cast<std::uint32_t>(dd)};};
const auto average_time_of=[&](const Object& item)->std::uint64_t{
    const auto rate=attribute_scalar(item,mf::MF_MT_FRAME_RATE);const auto n=rate>>32,dd=rate&UINT32_MAX;return n && dd?(UINT64_C(10000000)*dd+n/2)/n:0;};
// MFVideoArea：OffsetX／OffsetY（WORD 小數＋short 整數）與 SIZE。
const auto area_blob=[](std::int32_t x,std::int32_t y,std::int32_t cx,std::int32_t cy){
    std::vector<std::uint8_t> bytes(16);const auto store=[&](std::size_t at,std::uint32_t value){for(unsigned n=0;n<4;++n)bytes[at+n]=static_cast<std::uint8_t>(value>>(8*n));};
    store(0,static_cast<std::uint32_t>(static_cast<std::uint16_t>(x))<<16);store(4,static_cast<std::uint32_t>(static_cast<std::uint16_t>(y))<<16);store(8,static_cast<std::uint32_t>(cx));store(12,static_cast<std::uint32_t>(cy));return bytes;};
const auto area_of=[&](std::span<const std::uint8_t> bytes)->std::array<std::int32_t,4>{
    return {static_cast<std::int16_t>(fmt_le(bytes,2,2)),static_cast<std::int16_t>(fmt_le(bytes,6,2)),static_cast<std::int32_t>(fmt_le(bytes,8,4)),static_cast<std::int32_t>(fmt_le(bytes,12,4))};};
// 未壓縮格式的步幅（依由下而上與否帶正負號）、樣本大小與固定大小／全部獨立。
const auto set_layout=[&](Object& item,std::uint32_t code,std::uint32_t width,std::uint32_t height,bool bottom_up,std::uint32_t image){
    MfImageLong stride=0;
    if(code && width && !mf_image_stride(code,width,&stride)){
        const auto layout=mf_image_layout(code,width);std::int64_t pitch=stride<0?-static_cast<std::int64_t>(stride):stride;if(layout.rgb && bottom_up)pitch=-pitch;
        set(item,mf::MF_MT_DEFAULT_STRIDE,value32(static_cast<std::uint32_t>(static_cast<std::int32_t>(pitch))));
        MfImageWord size=0;if(!image && height && !mf_image_size(code,width,height,1,&size))image=size;
        set(item,mf::MF_MT_FIXED_SIZE_SAMPLES,value32(1));set(item,mf::MF_MT_ALL_SAMPLES_INDEPENDENT,value32(1));
    }
    put_nonzero(item,mf::MF_MT_SAMPLE_SIZE,image);};
// BITMAPINFOHEADER 為主的視訊格式（VIDEOINFOHEADER／2、MPEG1／2 共用）。
struct VideoHeader {std::span<const std::uint8_t> bitmap;std::array<std::int32_t,4> source{};std::uint32_t bitrate{},error_rate{};std::uint64_t average{};
    std::optional<std::uint32_t> interlace;std::uint32_t aspect_x{},aspect_y{},control{};std::optional<Guid> subtype;bool trailing{true};};
const auto init_video=[&](Object& item,const VideoHeader& header){
    const auto& bitmap=header.bitmap;const auto size=static_cast<std::uint32_t>(fmt_le(bitmap,0,4));require(size>=40 && size<=bitmap.size());
    const auto width=static_cast<std::int32_t>(fmt_le(bitmap,4,4)),height=static_cast<std::int32_t>(fmt_le(bitmap,8,4));
    const auto depth=static_cast<std::uint32_t>(fmt_le(bitmap,14,2)),compression=static_cast<std::uint32_t>(fmt_le(bitmap,16,4));
    const auto image=static_cast<std::uint32_t>(fmt_le(bitmap,20,4)),used=static_cast<std::uint32_t>(fmt_le(bitmap,32,4));
    require(width>0 && height!=0 && height!=INT32_MIN,invalid_media_type);const auto rows=static_cast<std::uint32_t>(height<0?-static_cast<std::int64_t>(height):height);
    std::size_t extra=compression==3?size+12:size;Guid subtype{};
    if(header.subtype && *header.subtype!=Guid{})subtype=from_dshow(*header.subtype);
    else if(compression==0){
        switch(depth){case 32:subtype=media_guid(22);break;case 24:subtype=media_guid(20);break;case 16:subtype=media_guid(24);break;case 8:subtype=media_guid(41);break;
            case 4:subtype=media_guid(MF_IMAGE_FOURCC('4','P','x','x'));break;case 1:subtype=media_guid(118);break;default:throw Failure{invalid_media_type};}
    }else if(compression==3){
        require(depth==16 || depth==32,invalid_media_type);const auto red=fmt_le(bitmap,size,4),green=fmt_le(bitmap,size+4,4),alpha=depth==32 && bitmap.size()>=size+16?fmt_le(bitmap,size+12,4):0;
        subtype=media_guid(depth==16?(red==0xf800 && green==0x7e0?23U:24U):alpha?21U:22U);
    }else subtype=media_guid(compression);
    const auto code=media_code(subtype);
    set(item,mf::MF_MT_MAJOR_TYPE,valueguid(mf::MFMediaType_Video));set(item,mf::MF_MT_SUBTYPE,valueguid(subtype));
    set(item,mf::MF_MT_FRAME_SIZE,value64((static_cast<std::uint64_t>(width)<<32)|rows));
    set_layout(item,code,static_cast<std::uint32_t>(width),rows,height>0,image);
    if(header.average){const auto [n,dd]=frame_rate_of(header.average);set(item,mf::MF_MT_FRAME_RATE,value64((static_cast<std::uint64_t>(n)<<32)|dd));}
    put_nonzero(item,mf::MF_MT_AVG_BITRATE,header.bitrate);put_nonzero(item,key_error_rate,header.error_rate);
    std::uint64_t par_n=1,par_d=1;
    if(header.aspect_x && header.aspect_y){par_n=static_cast<std::uint64_t>(header.aspect_x)*rows;par_d=static_cast<std::uint64_t>(header.aspect_y)*static_cast<std::uint32_t>(width);
        const auto common=std::gcd(par_n,par_d);par_n/=common;par_d/=common;while(par_n>UINT32_MAX || par_d>UINT32_MAX){par_n=(par_n+1)/2;par_d=(par_d+1)/2;}}
    set(item,mf::MF_MT_PIXEL_ASPECT_RATIO,value64((par_n<<32)|par_d));
    // AMINTERLACE_*：IsInterlaced 1、1FieldPerSample 2、Field1First 4、DisplayModeBobOrWeave 0x80。
    std::uint32_t mode=2;
    if(header.interlace && (*header.interlace&1)){
        const auto flags=*header.interlace;mode=(flags&2)?((flags&4)?5U:6U):(flags&0xc0)==0x80?7U:((flags&4)?3U:4U);
    }
    set(item,mf::MF_MT_INTERLACE_MODE,value32(mode));
    const auto& [left,top,right,bottom]=header.source;
    if(right>left && bottom>top && !(left==0 && top==0 && right==width && static_cast<std::uint32_t>(bottom)==rows))
        set(item,key_minimum,[&]{Value value;value.type=0x1011;value.blob=area_blob(left,top,right-left,bottom-top);return value;}());
    // AMCONTROL_USED 1、PAD_TO_4x3 2、PAD_TO_16x9 4、COLORINFO_PRESENT 0x80（高 24 位元為 DXVA_ExtendedFormat 的第 8～31 位元）。
    if(header.control&1){
        put_nonzero(item,key_pad,(header.control&2)?1U:(header.control&4)?2U:0U);
        if(header.control&0x80){const auto c=header.control;
            put_nonzero(item,key_chroma,(c>>8)&0xf);put_nonzero(item,key_nominal,(c>>12)&7);put_nonzero(item,key_matrix,(c>>15)&7);
            put_nonzero(item,key_lighting,(c>>18)&0xf);put_nonzero(item,key_primaries,(c>>22)&0x1f);put_nonzero(item,key_transfer,(c>>27)&0x1f);}
    }
    if(header.trailing && extra<bitmap.size()){
        std::vector<std::uint8_t> rest(bitmap.begin()+static_cast<std::ptrdiff_t>(extra),bitmap.end());
        if(compression==0 && depth<=8){
            const std::size_t entries=used?used:(std::size_t{1}<<depth);const auto bytes=std::min(rest.size(),entries*4)/4*4;
            if(bytes){Value value;value.type=0x1011;value.blob.assign(rest.begin(),rest.begin()+static_cast<std::ptrdiff_t>(bytes));set(item,key_palette,std::move(value));}
            rest.erase(rest.begin(),rest.begin()+static_cast<std::ptrdiff_t>(bytes));
        }
        if(!rest.empty()){Value value;value.type=0x1011;value.blob=std::move(rest);set(item,mf::MF_MT_USER_DATA,std::move(value));}
    }};
// VIDEOINFOHEADER（version 1，88 位元組）與 VIDEOINFOHEADER2（version 2，112 位元組）。
const auto video_header_of=[&](std::span<const std::uint8_t> bytes,unsigned version){
    const std::size_t bitmap=version==2?72:48;require(bytes.size()>=bitmap+40);VideoHeader header;
    for(unsigned n=0;n<4;++n)header.source[n]=static_cast<std::int32_t>(fmt_le(bytes,n*4,4));
    header.bitrate=static_cast<std::uint32_t>(fmt_le(bytes,32,4));header.error_rate=static_cast<std::uint32_t>(fmt_le(bytes,36,4));header.average=fmt_le(bytes,40,8);
    if(version==2){header.interlace=static_cast<std::uint32_t>(fmt_le(bytes,48,4));header.aspect_x=static_cast<std::uint32_t>(fmt_le(bytes,56,4));
        header.aspect_y=static_cast<std::uint32_t>(fmt_le(bytes,60,4));header.control=static_cast<std::uint32_t>(fmt_le(bytes,64,4));}
    header.bitmap=bytes.subspan(bitmap);return header;};
const auto init_mpeg=[&](Object& item,std::span<const std::uint8_t> bytes,unsigned version,std::optional<Guid> subtype){
    // MPEG1VIDEOINFO：VIDEOINFOHEADER＋dwStartTimeCode＋cbSequenceHeader＋bSequenceHeader（96）；
    // MPEG2VIDEOINFO：VIDEOINFOHEADER2＋dwStartTimeCode、cbSequenceHeader、dwProfile、dwLevel、dwFlags＋dwSequenceHeader（132）。
    const std::size_t base=version==2?112:88,sequence=version==2?132:96;require(bytes.size()>=sequence);
    auto header=video_header_of(bytes.first(base),version);header.bitmap=header.bitmap.first(40);header.trailing=false;
    const auto compression=static_cast<std::uint32_t>(fmt_le(header.bitmap,16,4));
    header.subtype=subtype?subtype:std::optional<Guid>{version==1?media_guid(MF_IMAGE_FOURCC('M','P','G','1')):compression>3?media_guid(compression):fmt_mpeg2_subtype};
    const auto length=static_cast<std::uint32_t>(fmt_le(bytes,base+4,4));require(length<=bytes.size()-sequence);
    init_video(item,header);put_nonzero(item,key_start_time_code,fmt_le(bytes,base,4));
    if(version==2){put_nonzero(item,mf::MF_MT_MPEG2_PROFILE,fmt_le(bytes,120,4));put_nonzero(item,mf::MF_MT_MPEG2_LEVEL,fmt_le(bytes,124,4));put_nonzero(item,key_mpeg2_flags,fmt_le(bytes,128,4));}
    if(length){Value value;value.type=0x1011;value.blob.assign(bytes.begin()+static_cast<std::ptrdiff_t>(sequence),bytes.begin()+static_cast<std::ptrdiff_t>(sequence+length));set(item,mf::MF_MT_MPEG_SEQUENCE_HEADER,std::move(value));}};
// MFVideoFlags：PAD_TO 0x3、SrcContentHint 0x1c、AnalogProtected 0x20、DigitallyProtected 0x40、PanScanEnabled 0x20000、BottomUpLinearRep 0x80000。
const auto apply_video_flags=[&](Object& item,std::uint64_t flags){
    put_nonzero(item,key_pad,flags&3);put_nonzero(item,key_content_hint,(flags>>2)&7);put_nonzero(item,key_drm,((flags&0x20)?1U:0U)|((flags&0x40)?2U:0U));
    if(flags&0x20000)set(item,key_pan_scan_enabled,value32(1));
    if(flags&0x80000)if(const auto* stride=attribute(item,mf::MF_MT_DEFAULT_STRIDE);stride && static_cast<std::int32_t>(stride->scalar)>0)
        set(item,mf::MF_MT_DEFAULT_STRIDE,value32(static_cast<std::uint32_t>(-static_cast<std::int32_t>(stride->scalar))));};
const auto video_flags_of=[&](const Object& item){
    std::uint64_t flags=attribute_scalar(item,key_pad)&3;flags|=(attribute_scalar(item,key_content_hint)&7)<<2;const auto drm=attribute_scalar(item,key_drm);
    if(drm&1)flags|=0x20;if(drm&2)flags|=0x40;if(attribute_scalar(item,key_pan_scan_enabled))flags|=0x20000;
    if(const auto* stride=attribute(item,mf::MF_MT_DEFAULT_STRIDE);stride && static_cast<std::int32_t>(stride->scalar)<0)flags|=0x80000;return flags;};
const auto init_mf_video=[&](Object& item,std::span<const std::uint8_t> bytes){
    require(bytes.size()>=176);const auto size=static_cast<std::uint32_t>(fmt_le(bytes,0,4));require(size>=176 && size<=bytes.size());bytes=bytes.first(size);
    Guid subtype{};std::copy_n(bytes.begin()+120,16,subtype.begin());
    const auto width=static_cast<std::uint32_t>(fmt_le(bytes,8,4)),height=static_cast<std::uint32_t>(fmt_le(bytes,12,4));const auto flags=fmt_le(bytes,112,8);
    set(item,mf::MF_MT_MAJOR_TYPE,valueguid(mf::MFMediaType_Video));set(item,mf::MF_MT_SUBTYPE,valueguid(subtype));
    if(width && height)set(item,mf::MF_MT_FRAME_SIZE,value64((static_cast<std::uint64_t>(width)<<32)|height));
    if(fmt_le(bytes,16,4) && fmt_le(bytes,20,4))set(item,mf::MF_MT_PIXEL_ASPECT_RATIO,value64(fmt_le(bytes,16,4)<<32|fmt_le(bytes,20,4)));
    put_nonzero(item,key_chroma,fmt_le(bytes,24,4));put_nonzero(item,mf::MF_MT_INTERLACE_MODE,fmt_le(bytes,28,4));put_nonzero(item,key_transfer,fmt_le(bytes,32,4));
    put_nonzero(item,key_primaries,fmt_le(bytes,36,4));put_nonzero(item,key_matrix,fmt_le(bytes,40,4));put_nonzero(item,key_lighting,fmt_le(bytes,44,4));
    if(fmt_le(bytes,48,4) && fmt_le(bytes,52,4))set(item,mf::MF_MT_FRAME_RATE,value64(fmt_le(bytes,48,4)<<32|fmt_le(bytes,52,4)));
    put_nonzero(item,key_nominal,fmt_le(bytes,56,4));
    for(const auto& [at,key]:std::array<std::pair<std::size_t,Guid>,3>{{{60,key_geometric},{76,key_minimum},{92,key_pan_scan}}})
        if(fmt_le(bytes,at+8,4) && fmt_le(bytes,at+12,4)){Value value;value.type=0x1011;value.blob.assign(bytes.begin()+static_cast<std::ptrdiff_t>(at),bytes.begin()+static_cast<std::ptrdiff_t>(at+16));set(item,key,std::move(value));}
    set_layout(item,media_code(subtype),width,height,(flags&0x80000)!=0,0);apply_video_flags(item,flags&~UINT64_C(0x80000));
    put_nonzero(item,mf::MF_MT_AVG_BITRATE,fmt_le(bytes,136,8));put_nonzero(item,key_error_rate,fmt_le(bytes,144,8));put_nonzero(item,key_keyframe,fmt_le(bytes,152,4));
    const auto entries=static_cast<std::uint32_t>(fmt_le(bytes,164,4));require(entries<=256 && 168+static_cast<std::size_t>(entries)*4<=bytes.size());
    if(entries){Value value;value.type=0x1011;value.blob.assign(bytes.begin()+168,bytes.begin()+168+static_cast<std::ptrdiff_t>(entries)*4);set(item,key_palette,std::move(value));}
    const auto user=std::max<std::size_t>(176,168+static_cast<std::size_t>(entries)*4);
    if(user<bytes.size()){Value value;value.type=0x1011;value.blob.assign(bytes.begin()+static_cast<std::ptrdiff_t>(user),bytes.end());set(item,mf::MF_MT_USER_DATA,std::move(value));}};
const auto require_video=[&](const Object& item){const auto* major=attribute(item,mf::MF_MT_MAJOR_TYPE);require(major && major->type==72,missing);require(major->id==mf::MFMediaType_Video,invalid_media_type);};
const auto subtype_of=[&](const Object& item){const auto* subtype=attribute(item,mf::MF_MT_SUBTYPE);return subtype && subtype->type==72?subtype->id:Guid{};};
const auto mf_video_bytes=[&](const Object& item){
    require_video(item);const auto subtype=subtype_of(item);const auto palette=attribute_blob(item,key_palette),user=attribute_blob(item,mf::MF_MT_USER_DATA);
    const auto entries=palette.size()/4;require(entries<=256);const auto user_at=std::max<std::size_t>(176,168+entries*4);
    std::vector<std::uint8_t> bytes(user_at+user.size());fmt_put(bytes,0,bytes.size(),4);
    const auto frame=attribute_scalar(item,mf::MF_MT_FRAME_SIZE),par=attribute_scalar(item,mf::MF_MT_PIXEL_ASPECT_RATIO),rate=attribute_scalar(item,mf::MF_MT_FRAME_RATE);
    fmt_put(bytes,8,frame>>32,4);fmt_put(bytes,12,frame,4);fmt_put(bytes,16,par>>32,4);fmt_put(bytes,20,par,4);
    fmt_put(bytes,24,attribute_scalar(item,key_chroma),4);fmt_put(bytes,28,attribute_scalar(item,mf::MF_MT_INTERLACE_MODE),4);fmt_put(bytes,32,attribute_scalar(item,key_transfer),4);
    fmt_put(bytes,36,attribute_scalar(item,key_primaries),4);fmt_put(bytes,40,attribute_scalar(item,key_matrix),4);fmt_put(bytes,44,attribute_scalar(item,key_lighting),4);
    fmt_put(bytes,48,rate>>32,4);fmt_put(bytes,52,rate,4);fmt_put(bytes,56,attribute_scalar(item,key_nominal),4);
    for(const auto& [at,key]:std::array<std::pair<std::size_t,Guid>,3>{{{60,key_geometric},{76,key_minimum},{92,key_pan_scan}}}){
        const auto area=attribute_blob(item,key);if(area.size()==16)std::copy(area.begin(),area.end(),bytes.begin()+static_cast<std::ptrdiff_t>(at));}
    fmt_put(bytes,112,video_flags_of(item),8);std::copy(subtype.begin(),subtype.end(),bytes.begin()+120);
    fmt_put(bytes,136,attribute_scalar(item,mf::MF_MT_AVG_BITRATE),8);fmt_put(bytes,144,attribute_scalar(item,key_error_rate),8);fmt_put(bytes,152,attribute_scalar(item,key_keyframe),4);
    fmt_put(bytes,160,media_code(subtype),4);fmt_put(bytes,164,entries,4);std::copy(palette.begin(),palette.begin()+static_cast<std::ptrdiff_t>(entries*4),bytes.begin()+168);
    std::copy(user.begin(),user.end(),bytes.begin()+static_cast<std::ptrdiff_t>(user_at));return bytes;};
// 由媒體型別產生 VIDEOINFOHEADER／VIDEOINFOHEADER2；trailing 時附上 BI_BITFIELDS 遮罩、調色盤與 MF_MT_USER_DATA。stride 非零時取代預設步幅決定由下而上與否。
const auto video_info_bytes=[&](const Object& item,unsigned version,bool trailing,std::int32_t stride_override){
    require_video(item);const auto subtype=subtype_of(item);const auto code=media_code(subtype);
    const auto frame=attribute_scalar(item,mf::MF_MT_FRAME_SIZE);const auto width=static_cast<std::uint32_t>(frame>>32),height=static_cast<std::uint32_t>(frame);
    const auto layout=mf_image_layout(code,width);const bool known=code && (layout.bits || layout.stride);
    const auto stride=stride_override?stride_override:static_cast<std::int32_t>(attribute_scalar(item,mf::MF_MT_DEFAULT_STRIDE,static_cast<std::uint32_t>(-1)));
    const bool bottom_up=layout.rgb && stride<0;
    const auto palette=attribute_blob(item,key_palette),user=attribute_blob(item,mf::MF_MT_USER_DATA);
    const bool fields=layout.rgb && code==23;const std::size_t bitmap=version==2?72:48;
    std::vector<std::uint8_t> bytes(bitmap+40);
    if(const auto area=attribute_blob(item,key_minimum);area.size()==16){const auto [x,y,cx,cy]=area_of(area);
        for(const std::size_t at:{std::size_t{0},std::size_t{16}}){fmt_put(bytes,at,static_cast<std::uint32_t>(x),4);fmt_put(bytes,at+4,static_cast<std::uint32_t>(y),4);fmt_put(bytes,at+8,static_cast<std::uint32_t>(x+cx),4);fmt_put(bytes,at+12,static_cast<std::uint32_t>(y+cy),4);}}
    fmt_put(bytes,32,attribute_scalar(item,mf::MF_MT_AVG_BITRATE),4);fmt_put(bytes,36,attribute_scalar(item,key_error_rate),4);fmt_put(bytes,40,average_time_of(item),8);
    if(version==2){
        const auto mode=attribute_scalar(item,mf::MF_MT_INTERLACE_MODE,2);
        fmt_put(bytes,48,mode==3?0x5U:mode==4?0x1U:mode==5?0x7U:mode==6?0x3U:mode==7?0x85U:0U,4);
        const auto par=attribute_scalar(item,mf::MF_MT_PIXEL_ASPECT_RATIO,UINT64_C(0x100000001));std::uint64_t x=static_cast<std::uint64_t>(width)*(par>>32),y=static_cast<std::uint64_t>(height)*(par&UINT32_MAX);
        if(x && y){const auto common=std::gcd(x,y);x/=common;y/=common;while(x>UINT32_MAX || y>UINT32_MAX){x=(x+1)/2;y=(y+1)/2;}fmt_put(bytes,56,x,4);fmt_put(bytes,60,y,4);}
        std::uint32_t control=0;const auto pad=attribute_scalar(item,key_pad);if(pad==1)control|=3;else if(pad==2)control|=5;
        const std::array<std::pair<Guid,std::pair<unsigned,std::uint32_t>>,6> colors{{{key_chroma,{8,0xf}},{key_nominal,{12,7}},{key_matrix,{15,7}},{key_lighting,{18,0xf}},{key_primaries,{22,0x1f}},{key_transfer,{27,0x1f}}}};
        for(const auto& [key,field]:colors)if(attribute(item,key)){control|=0x81;control|=(static_cast<std::uint32_t>(attribute_scalar(item,key))&field.second)<<field.first;}
        fmt_put(bytes,64,control,4);
    }
    fmt_put(bytes,bitmap,40,4);fmt_put(bytes,bitmap+4,width,4);fmt_put(bytes,bitmap+8,static_cast<std::uint32_t>(bottom_up || !layout.rgb?static_cast<std::int32_t>(height):-static_cast<std::int32_t>(height)),4);
    fmt_put(bytes,bitmap+12,1,2);fmt_put(bytes,bitmap+14,known?layout.bits*layout.scale/layout.divisor:0,2);
    fmt_put(bytes,bitmap+16,layout.rgb?(fields?3U:0U):code?code:static_cast<std::uint32_t>(fmt_le(subtype,0,4)),4);
    MfImageWord image=0;if(!attribute_scalar(item,mf::MF_MT_SAMPLE_SIZE) && known)(void)mf_image_size(code,width,height,1,&image);
    fmt_put(bytes,bitmap+20,attribute_scalar(item,mf::MF_MT_SAMPLE_SIZE,image),4);fmt_put(bytes,bitmap+32,palette.size()/4,4);
    if(trailing){
        if(fields)for(const std::uint32_t mask:{0xf800U,0x7e0U,0x1fU}){bytes.resize(bytes.size()+4);fmt_put(bytes,bytes.size()-4,mask,4);}
        bytes.insert(bytes.end(),palette.begin(),palette.end());bytes.insert(bytes.end(),user.begin(),user.end());
    }
    return bytes;};
const auto mpeg_bytes=[&](const Object& item,unsigned version){
    auto bytes=video_info_bytes(item,version,false,0);const auto sequence=attribute_blob(item,mf::MF_MT_MPEG_SEQUENCE_HEADER);
    const std::size_t base=bytes.size();bytes.resize(base+(version==2?20:8)+sequence.size());
    fmt_put(bytes,base,attribute_scalar(item,key_start_time_code),4);fmt_put(bytes,base+4,sequence.size(),4);
    if(version==2){fmt_put(bytes,base+8,attribute_scalar(item,mf::MF_MT_MPEG2_PROFILE),4);fmt_put(bytes,base+12,attribute_scalar(item,mf::MF_MT_MPEG2_LEVEL),4);fmt_put(bytes,base+16,attribute_scalar(item,key_mpeg2_flags),4);}
    std::copy(sequence.begin(),sequence.end(),bytes.end()-static_cast<std::ptrdiff_t>(sequence.size()));return bytes;};
// WAVEFORMATEX（MFInitMediaTypeFromWaveFormatEx 與 AM_MEDIA_TYPE 共用）。
const auto init_wave=[&](Object& item,std::span<const std::uint8_t> bytes){
    require(bytes.size()>=16);const auto load=[&](unsigned at,unsigned width){return static_cast<std::uint32_t>(fmt_le(bytes,at,width));};
    const auto tag=load(0,2),channels=load(2,2),rate=load(4,4),avg=load(8,4),align=load(12,2),depth=load(14,2);Guid subtype=media_guid(tag);std::uint32_t mask=0;
    require(channels && rate && align);if(tag==0xfffe){require(bytes.size()>=40 && load(16,2)>=22);std::copy_n(bytes.begin()+24,16,subtype.begin());mask=load(20,4);}else if(bytes.size()>=18)require(18+load(16,2)<=bytes.size());
    const bool pcm=subtype==mf::pcm || subtype==mf::ieee;if(pcm)require(depth && align==(channels*depth+7)/8 && avg==static_cast<std::uint64_t>(rate)*align);
    set(item,mf::MF_MT_MAJOR_TYPE,valueguid(mf::MFMediaType_Audio));set(item,mf::MF_MT_SUBTYPE,valueguid(subtype));
    for(const auto& pair:std::array<std::pair<Guid,std::uint32_t>,9>{{{mf::MF_MT_AUDIO_NUM_CHANNELS,channels},{mf::MF_MT_AUDIO_SAMPLES_PER_SECOND,rate},{mf::MF_MT_AUDIO_AVG_BYTES_PER_SECOND,avg},{mf::MF_MT_AUDIO_BLOCK_ALIGNMENT,align},{mf::MF_MT_AUDIO_BITS_PER_SAMPLE,depth},{mf::MF_MT_AUDIO_CHANNEL_MASK,mask},{mf::MF_MT_COMPRESSED,pcm?0U:1U},{mf::MF_MT_FIXED_SIZE_SAMPLES,pcm?1U:0U},{mf::MF_MT_SAMPLE_SIZE,align}}})set(item,pair.first,value32(pair.second));};
// MFCreateWaveFormatExFromMFMediaType 的轉換（PCM／IEEE float）；extended 為 WAVEFORMATEXTENSIBLE。
const auto pcm_wave_bytes=[&](const Object& item,bool extended){
    require(get(item,mf::MF_MT_MAJOR_TYPE,72).id==mf::MFMediaType_Audio,invalid_media_type);const auto subtype=get(item,mf::MF_MT_SUBTYPE,72).id;
    require(subtype==mf::pcm || subtype==mf::ieee,invalid_media_type);const auto channels=get(item,mf::MF_MT_AUDIO_NUM_CHANNELS,19).scalar,rate=get(item,mf::MF_MT_AUDIO_SAMPLES_PER_SECOND,19).scalar,align=get(item,mf::MF_MT_AUDIO_BLOCK_ALIGNMENT,19).scalar,depth=get(item,mf::MF_MT_AUDIO_BITS_PER_SAMPLE,19).scalar,avg=get(item,mf::MF_MT_AUDIO_AVG_BYTES_PER_SECOND,19).scalar;
    require(channels && channels<=UINT16_MAX && depth && depth<=UINT16_MAX && align<=UINT16_MAX && avg==rate*align);std::vector<std::uint8_t> bytes(extended?40U:18U);
    fmt_put(bytes,0,extended?0xfffe:subtype==mf::pcm?1:3,2);fmt_put(bytes,2,channels,2);fmt_put(bytes,4,rate,4);fmt_put(bytes,8,avg,4);fmt_put(bytes,12,align,2);fmt_put(bytes,14,depth,2);fmt_put(bytes,16,extended?22:0,2);
    if(extended){fmt_put(bytes,18,depth,2);fmt_put(bytes,20,attribute_scalar(item,mf::MF_MT_AUDIO_CHANNEL_MASK),4);std::copy(subtype.begin(),subtype.end(),bytes.begin()+24);}
    return bytes;};
// AM_MEDIA_TYPE 的音訊格式區塊：PCM／float 沿用上面的轉換；其他格式以子型別的格式標籤（非基本 GUID 時為 WAVEFORMATEXTENSIBLE）並附上 MF_MT_USER_DATA。
const auto wave_bytes=[&](const Object& item){
    const auto subtype=subtype_of(item);if(subtype==mf::pcm || subtype==mf::ieee)return pcm_wave_bytes(item,!(attribute_scalar(item,mf::MF_MT_AUDIO_CHANNEL_MASK)==0 && attribute_scalar(item,mf::MF_MT_AUDIO_NUM_CHANNELS)<=2));
    const auto user=attribute_blob(item,mf::MF_MT_USER_DATA);const auto code=media_code(subtype);const bool extended=subtype!=media_guid(code) || code>UINT16_MAX;
    require(user.size()<=UINT16_MAX-22);std::vector<std::uint8_t> bytes((extended?40U:18U)+user.size());
    fmt_put(bytes,0,extended?0xfffe:code,2);fmt_put(bytes,2,attribute_scalar(item,mf::MF_MT_AUDIO_NUM_CHANNELS),2);fmt_put(bytes,4,attribute_scalar(item,mf::MF_MT_AUDIO_SAMPLES_PER_SECOND),4);
    fmt_put(bytes,8,attribute_scalar(item,mf::MF_MT_AUDIO_AVG_BYTES_PER_SECOND),4);fmt_put(bytes,12,attribute_scalar(item,mf::MF_MT_AUDIO_BLOCK_ALIGNMENT),2);
    fmt_put(bytes,14,attribute_scalar(item,mf::MF_MT_AUDIO_BITS_PER_SAMPLE),2);fmt_put(bytes,16,(extended?22:0)+user.size(),2);
    if(extended){fmt_put(bytes,18,attribute_scalar(item,mf::MF_MT_AUDIO_BITS_PER_SAMPLE),2);fmt_put(bytes,20,attribute_scalar(item,mf::MF_MT_AUDIO_CHANNEL_MASK),4);std::copy(subtype.begin(),subtype.end(),bytes.begin()+24);}
    std::copy(user.begin(),user.end(),bytes.end()-static_cast<std::ptrdiff_t>(user.size()));return bytes;};
// AM_MEDIA_TYPE：x86 為 72 位元組（pUnk 60、cbFormat 64、pbFormat 68），x64 為 88 位元組（pUnk 64、cbFormat 72、pbFormat 80）；pUnk 一律為 NULL。
const std::size_t am_size=x86_?72:88,am_length=x86_?64:72,am_format=x86_?68:80;
// guidFormatBlockType 為 GUID_NULL 或 AM_MEDIA_TYPE_REPRESENTATION 時：有 MF_MT_AM_FORMAT_TYPE 用它，否則音訊 WAVEFORMATEX、MPEG-2 視訊 MPEG2VIDEOINFO、其他視訊 VIDEOINFOHEADER。
const auto am_block=[&](const Object& item,Guid requested)->std::pair<Guid,std::vector<std::uint8_t>>{
    const auto* major=attribute(item,mf::MF_MT_MAJOR_TYPE);require(major && major->type==72,missing);
    const bool audio=major->id==mf::MFMediaType_Audio,video=major->id==mf::MFMediaType_Video;
    if(requested==Guid{} || requested==fmt_am_representation){
        if(const auto* stored=attribute(item,key_am_format);stored && stored->type==72)requested=stored->id;
        else requested=audio?fmt_wave:video?(subtype_of(item)==fmt_mpeg2_subtype?fmt_mpeg2:fmt_video_info):Guid{};
    }
    if(requested==fmt_wave){require(audio,unsupported_representation);return {requested,wave_bytes(item)};}
    if(requested==fmt_video_info || requested==fmt_video_info2){require(video,unsupported_representation);return {requested,video_info_bytes(item,requested==fmt_video_info?1:2,true,0)};}
    if(requested==fmt_mpeg1 || requested==fmt_mpeg2){require(video,unsupported_representation);return {requested,mpeg_bytes(item,requested==fmt_mpeg1?1:2)};}
    if(requested==fmt_mf_video){require(video,unsupported_representation);return {requested,mf_video_bytes(item)};}
    // 其他格式型別：只在媒體型別記錄了相同的 MF_MT_AM_FORMAT_TYPE 時，以 MF_MT_USER_DATA 作為格式區塊。
    const auto* stored=attribute(item,key_am_format);require(requested==Guid{} || (stored && stored->type==72 && stored->id==requested),unsupported_representation);
    return {requested,requested==Guid{}?std::vector<std::uint8_t>{}:attribute_blob(item,mf::MF_MT_USER_DATA)};};
const auto write_am=[&](std::uint64_t address,const Object& item,const Guid& requested){
    check(address,am_size);const auto [type,block]=am_block(item,requested);const auto& major=get(item,mf::MF_MT_MAJOR_TYPE,72).id;
    std::uint64_t pointer=0;
    if(!block.empty()){pointer=allocate(block.size());memory.write(GuestAddress{pointer},block);gui_.task_allocations.insert(pointer);}
    std::vector<std::uint8_t> bytes(am_size);std::copy(major.begin(),major.end(),bytes.begin());
    const auto subtype=major==mf::MFMediaType_Video?to_dshow(subtype_of(item)):subtype_of(item);std::copy(subtype.begin(),subtype.end(),bytes.begin()+16);
    fmt_put(bytes,32,attribute_scalar(item,mf::MF_MT_FIXED_SIZE_SAMPLES)?1:0,4);fmt_put(bytes,36,attribute_scalar(item,mf::MF_MT_ALL_SAMPLES_INDEPENDENT)?0:1,4);
    fmt_put(bytes,40,attribute_scalar(item,mf::MF_MT_SAMPLE_SIZE),4);std::copy(type.begin(),type.end(),bytes.begin()+44);
    fmt_put(bytes,am_length,block.size(),4);fmt_put(bytes,am_format,pointer,x86_?4:8);memory.write(GuestAddress{address},bytes);};
const auto read_am=[&](Object& item,std::uint64_t address){
    require(address!=0,pointer_error);const auto bytes=data(address,am_size);Guid major{},subtype{},type{};
    std::copy_n(bytes.begin(),16,major.begin());std::copy_n(bytes.begin()+16,16,subtype.begin());std::copy_n(bytes.begin()+44,16,type.begin());
    const auto length=static_cast<std::uint32_t>(fmt_le(bytes,am_length,4));const auto pointer=fmt_le(bytes,am_format,x86_?4:8);const auto block=data(pointer,length);
    if(type==fmt_wave)init_wave(item,block);
    else if(type==fmt_video_info || type==fmt_video_info2)init_video(item,[&]{auto header=video_header_of(block,type==fmt_video_info?1:2);header.subtype=subtype;return header;}());
    else if(type==fmt_mpeg1 || type==fmt_mpeg2)init_mpeg(item,block,type==fmt_mpeg1?1:2,subtype);
    else if(type==fmt_mf_video)init_mf_video(item,block);
    else {set(item,mf::MF_MT_MAJOR_TYPE,valueguid(major));set(item,mf::MF_MT_SUBTYPE,valueguid(major==mf::MFMediaType_Video?from_dshow(subtype):subtype));
        if(!block.empty()){Value value;value.type=0x1011;value.blob=block;set(item,mf::MF_MT_USER_DATA,std::move(value));}}
    if(type==fmt_wave || type==fmt_video_info || type==fmt_video_info2 || type==fmt_mpeg1 || type==fmt_mpeg2 || type==fmt_mf_video)
        if(major!=Guid{})require(get(item,mf::MF_MT_MAJOR_TYPE,72).id==major,invalid_media_type);
    set(item,mf::MF_MT_FIXED_SIZE_SAMPLES,value32(fmt_le(bytes,32,4)?1:0));set(item,mf::MF_MT_ALL_SAMPLES_INDEPENDENT,value32(fmt_le(bytes,36,4)?0:1));
    put_nonzero(item,mf::MF_MT_SAMPLE_SIZE,fmt_le(bytes,40,4));if(type!=Guid{})set(item,key_am_format,valueguid(type));};
const auto free_am=[&](std::uint64_t address){
    if(!address)return;const auto pointer=memory.load(GuestAddress{address+am_format},x86_?32:64);
    if(pointer){gui_.task_allocations.erase(pointer);(void)free_heap(memory,pointer,state);}
    gui_.task_allocations.erase(address);(void)free_heap(memory,address,state);};
// D3DFORMAT／FOURCC 與 DXGI_FORMAT 對照（MFMapDX9FormatToDXGIFormat／MFMapDXGIFormatToDX9Format 與樣本配置器的 D3D11 材質共用），未列出者為 0。
static constexpr std::array<std::pair<std::uint32_t,std::uint32_t>,48> dxgi{{
    {116,2},{113,10},{36,11},{110,13},{115,16},{31,24},{32,28},{63,31},{112,34},{34,35},{64,37},{114,41},{111,54},{28,65},{21,87},{22,88},{23,85},{25,86},{26,115},
    {MF_IMAGE_FOURCC('D','X','T','1'),71},{MF_IMAGE_FOURCC('D','X','T','3'),74},{MF_IMAGE_FOURCC('D','X','T','2'),74},{MF_IMAGE_FOURCC('D','X','T','5'),77},{MF_IMAGE_FOURCC('D','X','T','4'),77},
    {50,61},{51,49},{81,56},{60,51},{80,55},{82,40},{75,45},
    {MF_IMAGE_FOURCC('A','Y','U','V'),100},{MF_IMAGE_FOURCC('Y','4','1','0'),101},{MF_IMAGE_FOURCC('Y','4','1','6'),102},{MF_IMAGE_FOURCC('N','V','1','2'),103},
    {MF_IMAGE_FOURCC('P','0','1','0'),104},{MF_IMAGE_FOURCC('P','0','1','6'),105},{MF_IMAGE_FOURCC('4','2','0','O'),106},{MF_IMAGE_FOURCC('Y','U','Y','2'),107},
    {MF_IMAGE_FOURCC('Y','2','1','0'),108},{MF_IMAGE_FOURCC('Y','2','1','6'),109},{MF_IMAGE_FOURCC('N','V','1','1'),110},{MF_IMAGE_FOURCC('A','I','4','4'),111},
    {MF_IMAGE_FOURCC('I','A','4','4'),112},{41,113},{40,114},{MF_IMAGE_FOURCC('P','2','0','8'),130},{MF_IMAGE_FOURCC('V','2','0','8'),131}}};
// 2D 緩衝：宿主儲存區＋MediaSurface 讀寫，鎖定時由 map_buffer／flush_buffer 與客體記憶體同步。
const auto buffer_2d=[&](std::uint32_t width,std::uint32_t height,std::uint32_t code,bool bottom_up){
    MfImageLong stride=0;require(width && height && !mf_image_stride(code,width,&stride),invalid_media_type);
    MfImageWord size=0;const auto status=mf_image_size(code,width,height,1,&size);require(!status,status);require(size!=0,invalid_media_type);
    const auto pitch=static_cast<std::uint32_t>(stride<0?-static_cast<std::int64_t>(stride):stride);require(pitch!=0 && size%pitch==0,invalid_media_type);
    auto storage=std::make_shared<std::vector<std::uint8_t>>(size);
    MediaSurface surface;surface.row_pitch=pitch;surface.height=size/pitch;surface.size=size;surface.format=code;
    surface.read=[storage]{return *storage;};
    surface.write=[storage](std::span<const std::uint8_t> bytes){std::copy_n(bytes.begin(),std::min(bytes.size(),storage->size()),storage->begin());};
    const auto p=make(mf::Buffer);auto item=object(p,mf::Buffer);item->surface=std::move(surface);item->bottom_up=bottom_up;item->maximum=item->length=size;return p;};

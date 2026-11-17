// mfplat 的媒體型別建立與格式轉換（由 windows_media_foundation.cpp 的匯出鏈引入；對應規則在 mf_format_helpers.inc）：
// - MFCreateVideoMediaTypeFromSubtype／MFCreateVideoMediaType／MFCreateAudioMediaType／MFCreateVideoMediaTypeFromVideoInfoHeader：建立 IMFVideoMediaType／IMFAudioMediaType
//   （同一個媒體型別物件；GetVideoFormat 與 GetAudioFormat 佔同一個方法位置，依主要型別回傳 MFVIDEOFORMAT 或 WAVEFORMATEX）。
// - MFInitMediaTypeFromVideoInfoHeader／2、MFInitMediaTypeFromMPEG1VideoInfo／MPEG2VideoInfo、MFInitMediaTypeFromMFVideoFormat、MFInitMediaTypeFromAMMediaType：設定屬性。
// - MFInitAMMediaTypeFromMFMediaType／MFCreateAMMediaTypeFromMFMediaType、MFCreateMFVideoFormatFromMFMediaType：格式區塊以 CoTaskMemAlloc 配置。
// - MFCreateMediaTypeFromRepresentation：只接受 AM_MEDIA_TYPE_REPRESENTATION。MFInitVideoFormat_RGB：RGB 的預設 MFVIDEOFORMAT。
// - MFConvertColorInfoToDXVA：DXVA_ExtendedFormat 位元欄位（SampleFormat 8、ChromaSubsampling 4、NominalRange 3、TransferMatrix 3、Lighting 4、Primaries 5、TransferFunction 5）。
// - MFMapDX9FormatToDXGIFormat／MFMapDXGIFormatToDX9Format：D3DFORMAT 與 DXGI_FORMAT 對照，未知格式為 0。
if(name=="MFCreateVideoMediaTypeFromSubtype" || name=="MFCreateVideoMediaType" || name=="MFCreateAudioMediaType" || name=="MFCreateVideoMediaTypeFromVideoInfoHeader" ||
   name=="MFInitMediaTypeFromVideoInfoHeader" || name=="MFInitMediaTypeFromVideoInfoHeader2" || name=="MFInitMediaTypeFromMPEG1VideoInfo" || name=="MFInitMediaTypeFromMPEG2VideoInfo" ||
   name=="MFInitMediaTypeFromMFVideoFormat" || name=="MFInitMediaTypeFromAMMediaType" || name=="MFInitAMMediaTypeFromMFMediaType" || name=="MFCreateAMMediaTypeFromMFMediaType" ||
   name=="MFCreateMFVideoFormatFromMFMediaType" || name=="MFCreateMediaTypeFromRepresentation" || name=="MFInitVideoFormat_RGB" || name=="MFConvertColorInfoToDXVA" ||
   name=="MFMapDX9FormatToDXGIFormat" || name=="MFMapDXGIFormatToDX9Format"){
    // 新建立的媒體型別：設定失敗時釋放。
    const auto create_type=[&](std::uint64_t output,const auto& fill){
        out(output);const auto p=make(mf::MediaType);try{fill(*object(p,mf::MediaType));}catch(...){release(p);throw;}put(output,p,bits);};
    if(name=="MFCreateVideoMediaTypeFromSubtype"){
        const auto subtype=guid(arg(0));create_type(arg(1),[&](Object& item){set(item,mf::MF_MT_MAJOR_TYPE,valueguid(mf::MFMediaType_Video));set(item,mf::MF_MT_SUBTYPE,valueguid(subtype));});
    }else if(name=="MFCreateVideoMediaType" || name=="MFInitMediaTypeFromMFVideoFormat"){
        const bool creating=name=="MFCreateVideoMediaType";const auto format=arg(creating?0:1);require(format!=0,pointer_error);
        const auto size=creating?static_cast<std::uint32_t>(memory.load(GuestAddress{format},32)):d(2);const auto bytes=data(format,size);
        if(creating)create_type(arg(1),[&](Object& item){init_mf_video(item,bytes);});else init_mf_video(*object(arg(0),mf::MediaType),bytes);
    }else if(name=="MFCreateAudioMediaType"){
        // WAVE_FORMAT_PCM 可能是 16 位元組的 PCMWAVEFORMAT；其他格式讀 cbSize。
        require(arg(0)!=0,pointer_error);const auto tag=memory.load(GuestAddress{arg(0)},16);
        const auto size=tag==1?16U:18U+static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+16},16));const auto bytes=data(arg(0),size);
        create_type(arg(1),[&](Object& item){init_wave(item,bytes);});
    }else if(name=="MFCreateVideoMediaTypeFromVideoInfoHeader"){
        // (KS_VIDEOINFOHEADER*, cbVideoInfoHeader, dwPixelAspectRatioX, dwPixelAspectRatioY, InterlaceMode, VideoFlags, pSubtype, ppIVideoMediaType)
        require(arg(0)!=0,pointer_error);const auto bytes=data(arg(0),d(1));auto header=video_header_of(bytes,1);if(arg(6))header.subtype=guid(arg(6));
        const auto flags=arg(5);
        create_type(arg(7),[&](Object& item){
            init_video(item,header);if(d(2) && d(3))set(item,mf::MF_MT_PIXEL_ASPECT_RATIO,value64(static_cast<std::uint64_t>(d(2))<<32|d(3)));
            if(d(4))set(item,mf::MF_MT_INTERLACE_MODE,value32(d(4)));apply_video_flags(item,flags);});
    }else if(name=="MFInitMediaTypeFromVideoInfoHeader" || name=="MFInitMediaTypeFromVideoInfoHeader2"){
        auto item=object(arg(0),mf::MediaType);require(arg(1)!=0,pointer_error);const auto bytes=data(arg(1),d(2));
        auto header=video_header_of(bytes,name.ends_with('2')?2:1);if(arg(3))header.subtype=guid(arg(3));init_video(*item,header);
    }else if(name=="MFInitMediaTypeFromMPEG1VideoInfo" || name=="MFInitMediaTypeFromMPEG2VideoInfo"){
        auto item=object(arg(0),mf::MediaType);require(arg(1)!=0,pointer_error);const auto bytes=data(arg(1),d(2));
        init_mpeg(*item,bytes,name.find("MPEG2")!=std::string_view::npos?2:1,arg(3)?std::optional<Guid>{guid(arg(3))}:std::nullopt);
    }else if(name=="MFInitMediaTypeFromAMMediaType"){read_am(*object(arg(0),mf::MediaType),arg(1));}
    else if(name=="MFInitAMMediaTypeFromMFMediaType"){write_am(arg(2),*object(arg(0),mf::MediaType),guid(arg(1)));}
    else if(name=="MFCreateAMMediaTypeFromMFMediaType"){
        out(arg(2));auto item=object(arg(0),mf::MediaType);const auto requested=guid(arg(1));const auto p=allocate(am_size);gui_.task_allocations.insert(p);
        try{write_am(p,*item,requested);}catch(...){gui_.task_allocations.erase(p);(void)free_heap(memory,p,state);throw;}put(arg(2),p,bits);
    }else if(name=="MFCreateMFVideoFormatFromMFMediaType"){
        out(arg(1));if(arg(2))put(arg(2),0);const auto bytes=mf_video_bytes(*object(arg(0),mf::MediaType));
        const auto p=allocate(bytes.size());memory.write(GuestAddress{p},bytes);gui_.task_allocations.insert(p);put(arg(1),p,bits);if(arg(2))put(arg(2),bytes.size());
    }else if(name=="MFCreateMediaTypeFromRepresentation"){
        require(guid(arg(0))==fmt_am_representation,unsupported_representation);require(arg(1)!=0,pointer_error);
        create_type(arg(2),[&](Object& item){read_am(item,arg(1));});
    }else if(name=="MFInitVideoFormat_RGB"){
        // (pVideoFormat, dwWidth, dwHeight, D3Dfmt)：0 為 X8R8G8B8；像素長寬比 1:1、逐行、0～255、BT.709 原色、sRGB 轉換函數。
        require(arg(0)!=0,pointer_error);const auto format=d(3)?d(3):22U;const auto layout=mf_image_layout(format,d(1));require(layout.rgb,invalid_media_type);
        std::vector<std::uint8_t> bytes(176);fmt_put(bytes,0,176,4);fmt_put(bytes,8,d(1),4);fmt_put(bytes,12,d(2),4);fmt_put(bytes,16,1,4);fmt_put(bytes,20,1,4);
        fmt_put(bytes,28,2,4);fmt_put(bytes,32,7,4);fmt_put(bytes,36,2,4);fmt_put(bytes,56,1,4);
        const auto subtype=media_guid(format);std::copy(subtype.begin(),subtype.end(),bytes.begin()+120);fmt_put(bytes,160,format,4);
        check(arg(0),bytes.size());memory.write(GuestAddress{arg(0)},bytes);
    }else if(name=="MFConvertColorInfoToDXVA"){
        require(arg(0)!=0 && arg(1)!=0,pointer_error);const auto bytes=data(arg(1),60);const auto field=[&](std::size_t at){return static_cast<std::uint32_t>(fmt_le(bytes,at,4));};
        const auto value=(field(28)&0xff)|(field(24)&0xf)<<8|(field(56)&7)<<12|(field(40)&7)<<15|(field(44)&0xf)<<18|(field(36)&0x1f)<<22|(field(32)&0x1f)<<27;
        put(arg(0),value);
    }else if(name=="MFMapDX9FormatToDXGIFormat"){result=0;for(const auto& [dx9,format]:dxgi)if(dx9==d(0)){result=format;break;}}
    else if(name=="MFMapDXGIFormatToDX9Format"){result=0;for(const auto& [dx9,format]:dxgi)if(format==d(0)){result=dx9;break;}}
}

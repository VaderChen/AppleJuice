// mfplat 的檔案位元組串流與緩衝建立（由 windows_media_foundation.cpp 的匯出鏈引入）：
// - MFCreateFile／MFBeginCreateFile／MFCreateTempFile 共用同一個開檔流程（存取模式 1～3、開啟模式 0～4、MF_FILEFLAGS_NOBUFFERING／ALLOW_WRITE_SHARING）。
//   MFBeginCreateFile 同步開檔後把結果放入標準工作佇列，回呼由 poll_media_foundation 派送；取消 cookie 即非同步結果物件，
//   MFCancelCreateFile 對尚未派送的要求撤銷（不呼叫回呼），已派送時不做事。MFEndCreateFile 取回位元組串流或開檔錯誤。
// - MFCreateTempFile：在客體暫存目錄以 GetTempFileNameW 取唯一名稱，關閉時刪除（FILE_FLAG_DELETE_ON_CLOSE）。
// - MFCreate2DMediaBuffer：記憶體 2D 緩衝，步幅與大小沿用 MF 影像工具；以宿主儲存區提供 IMF2DBuffer／IMF2DBuffer2 的 surface 讀寫，fBottomUp 時線性鎖定為上下翻轉。
// - MFCreateMediaBufferFromMediaType：視訊為 2D 緩衝（預設步幅為負時由下而上），音訊依時間長度、平均位元組率與區塊對齊計算大小，其他為最小長度的記憶體緩衝。
// - MFCreateLegacyMediaBufferOnMFMediaBuffer：DMO 的 IMediaBuffer（SetLength／GetMaxLength／GetBufferAndLength），自 cbOffset 起對應 MF 緩衝並持有它與樣本。
if(name=="MFCreateFile" || name=="MFBeginCreateFile" || name=="MFEndCreateFile" || name=="MFCancelCreateFile" || name=="MFCreateTempFile" ||
   name=="MFCreate2DMediaBuffer" || name=="MFCreateMediaBufferFromMediaType" || name=="MFCreateLegacyMediaBufferOnMFMediaBuffer"){
    const auto open_stream=[&](std::uint32_t access_mode,std::uint32_t open_mode,std::uint32_t flags,const std::u16string& path,std::uint32_t extra_flags){
        require(access_mode>=1 && access_mode<=3 && open_mode<=4 && !(flags&~3U));require(!path.empty());
        constexpr std::array<std::uint32_t,5> dispositions{3,1,5,4,2};
        const auto access=(access_mode&1?0x80000000U:0U)|(access_mode&2?0x40000000U:0U);
        const auto opened=filesystem_.open_host(utf16_to_utf8(path),access|(extra_flags&0x04000000U?0x10000U:0U),dispositions[open_mode],(flags&1?0x20000000U:0U)|extra_flags,flags&2?3U:1U);
        require(!opened.error && bool(opened.file),sink_io_error(opened.error?opened.error:2));
        auto stream=std::make_shared<ByteStreamState>();stream->file=opened.file;stream->capabilities=access_mode|4U|0x800U|(flags&2?0x400U:0U);
        const auto p=make(mf::ByteStream);object(p,mf::ByteStream)->byte_stream=std::move(stream);return p;};
    if(name=="MFCreateFile"){
        out(arg(4));require(arg(3)!=0,pointer_error);put(arg(4),open_stream(d(0),d(1),d(2),read_utf16(memory,GuestAddress{arg(3)}),0),bits);
    }else if(name=="MFBeginCreateFile"){
        // (AccessMode, OpenMode, fFlags, pwszFilePath, pCallback, pState, ppCancelCookie)
        if(arg(6))put(arg(6),0,bits);require(arg(3)!=0 && arg(4)!=0,pointer_error);(void)foreign_method(arg(4),4);
        std::uint64_t stream=0;std::uint32_t status=0;
        try{stream=open_stream(d(0),d(1),d(2),read_utf16(memory,GuestAddress{arg(3)}),0);}catch(const Failure& failure){if(failure.code==invalid || failure.code==pointer_error)throw;status=failure.code;}
        const auto p=make(mf::Async);auto completion=object(p,mf::Async);hold(arg(4));hold(arg(5));completion->callback=arg(4);completion->state=arg(5);
        completion->result_object=stream;completion->status=status;bridge.work_items.push_back({p,foreign_method(arg(4),4),arg(4),1});
        if(arg(6)){hold(p);put(arg(6),p,bits);}
    }else if(name=="MFEndCreateFile"){
        out(arg(1));auto completion=object(arg(0),mf::Async);require(!completion->status,completion->status);require(completion->result_object!=0,0x8000ffffU);
        hold(completion->result_object);put(arg(1),completion->result_object,bits);
    }else if(name=="MFCancelCreateFile"){
        auto completion=object(arg(0),mf::Async);
        if(const auto found=std::find_if(bridge.work_items.begin(),bridge.work_items.end(),[&](const auto& item){return item.result==arg(0);});found!=bridge.work_items.end()){
            bridge.work_items.erase(found);completion->status=0xc00d36edU;release(arg(0));}
    }else if(name=="MFCreateTempFile"){
        // (AccessMode, OpenMode, fFlags, ppByteStream)
        out(arg(3));GuestScratch scratch(*this,memory,state);const auto directory=scratch.allocate(2*(260+1)),file=scratch.allocate(2*(260+1)),prefix=scratch.text(u"MFT");
        require(directory && file && prefix,0x8007000eU);
        const auto length=static_cast<std::uint32_t>(nested_service(state,memory,"kernel32.dll","GetTempPathW",{261,directory}));require(length && length<=260,0x80004005U);
        require(static_cast<std::uint32_t>(nested_service(state,memory,"kernel32.dll","GetTempFileNameW",{directory,prefix,0,file}))!=0,0x80004005U);
        // GetTempFileNameW 已建立空檔；以 OpenMode 指定的處置開啟（不存在時建立），關閉時刪除。
        put(arg(3),open_stream(d(0),d(1),d(2),read_utf16(memory,GuestAddress{file},260),0x04000000U),bits);
    }else if(name=="MFCreate2DMediaBuffer"){
        // (dwWidth, dwHeight, dwFourCC, fBottomUp, ppBuffer)
        out(arg(4));put(arg(4),buffer_2d(d(0),d(1),d(2),d(3)!=0),bits);
    }else if(name=="MFCreateMediaBufferFromMediaType"){
        // (pMediaType, llDuration, dwMinLength, dwMinAlignment, ppBuffer)：dwMinAlignment 可為對齊值（2 的冪）或 MF_*_BYTE_ALIGNMENT 遮罩。
        out(arg(4));const auto type=object(arg(0),mf::MediaType);const auto duration=static_cast<std::int64_t>(arg(1));const auto minimum=d(2);
        auto mask=d(3);if(mask && !(mask&(mask-1)))mask-=1;require(mask<=8191 && !(mask&(mask+1)));
        const auto* major=attribute(*type,mf::MF_MT_MAJOR_TYPE);require(major && major->type==72,missing);
        std::uint64_t p=0;
        if(major->id==mf::MFMediaType_Video){
            const auto frame=attribute_scalar(*type,mf::MF_MT_FRAME_SIZE);const auto code=media_code(subtype_of(*type));const auto layout=mf_image_layout(code,static_cast<std::uint32_t>(frame>>32));
            if(code && (layout.bits || layout.stride) && frame>>32 && (frame&UINT32_MAX))
                p=buffer_2d(static_cast<std::uint32_t>(frame>>32),static_cast<std::uint32_t>(frame),code,static_cast<std::int32_t>(attribute_scalar(*type,mf::MF_MT_DEFAULT_STRIDE,1))<0);
            else p=memory_buffer(std::max<std::uint32_t>(minimum,static_cast<std::uint32_t>(attribute_scalar(*type,mf::MF_MT_SAMPLE_SIZE,1))),mask);
        }else if(major->id==mf::MFMediaType_Audio){
            const auto rate=attribute_scalar(*type,mf::MF_MT_AUDIO_AVG_BYTES_PER_SECOND),align=std::max<std::uint64_t>(1,attribute_scalar(*type,mf::MF_MT_AUDIO_BLOCK_ALIGNMENT,1));
            std::uint64_t size=duration>0?(rate*static_cast<std::uint64_t>(duration)+9999999)/10000000:0;size=std::max<std::uint64_t>(size,minimum);
            size=std::max<std::uint64_t>((size+align-1)/align*align,align);require(size<=256U*1024U*1024U,0x8007000eU);p=memory_buffer(static_cast<std::uint32_t>(size),mask);
        }else p=memory_buffer(std::max<std::uint32_t>(minimum,1),mask);
        put(arg(4),p,bits);
    }else if(name=="MFCreateLegacyMediaBufferOnMFMediaBuffer"){
        // (pSample, pMFMediaBuffer, cbOffset, ppMediaBuffer)
        out(arg(3));auto buffer=object(arg(1),mf::Buffer);require(d(2)<=buffer->maximum);if(arg(0))(void)object(arg(0),mf::Sample);
        const auto p=make(mf::LegacyBuffer);auto item=object(p,mf::LegacyBuffer);item->parent=arg(1);item->state=arg(0);item->identifier=d(2);hold(arg(1));hold(arg(0));put(arg(3),p,bits);
    }
}

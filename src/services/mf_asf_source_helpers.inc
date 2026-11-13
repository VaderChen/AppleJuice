// ASF 媒體來源：沿用 Source／Stream／Presentation 物件與同一份 byte stream 讀取路徑，容器解析與封包解多工由 mf_asf_demux.hpp 提供。
// 原生型別交付壓縮樣本；Source Reader 要求未壓縮輸出時，由內建的 VC-1／WMV／WMA 解碼器（x64arm/media_decode.hpp）解碼，
// 色彩配置與 PCM 轉換沿用 MF 既有的 video_layout／AudioTranscoder。
// Media Session 的拓撲尚未接上 ASF 解碼流程：依 Windows 找不到解碼器時的行為回報 MF_E_TOPO_CODEC_NOT_FOUND，不回報成功後交出空白資料。
const auto asf_require_decoder=[&](const Object& native,const Object* requested){
    (void)native;(void)requested;throw Failure{0xc00d5212U};
};
// Source Reader「要求解碼」的唯一判斷點：依原生串流與要求的型別建立解碼輸出，回傳完整的輸出型別（呼叫端持有）。
// 沒有對應解碼器或無法轉成要求的格式時回報 MF_E_TOPO_CODEC_NOT_FOUND；型別內容與串流不符時回報 MF_E_INVALIDMEDIATYPE。
const auto asf_decode_output=[&](const mf::AsfStream& stream,const Object& requested,std::unique_ptr<AsfStreamOutput>& output)->std::uint64_t{
    const auto find=[&](const Guid& key,std::uint32_t type)->const Value*{const auto found=requested.attributes.find(key);return found!=requested.attributes.end() && found->second.type==type?&found->second:nullptr;};
    const auto subtype=find(mf::MF_MT_SUBTYPE,72)?find(mf::MF_MT_SUBTYPE,72)->id:Guid{};
    auto result=std::make_unique<AsfStreamOutput>();
    const auto p=make(mf::MediaType);auto type=object(p,mf::MediaType);
    try{
        for(const auto& [key,value]:requested.attributes)set(*type,key,value);
        if(stream.video){
            require(media::video_decoder_available(stream.fourcc),0xc00d5212U);
            mf::VideoFormat format=mf::VideoFormat::Nv12;
            if(subtype==mf::nv12)format=mf::VideoFormat::Nv12;else if(subtype==mf::yv12)format=mf::VideoFormat::Yv12;else if(subtype==mf::iyuv || subtype==mf::i420)format=mf::VideoFormat::I420;
            else if(subtype==mf::yuy2)format=mf::VideoFormat::Yuy2;else if(subtype==mf::uyvy)format=mf::VideoFormat::Uyvy;else if(subtype==mf::rgb32 || subtype==mf::argb32)format=mf::VideoFormat::Bgra;else throw Failure{0xc00d5212U};
            const auto aligned=[](std::uint32_t value){return (value+15U)&~15U;};bool tight=true;
            if(const auto* extent=find(mf::MF_MT_FRAME_SIZE,21)){
                const auto w=static_cast<std::uint32_t>(extent->scalar>>32),h=static_cast<std::uint32_t>(extent->scalar);const bool same=w==stream.width && h==stream.height,padded=w==aligned(stream.width) && h==aligned(stream.height);
                require(same || padded,0xc00d36b4U);tight=same;
            }
            const auto* stride=find(mf::MF_MT_DEFAULT_STRIDE,19);
            result->layout=mf::video_layout(format,stream.width,stream.height,tight,stride?static_cast<std::int32_t>(stride->scalar):0);result->frame_time=stream.frame_time;
            result->video=std::make_unique<media::VideoDecoder>(media::CompressedVideo{stream.fourcc,stream.width,stream.height,stream.extra});
            set(*type,mf::MF_MT_MAJOR_TYPE,valueguid(mf::MFMediaType_Video));set(*type,mf::MF_MT_SUBTYPE,valueguid(subtype));
            set(*type,mf::MF_MT_FRAME_SIZE,value64((std::uint64_t{stream.width}<<32)|stream.height));
            if(const auto rate=mf::asf_frame_rate(stream.frame_time);rate.first)set(*type,mf::MF_MT_FRAME_RATE,value64((std::uint64_t{rate.first}<<32)|rate.second));
            set(*type,mf::MF_MT_PIXEL_ASPECT_RATIO,value64((std::uint64_t{stream.par_num}<<32)|stream.par_den));
            // 解碼器輸出整幀（交錯內容已組成完整畫面）。
            set(*type,mf::MF_MT_INTERLACE_MODE,value32(2));
            const auto signed_stride=result->layout.bottom_up?-static_cast<std::int32_t>(result->layout.stride):static_cast<std::int32_t>(result->layout.stride);
            set(*type,mf::MF_MT_DEFAULT_STRIDE,value32(static_cast<std::uint32_t>(signed_stride)));
            set(*type,mf::MF_MT_SAMPLE_SIZE,value32(static_cast<std::uint32_t>(result->layout.size)));
        }else{
            require(media::audio_decoder_available(stream.format_tag),0xc00d5212U);require(subtype==mf::pcm || subtype==mf::ieee,0xc00d5212U);
            result->audio=std::make_unique<media::AudioDecoder>(media::CompressedAudio{stream.format_tag,stream.channels,stream.block_align,stream.bits,stream.sample_rate,stream.bytes_per_second,stream.extra});
            const auto rate=result->audio->rate(),channels=result->audio->channels();const bool floating=subtype==mf::ieee;
            const auto number=[&](const Guid& key,unsigned fallback){const auto* value=find(key,19);return value && value->scalar?static_cast<unsigned>(value->scalar):fallback;};
            result->output={number(mf::MF_MT_AUDIO_SAMPLES_PER_SECOND,rate),number(mf::MF_MT_AUDIO_NUM_CHANNELS,channels),number(mf::MF_MT_AUDIO_BITS_PER_SAMPLE,floating?32:16),floating};
            const auto& out=result->output;require(floating?out.bits==32:out.bits==8 || out.bits==16 || out.bits==24 || out.bits==32,0xc00d36b4U);
            if(!(floating && out.bits==32 && out.rate==rate && out.channels==channels))result->transcoder=std::make_unique<mf::AudioTranscoder>(mf::AudioInput{false,{},rate,channels,32,channels*4U,true},out);
            set(*type,mf::MF_MT_MAJOR_TYPE,valueguid(mf::MFMediaType_Audio));set(*type,mf::MF_MT_SUBTYPE,valueguid(subtype));
            for(const auto& [key,value]:std::array<std::pair<Guid,std::uint32_t>,5>{{{mf::MF_MT_AUDIO_NUM_CHANNELS,out.channels},{mf::MF_MT_AUDIO_SAMPLES_PER_SECOND,out.rate},{mf::MF_MT_AUDIO_BITS_PER_SAMPLE,out.bits},{mf::MF_MT_AUDIO_BLOCK_ALIGNMENT,out.frame_bytes()},{mf::MF_MT_AUDIO_AVG_BYTES_PER_SECOND,out.rate*out.frame_bytes()}}})set(*type,key,value32(value));
            // 未指定時依聲道數使用標準喇叭配置（KSAUDIO_SPEAKER_*）。
            if(!find(mf::MF_MT_AUDIO_CHANNEL_MASK,19)){
                const std::uint32_t masks[]{0,0x4,0x3,0x7,0x33,0x37,0x3f,0x13f,0x63f};if(out.channels<std::size(masks))set(*type,mf::MF_MT_AUDIO_CHANNEL_MASK,value32(masks[out.channels]));
            }
        }
        for(const auto& key:{mf::MF_MT_COMPRESSED,mf::MF_MT_USER_DATA,mf::MF_MT_AVG_BITRATE})type->attributes.erase(key);
        set(*type,mf::MF_MT_FIXED_SIZE_SAMPLES,value32(stream.video));set(*type,mf::MF_MT_ALL_SAMPLES_INDEPENDENT,value32(1));
    }catch(const Failure&){release(p);throw;}
    catch(const std::invalid_argument&){release(p);throw Failure{0xc00d36b4U};}
    catch(const std::runtime_error&){release(p);throw Failure{0xc00d5212U};}
    output=std::move(result);return p;
};
const auto asf_media_type=[&](const mf::AsfStream& stream){
    const auto p=make(mf::MediaType);auto type=object(p,mf::MediaType);
    // FOURCC 與 wFormatTag 都以 {XXXXXXXX-0000-0010-8000-00AA00389B71} 組成 subtype。
    const auto subtype=[](std::uint32_t first){return mf::guid(first,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71});};
    if(stream.video){
        set(*type,mf::MF_MT_MAJOR_TYPE,valueguid(mf::MFMediaType_Video));set(*type,mf::MF_MT_SUBTYPE,valueguid(subtype(stream.fourcc)));
        set(*type,mf::MF_MT_FRAME_SIZE,value64((std::uint64_t{stream.width}<<32)|stream.height));
        if(const auto rate=mf::asf_frame_rate(stream.frame_time);rate.first)set(*type,mf::MF_MT_FRAME_RATE,value64((std::uint64_t{rate.first}<<32)|rate.second));
        set(*type,mf::MF_MT_PIXEL_ASPECT_RATIO,value64((std::uint64_t{stream.par_num}<<32)|stream.par_den));
        // 容器不記錄掃描方式；VC-1 Advanced Profile 可逐幀切換，回報 MFVideoInterlace_MixedInterlaceOrProgressive。
        set(*type,mf::MF_MT_INTERLACE_MODE,value32(7));
        set(*type,mf::MF_MT_COMPRESSED,value32(1));set(*type,mf::MF_MT_FIXED_SIZE_SAMPLES,value32(0));set(*type,mf::MF_MT_ALL_SAMPLES_INDEPENDENT,value32(0));
        if(stream.bitrate)set(*type,mf::MF_MT_AVG_BITRATE,value32(stream.bitrate));
    }else{
        const bool compressed=stream.format_tag!=1 && stream.format_tag!=3;
        set(*type,mf::MF_MT_MAJOR_TYPE,valueguid(mf::MFMediaType_Audio));set(*type,mf::MF_MT_SUBTYPE,valueguid(subtype(stream.format_tag)));
        for(const auto& [key,value]:std::array<std::pair<Guid,std::uint32_t>,8>{{{mf::MF_MT_AUDIO_NUM_CHANNELS,stream.channels},{mf::MF_MT_AUDIO_SAMPLES_PER_SECOND,stream.sample_rate},{mf::MF_MT_AUDIO_AVG_BYTES_PER_SECOND,stream.bytes_per_second},{mf::MF_MT_AUDIO_BLOCK_ALIGNMENT,stream.block_align},{mf::MF_MT_AUDIO_BITS_PER_SAMPLE,stream.bits},{mf::MF_MT_COMPRESSED,compressed},{mf::MF_MT_FIXED_SIZE_SAMPLES,!compressed},{mf::MF_MT_ALL_SAMPLES_INDEPENDENT,1}}})set(*type,key,value32(value));
    }
    // BITMAPINFOHEADER／WAVEFORMATEX 之後的編碼器私有資料原樣放入 MF_MT_USER_DATA（WVC1 含開頭的 ASF binding byte 與 sequence／entry-point header）。
    if(!stream.extra.empty()){Value user;user.type=0x1011;user.blob=stream.extra;set(*type,mf::MF_MT_USER_DATA,std::move(user));}
    return p;
};
const auto asf_source_create=[&](std::shared_ptr<FileSourceState> file,std::uint64_t external){
    const auto movie=file->asf;file->demux=std::make_shared<mf::AsfDemux>();for(const auto& stream:movie->streams)file->demux->selected.set(stream.number);
    const auto p=make(mf::Source),presentation=make(mf::Presentation);
    auto source=object(p,mf::Source);source->file_source=std::move(file);source->parent=external;hold(external);source->children={presentation};
    auto pd=object(presentation,mf::Presentation);
    for(const auto& stream:movie->streams){
        const auto type=asf_media_type(stream),descriptor=make(mf::Stream);auto sd=object(descriptor,mf::Stream);
        sd->identifier=stream.number;sd->children={type};sd->current_type=type;hold(type);pd->children.push_back(descriptor);pd->selected.push_back(true);
    }
    set(*pd,mf::MF_PD_DURATION,value64(static_cast<std::uint64_t>(movie->duration)));return p;
};
// 解多工的兩種操作共用同一條讀取路徑：宿主檔案同步完成；客體 byte stream 在單一服務續行內依序讀取所需封包。
// 解多工狀態只在協程內前進，API 重新進入時直接取回既有結果，不重做讀取。
enum class AsfOp {Next,Seek};
const auto asf_io=[&](const std::shared_ptr<FileSourceState>& file,AsfOp op,std::uint8_t stream,std::int64_t time)->std::optional<mf::AsfObject>{
    try{
        if(file->external_stream)return service_callback::invoke<std::optional<mf::AsfObject>>(this,threads_.current,state,memory,"mf-asf-demux",byte_stream_begin,
            [this,file,op,stream,time](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<mf::AsfObject>>{
                const auto scratch=allocate_heap(m,65568);if(!scratch)throw std::bad_alloc{};
                struct Scratch {WindowsServices& owner;GuestState& state;GuestMemory& memory;std::uint64_t pointer;~Scratch(){(void)owner.free_heap(memory,pointer,state);}} cleanup{*this,s,m,scratch};
                GuestByteStreamAccess access{m,file->external_stream,scratch,static_cast<std::uint8_t>(x86_?32:64)};
                const auto read=[&access,base=file->base](std::uint64_t at,std::span<std::uint8_t> bytes){return access.read(base+at,bytes);};
                if(op==AsfOp::Seek){(void)co_await mf::asf_seek_async(*file->asf,*file->demux,time,read);co_return std::nullopt;}
                co_return co_await mf::asf_next_object_async(*file->asf,*file->demux,stream,read);
            });
        const auto read=[&](std::uint64_t at,std::span<std::uint8_t> bytes){
            auto cursor=file->base+at;
            while(!bytes.empty()){const auto r=file->file->read(bytes,cursor);require(!r.error && r.bytes && r.bytes<=bytes.size(),0x80070000U|((r.error?r.error:38)&0xffffU));cursor+=r.bytes;bytes=bytes.subspan(r.bytes);}
            return mf::AsfReadReady{};
        };
        if(op==AsfOp::Seek){(void)mf::asf_run(mf::asf_seek_async(*file->asf,*file->demux,time,read));return std::nullopt;}
        return mf::asf_run(mf::asf_next_object_async(*file->asf,*file->demux,stream,read));
    }catch(const std::invalid_argument&){throw Failure{mf::asf_invalid_data};}
};
// 串流選取：未選取的串流不重組、不佔用佇列。讀取進行中不可變更。
const auto asf_select=[&](const std::shared_ptr<FileSourceState>& file,const std::vector<bool>& selected){
    require(!file->demux->busy,0xc00d36b2U);const auto& streams=file->asf->streams;
    for(std::size_t n=0;n<streams.size();++n)file->demux->select(streams[n].number,n<selected.size() && selected[n]);
};
// 取得下一個壓縮樣本。index 是 presentation 內的串流索引，UINT32_MAX 表示任一已選取串流（依檔案順序）；
// 回傳 0 表示該串流（或全部串流）已到結尾。actual 是樣本所屬的串流索引。
const auto asf_sample=[&](const std::shared_ptr<Object>& source,std::uint32_t index,std::uint32_t& actual){
    const auto& file=source->file_source;const auto& streams=file->asf->streams;
    const auto media=asf_io(file,AsfOp::Next,index==UINT32_MAX?std::uint8_t{0}:streams.at(index).number,0);
    if(!media){actual=index==UINT32_MAX?0:index;return UINT64_C(0);}
    const auto found=std::find_if(streams.begin(),streams.end(),[&](const mf::AsfStream& item){return item.number==media->stream;});
    actual=static_cast<std::uint32_t>(found-streams.begin());
    // 容器只在 Extended Stream Properties 提供視訊的平均每幀時間；沒有時不設定樣本長度。
    std::optional<std::uint64_t> duration;if(found->video && found->frame_time)duration=found->frame_time;
    const auto sample=bytes_sample(media->bytes,std::bit_cast<std::uint64_t>(media->time),duration);
    if(media->key)set(*object(sample,mf::Sample),mf::MFSampleExtension_CleanPoint,value32(1));
    return sample;
};
// Source Reader 的解碼讀取：讀取並解碼直到指定串流（或任一已選取串流）有輸出或結束。
// 客體 byte stream 的多次讀取與解碼都在同一個服務續行內完成；API 重新進入時以 pump_pending 取回同一工作的結果，不重做讀取。
const auto asf_pump=[&](const std::shared_ptr<FileSourceState>& file,const std::shared_ptr<AsfReaderDecode>& decode,std::uint32_t index,const std::vector<bool>& selected){
    const auto movie=file->asf;const auto number=index==UINT32_MAX?std::uint8_t{0}:movie->streams.at(index).number;
    // 一個已解多工的物件（nullopt 表示串流結束）送入對應的輸出佇列；回傳是否已滿足要求。
    const auto step=[movie,decode,index,selected](std::optional<mf::AsfObject> media){
        try{
            if(!media){
                if(index!=UINT32_MAX)decode->streams.at(index)->finish();
                else for(std::size_t n=0;n<decode->streams.size();++n)if(n<selected.size() && selected[n])decode->streams[n]->finish();
                return true;
            }
            const auto found=std::find_if(movie->streams.begin(),movie->streams.end(),[&](const mf::AsfStream& item){return item.number==media->stream;});
            if(found!=movie->streams.end())decode->streams.at(static_cast<std::size_t>(found-movie->streams.begin()))->consume(*media);
            return decode->satisfied(index,selected);
        }catch(const std::runtime_error&){throw Failure{0xc00d36cbU};} // MF_E_INVALID_STREAM_DATA：解碼器無法處理串流內容。
    };
    try{
        if(file->external_stream){
            (void)service_callback::invoke<bool>(this,threads_.current,state,memory,"mf-asf-decode",byte_stream_begin,
                [this,file,number,step](GuestState& s,GuestMemory& m)->service_callback::Task<bool>{
                    const auto scratch=allocate_heap(m,65568);if(!scratch)throw std::bad_alloc{};
                    struct Scratch {WindowsServices& owner;GuestState& state;GuestMemory& memory;std::uint64_t pointer;~Scratch(){(void)owner.free_heap(memory,pointer,state);}} cleanup{*this,s,m,scratch};
                    GuestByteStreamAccess access{m,file->external_stream,scratch,static_cast<std::uint8_t>(x86_?32:64)};
                    const auto read=[&access,base=file->base](std::uint64_t at,std::span<std::uint8_t> bytes){return access.read(base+at,bytes);};
                    while(!step(co_await mf::asf_next_object_async(*file->asf,*file->demux,number,read))){}
                    co_return true;
                });
            return;
        }
        const auto read=[&](std::uint64_t at,std::span<std::uint8_t> bytes){
            auto cursor=file->base+at;
            while(!bytes.empty()){const auto r=file->file->read(bytes,cursor);require(!r.error && r.bytes && r.bytes<=bytes.size(),0x80070000U|((r.error?r.error:38)&0xffffU));cursor+=r.bytes;bytes=bytes.subspan(r.bytes);}
            return mf::AsfReadReady{};
        };
        while(!step(mf::asf_run(mf::asf_next_object_async(*file->asf,*file->demux,number,read)))){}
    }catch(const std::invalid_argument&){throw Failure{mf::asf_invalid_data};}
};
// 取得 Reader 的下一個解碼（或原樣交付）樣本；回傳 0 表示串流結束，actual 是樣本所屬的串流索引。
const auto asf_decoded_sample=[&](const std::shared_ptr<Object>& reader,const std::shared_ptr<Object>& source,std::uint32_t index,std::uint32_t& actual){
    const auto decode=reader->asf_decode;
    if(decode->pump_pending || !decode->satisfied(index,reader->selected)){
        decode->pump_pending=true;
        try{asf_pump(source->file_source,decode,index,reader->selected);}
        catch(const service_callback::Suspended&){throw;}
        catch(...){decode->pump_pending=false;throw;}
        decode->pump_pending=false;
        // 讀取期間已被 Flush：此結果由呼叫端依 generation 捨棄，這裡清除殘留的解碼資料。
        if(decode->flush_requested){decode->flush_requested=false;for(auto& output:decode->streams)output->reset(false);actual=index==UINT32_MAX?0:index;return UINT64_C(0);}
    }
    auto sample=decode->take(index,reader->selected,actual);if(!sample)return UINT64_C(0);
    const auto p=bytes_sample(sample->bytes,std::bit_cast<std::uint64_t>(sample->time),sample->duration);auto item=object(p,mf::Sample);
    if(sample->key)set(*item,mf::MFSampleExtension_CleanPoint,value32(1));
    if(sample->discontinuity)set(*item,mf::MFSampleExtension_Discontinuity,value32(1));
    return p;
};

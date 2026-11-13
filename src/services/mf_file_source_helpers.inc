// Resolver 與 Reader 共用已授權檔案、PCM 格式、媒體物件及回呼排程。
const auto open_media_stream=[&](std::uint64_t url,std::uint32_t flags){
    require(url!=0,pointer_error);auto path=read_utf16(memory,GuestAddress{url});require(!path.empty());
    if(path.starts_with(u"file://")){
        path.erase(0,7);if(path.starts_with(u"localhost/"))path.erase(0,9);
        if(path.size()>=3 && path[0]==u'/' && path[2]==u':')path.erase(0,1);
        const auto encoded=utf16_to_utf8(path);std::string decoded;
        for(std::size_t i=0;i<encoded.size();++i){
            if(encoded[i]!='%'){decoded+=encoded[i];continue;}require(i+2<encoded.size());
            const auto digit=[](char c)->int{return c>='0' && c<='9'?c-'0':c>='a' && c<='f'?c-'a'+10:c>='A' && c<='F'?c-'A'+10:-1;};
            const int a=digit(encoded[i+1]),b=digit(encoded[i+2]);require(a>=0 && b>=0 && (a || b));decoded+=static_cast<char>(a*16+b);i+=2;
        }path=utf8_to_utf16(decoded);
        require(path.size()>=3 && path[1]==u':' && (path[2]==u'/' || path[2]==u'\\'),0xc00d36c3U);
    }else require(path.find(u"://")==std::u16string::npos,0xc00d36c3U);
    const auto access=(flags&0x20000U?0x40000000U:0U)|(flags&0x20000U && !(flags&0x10000U)?0U:0x80000000U);
    const auto opened=filesystem_.open_host(utf16_to_utf8(path),access,3,0,1);
    require(!opened.error && bool(opened.file),sink_io_error(opened.error?opened.error:2));
    auto stream=std::make_shared<ByteStreamState>();stream->file=opened.file;stream->capabilities=4U|0x800U|(access&0x80000000U?1U:0U)|(access&0x40000000U?2U:0U);return stream;
};
const auto file_source_create=[&](const std::shared_ptr<ByteStreamState>& stream,std::uint64_t external=0){
    std::shared_ptr<FileSourceState> file;
    if(external)file=external_stream_metadata(external);
    else {
        require(!stream->closed,shutdown);require((stream->capabilities&5)==5,0xc00d36b2U);
        file=std::make_shared<FileSourceState>();file->file=stream->file;file->base=stream->position;
        const auto size=file->file->current_size();require(file->base<=size,0xc00d36c4U);
        const auto read=[&](std::uint64_t at,std::span<std::uint8_t> bytes){
            while(!bytes.empty()){const auto r=file->file->read(bytes,file->base+at);require(!r.error && r.bytes && r.bytes<=bytes.size(),sink_io_error(r.error?r.error:38));at+=r.bytes;bytes=bytes.subspan(r.bytes);}
        };
        // 開頭 16 bytes 足以辨認 ASF Header Object GUID；MP4 與 RIFF 只看前 8 bytes。
        std::array<std::uint8_t,16> head{};if(const auto available=size-file->base;available>=8)read(0,std::span<std::uint8_t>(head).first(available>=16?16:8));
        if(mf::asf_signature(head)){try{file->asf=mf::read_asf_movie(size-file->base,read);}catch(const std::invalid_argument&){throw Failure{mf::asf_invalid_data};}}
        else if(mf::mp4_signature(std::span<const std::uint8_t,16>(head).first<8>())){try{file->movie=mf::read_mp4_movie(size-file->base,read);}catch(const std::invalid_argument&){throw Failure{0xc00d36c4U};}}
        else {const auto parsed=parse_pcm_wave_stream(size-file->base,read);require(parsed.has_value(),0xc00d36c4U);file->wave=*parsed;}
    }
    if(file->movie){try{return movie_source_create(std::move(file),external);}catch(const std::invalid_argument&){throw Failure{0xc00d36c4U};}}
    if(file->asf)return asf_source_create(std::move(file),external);
    const auto p=make(mf::Source),type=make(mf::MediaType),descriptor=make(mf::Stream),presentation=make(mf::Presentation);
    auto source=object(p,mf::Source);source->file_source=std::move(file);source->parent=external;hold(external);source->children={presentation};const auto& format=source->file_source->wave.format;
    auto native=object(type,mf::MediaType);set(*native,mf::MF_MT_MAJOR_TYPE,valueguid(mf::MFMediaType_Audio));set(*native,mf::MF_MT_SUBTYPE,valueguid(format.floating?mf::ieee:mf::pcm));
    for(const auto& [key,value]:std::array<std::pair<Guid,std::uint32_t>,9>{{{mf::MF_MT_AUDIO_NUM_CHANNELS,format.channels},{mf::MF_MT_AUDIO_SAMPLES_PER_SECOND,format.rate},{mf::MF_MT_AUDIO_BLOCK_ALIGNMENT,format.align},{mf::MF_MT_AUDIO_BITS_PER_SAMPLE,format.bits},{mf::MF_MT_AUDIO_AVG_BYTES_PER_SECOND,format.rate*format.align},{mf::MF_MT_AUDIO_CHANNEL_MASK,format.channel_mask?format.channel_mask:format.channels==1?4U:format.channels==2?3U:0U},{mf::MF_MT_COMPRESSED,0},{mf::MF_MT_FIXED_SIZE_SAMPLES,1},{mf::MF_MT_SAMPLE_SIZE,format.align}}})set(*native,key,value32(value));
    auto sd=object(descriptor,mf::Stream);sd->children={type};sd->current_type=type;hold(type);
    auto pd=object(presentation,mf::Presentation);pd->children={descriptor};pd->selected={true};set(*pd,mf::MF_PD_DURATION,value64(source->file_source->wave.bytes/format.align*10000000U/format.rate));return p;
};
const auto source_from_byte_stream=[&](std::uint64_t pointer){
    if(!bridge.objects.contains(pointer))return file_source_create({},pointer);
    const auto own=object(pointer,mf::ByteStream);
    // 包裝客體 IStream 的 ByteStream 沒有宿主檔案；沿外部 ByteStream 路徑，經本物件的方法回呼客體讀取。
    if(own->byte_stream && own->byte_stream->guest_stream)return file_source_create({},pointer);
    return file_source_create(own->byte_stream);
};
const auto reader_create=[&](std::uint64_t source_pointer,std::uint64_t options){
    auto source=object(source_pointer,mf::Source);require(!source->closed,shutdown);
    // Reader 讀取單串流 PCM 與多串流 ASF（原生壓縮樣本）；MP4 由 Media Session 自行解多工與解碼。
    require(!source->file_source || !source->file_source->movie,0xc00d36c4U);
    // 先驗證選項，再建立持有來源的物件，錯誤不洩漏來源引用。
    std::shared_ptr<Object> attributes;if(options){attributes=entry(options).object;const auto cb=attributes->attributes.find(mf::MF_SOURCE_READER_ASYNC_CALLBACK);if(cb!=attributes->attributes.end())require(cb->second.type==13,type_error);}
    const auto p=make(mf::Reader);auto reader=object(p,mf::Reader);reader->parent=source_pointer;hold(source_pointer);
    if(source->file_source && source->file_source->asf){
        // 多串流來源：children 依串流索引保存各串流的目前輸出型別（初始為原生型別），選取狀態沿用來源的預設。
        const auto presentation=object(source->children.at(0),mf::Presentation);reader->selected=presentation->selected;asf_select(source->file_source,reader->selected);
        for(const auto descriptor:presentation->children){const auto type=object(descriptor,mf::Stream)->children.at(0);hold(type);reader->children.push_back(type);}
    }else {reader->selected={true};reader->current_type=source_type(source);hold(reader->current_type);}
    if(attributes){for(const auto& [key,value]:attributes->attributes)set(*reader,key,value);if(const auto cb=attributes->attributes.find(mf::MF_SOURCE_READER_ASYNC_CALLBACK);cb!=attributes->attributes.end()){reader->callback=cb->second.scalar;hold(reader->callback);}if(const auto disconnect=attributes->attributes.find(mf::MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN);disconnect!=attributes->attributes.end())reader->disconnect_source=disconnect->second.scalar!=0;}
    return p;
};

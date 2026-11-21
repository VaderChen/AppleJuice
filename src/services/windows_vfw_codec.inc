    if(service.family=="msvfw32.dll" || service.family=="msrle32.dll" || service.family=="msvidc32.dll"){
        if(name=="VideoForWindowsVersion" || name=="#2")return done(vfw::version);
        return nested_continuation(state,memory,"Video for Windows 編解碼器",
            [this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall args(s,m,service);
                const auto name=service.symbol;
                auto& store=*avi_disk_;
                const unsigned unit=x86_?4U:8U;
                const auto ptr_bits=static_cast<std::uint8_t>(unit*8);
                const auto word_arg=[&](unsigned n){return static_cast<std::uint32_t>(args(n));};
                const auto finish=[&](std::uint64_t value){args.return_to(s,value);return std::optional<std::uint32_t>{};};
                const auto error=[&](int value){return finish(vfw::deliver_error(value));};
                const bool driver_is_video1=service.family=="msvidc32.dll";
                const bool driver_entry=driver_is_video1 || service.family=="msrle32.dll";
                const bool returns_handle=(driver_entry && word_arg(2)==3) || name=="ICOpen" || name=="ICOpenFunction" || name=="ICLocate" ||
                    name=="ICGetDisplayFormat" || name=="ICImageDecompress" || name=="ICImageCompress" ||
                    name=="ICSeqCompressFrame";
                const bool drawdib=name.starts_with("DrawDib") || name=="StretchDIB";
                const bool returns_bool=drawdib || name=="ICInfo" || name=="ICInstall" || name=="ICRemove" ||
                    name=="ICSeqCompressFrameStart" || name=="ICSeqCompressFrameEnd" || name=="ICCompressorFree" ||
                    name=="ICCompressorChoose";
                using service_callback::Task;
                using vfw::Backend;
                constexpr unsigned decompress_format=0x400a,decompress_query=0x400b,decompress_begin=0x400c,
                    decompress_frame=0x400d,decompress_end=0x400e,get_info=0x5002;
                GuestScratch scratch(*this,m,s);
                const auto guest_bytes=[&](avi::View bytes)->std::uint64_t {
                    const auto address=scratch.allocate(bytes.size());
                    if(!address)throw std::bad_alloc{};
                    m.write(GuestAddress{address},bytes);
                    return address;
                };
                const auto guest_text=[&](std::u16string_view text){return guest_bytes(utf16_bytes(text));};
                const auto api=[&](std::string_view module,std::string_view function,std::vector<std::uint64_t> params)
                    ->Task<std::uint64_t>{
                    co_return co_await service_callback::call_value(resolve(module,function).value,std::move(params));
                };
                const auto release_draw=[&](vfw::DrawChannel& draw)->Task<bool>{
                    const auto previous=std::exchange(draw,vfw::DrawChannel{});
                    if(previous.surface)(void)co_await api("msvfw32.dll","DrawDibClose",{previous.surface});
                    if(previous.owner_window)(void)co_await api("user32.dll","ReleaseDC",{previous.owner_window,previous.dc});
                    co_return previous.surface!=0;
                };
                const auto fold_key=[](std::uint32_t type,std::uint32_t handler){
                    return std::pair{vfw::folded(type),vfw::folded(handler)};
                };
                const auto catalog=[&]{
                    std::vector<vfw::Driver> drivers;
                    {
                        RegistryTransaction registry(runtime_.registry,registry_file_,&registry_store_cache_,true);
                        const auto key=std::u16string(1,2)+u"\\"+std::u16string(vfw::drivers32);
                        if(!registry.error){
                            const auto view=registry_view_path(key,registry_view(0,x86_));
                            if(const auto found=runtime_.registry.keys.find(view);found!=runtime_.registry.keys.end()){
                                for(const auto& [label,value]:found->second){
                                    if(label.size()!=9 || label[4]!=u'.' || (value.type!=1 && value.type!=2) ||
                                       value.bytes.empty() || value.bytes.size()%2)continue;
                                    const auto type=vfw::read_fourcc(std::u16string_view(label).substr(0,4));
                                    const auto handler=vfw::read_fourcc(std::u16string_view(label).substr(5));
                                    if(!type || !handler)continue;
                                    std::u16string file;
                                    for(std::size_t at=0;at<value.bytes.size();at+=2){
                                        const auto ch=static_cast<char16_t>(value.bytes[at]|(value.bytes[at+1]<<8));
                                        if(!ch)break;
                                        file.push_back(ch);
                                    }
                                    if(!file.empty())drivers.push_back({type,handler,Backend::FileDriver,{}, {},std::move(file)});
                                }
                            }
                        }
                    }
                    for(const auto& [key,driver]:store.codec_overrides){
                        std::erase_if(drivers,[&](const vfw::Driver& item){return fold_key(item.type,item.handler)==key;});
                        drivers.push_back(driver);
                    }
                    for(auto& known:vfw::builtins()){
                        const auto key=fold_key(known.type,known.handler);
                        if(std::none_of(drivers.begin(),drivers.end(),[&](const auto& driver){
                            return fold_key(driver.type,driver.handler)==key;
                        }))drivers.push_back(std::move(known));
                    }
                    return drivers;
                };
                const auto header_at=[&](std::uint64_t guest)->avi::Bytes{
                    if(!guest)return {};
                    avi::Bytes header(40);
                    m.read(GuestAddress{guest},header);
                    const auto size=avi::frame_header_size(header);
                    if(!size)return {};
                    header.resize(size);
                    if(size>40)m.read(GuestAddress{guest+40},std::span(header).subspan(40));
                    return header;
                };
                const auto proc=[&](const vfw::Codec& codec,std::uint32_t message,std::uint64_t first,std::uint64_t second)
                    ->Task<std::uint64_t>{
                    if(codec.driver.backend==Backend::Callback)
                        co_return co_await service_callback::call_value(codec.driver.procedure,{codec.instance,0,message,first,second});
                    co_return co_await api("winmm.dll","SendDriverMessage",{codec.driver.driver,message,first,second});
                };
                const auto allocation=[&](const vfw::Driver& driver,std::uint32_t mode)->Task<std::uint64_t>{
                    auto codec=std::make_shared<vfw::Codec>();
                    codec->driver=driver;
                    if(driver.backend==Backend::RunLength)codec->quality=vfw::rle_default_quality;
                    const auto handle=runtime_.allocate_wait_handle();
                    const bool callback=driver.backend==Backend::Callback;
                    codec->busy=true;
                    store.codecs.emplace(handle,codec);
                    struct PendingOpen {
                        decltype(store.codecs)& codecs;
                        std::uint64_t handle;
                        bool published{};
                        ~PendingOpen(){if(!published)codecs.erase(handle);}
                    } pending{store.codecs,handle};
                    if(callback || driver.backend==Backend::FileDriver){
                        const unsigned size=x86_?36U:56U;
                        avi::Bytes open(size);
                        avi::put(open,0,size);
                        avi::put(open,4,driver.type);
                        avi::put(open,8,driver.handler);
                        avi::put(open,12,vfw::interface_version);
                        avi::put(open,16,mode);
                        const auto block=guest_bytes(open);
                        if(callback){
                            if(co_await proc(*codec,1,0,0)!=1)co_return 0;
                            (void)co_await proc(*codec,2,0,0);
                            codec->instance=co_await proc(*codec,3,0,block);
                            if(!codec->instance){
                                (void)co_await proc(*codec,5,0,0);
                                (void)co_await proc(*codec,6,0,0);
                                co_return 0;
                            }
                        }else{
                            codec->driver.driver=co_await api("winmm.dll","OpenDriver",{guest_text(
                                vfw::driver_name(driver.type,driver.handler)),0,block});
                            if(!codec->driver.driver)co_return 0;
                            codec->owns_driver=true;
                        }
                    }
                    codec->busy=false;
                    pending.published=true;
                    co_return handle;
                };
                const auto close=[&](std::uint64_t handle)->Task<std::uint64_t>{
                    const auto found=store.codecs.find(handle);
                    if(found==store.codecs.end() || found->second->busy)co_return vfw::deliver_error(vfw::bad_handle);
                    const auto codec=found->second;
                    store.codecs.erase(found);
                    if(codec->drawing.surface)(void)co_await release_draw(codec->drawing);
                    if(codec->driver.backend==Backend::Callback){
                        (void)co_await proc(*codec,4,0,0);
                        codec->instance=0;
                        (void)co_await proc(*codec,5,0,0);
                        (void)co_await proc(*codec,6,0,0);
                    }else if(codec->owns_driver){
                        (void)co_await api("winmm.dll","CloseDriver",{codec->driver.driver,0,0});
                    }
                    co_return 0;
                };
                const auto send=[&](std::uint64_t handle,std::uint32_t message,std::uint64_t first,std::uint64_t second)
                    ->Task<std::uint64_t>{
                    const auto found=store.codecs.find(handle);
                    if(found==store.codecs.end() || found->second->busy)co_return vfw::deliver_error(vfw::bad_handle);
                    const auto codec=found->second;
                    const auto backend=codec->driver.backend;
                    if(backend==Backend::Callback || backend==Backend::FileDriver || backend==Backend::InstallableDriver){
                        codec->busy=true;
                        struct Clear {vfw::Codec& codec;~Clear(){codec.busy=false;}} clear{*codec};
                        co_return co_await proc(*codec,message,first,second);
                    }
                    const bool rle=backend==Backend::RunLength;
                    const bool video1=backend==Backend::Video1;
                    const auto valid_depth=[&](unsigned depth){return !rle || vfw::rle_depth(codec->driver.handler,depth);};
                    const auto begin_video1=[&](avi::View source,const avi::FrameLayout& from){
                        try{
                            auto decoder=avi::open_video1(source,from);
                            codec->video1_source=avi::uncompressed_frame_header(source);
                            codec->video1_decoder=std::move(decoder);
                            return true;
                        }catch(const std::runtime_error&){return false;}
                    };
                    const auto decode_video1=[&](avi::View source,avi::View encoded,bool key){
                        if(!codec->video1_decoder || codec->video1_source!=avi::uncompressed_frame_header(source))
                            return avi::Bytes{};
                        try{return codec->video1_decoder->decode(encoded,key);}
                        catch(const std::runtime_error&){return avi::Bytes{};}
                    };
#include "windows_vfw_renderer.inc"
#include "windows_vfw_compression_messages.inc"
#include "windows_vfw_video1.inc"
                    if(message==get_info){
                        if(codec->standalone){
                            if(!first)co_return vfw::icinfo_size;
                            if(second<vfw::icinfo_size)co_return 0;
                        }
                        if(!first || second<24)co_return 0;
                        const auto data=vfw::pack_info(codec->driver);
                        const auto copied=std::min<std::uint64_t>(second,data.size());
                        m.write(GuestAddress{first},std::span(data).first(copied));
                        co_return copied;
                    }
                    if(message==decompress_end || (video1 && message==vfw::decompress_ex_end)){
                        codec->decompressing=false;
                        codec->rle_decoded_format.clear();
                        codec->rle_output.clear();
                        codec->video1_source.clear();
                        codec->video1_decoder.reset();
                        co_return 0;
                    }
                    if((rle || video1) && (message==vfw::get_state || message==vfw::set_state))co_return 0;
                    if((rle || video1) && message==vfw::codec_about){
                        if(first!=(x86_?UINT32_MAX:UINT64_MAX)){
                            const auto title=guest_text(codec->driver.description);
                            const auto text=guest_text(video1?u"Video 1 的 8／16 位元解碼與 RGB555 編碼":
                                                             u"RLE 4／8 位元點陣圖編解碼器");
                            (void)co_await api("user32.dll","MessageBoxW",{first,text,title,0x40});
                        }
                        co_return 0;
                    }
                    if((rle || video1) && message==vfw::decompress_palette){
                        if(!first || !second)co_return vfw::deliver_error(vfw::bad_parameter);
                        const auto source=header_at(first),destination=header_at(second);
                        const auto from=avi::frame_layout(source),to=avi::frame_layout(destination);
                        if(!from || !to || !vfw::accepts(backend,*from) || !valid_depth(from->depth) ||
                           from->depth>8 || !to->raw() || to->depth!=from->depth)
                            co_return vfw::deliver_error(vfw::bad_format);
                        const auto bytes=avi::View(source).subspan(avi::word(source,0));
                        const auto address=second+avi::word(destination,0);
                        m.check(GuestAddress{second+32},4,Access::Write);
                        m.check(GuestAddress{address},bytes.size(),Access::Write);
                        m.write(GuestAddress{address},bytes);
                        m.store(GuestAddress{second+32},32,bytes.size()/4);
                        co_return 0;
                    }
                    if(message==decompress_format || message==decompress_query || message==decompress_begin){
                        const auto source=header_at(first);
                        const auto from=avi::frame_layout(source);
                        if(!from || !vfw::accepts(backend,*from) || !valid_depth(from->depth))
                            co_return vfw::deliver_error(vfw::bad_format);
                        if(message==decompress_format){
                            auto output=avi::frame_output(source,*from,{});
                            if((rle || video1) && codec->standalone){
                                output=avi::uncompressed_frame_header(source);
                                const auto decoded=avi::frame_layout(output);
                                if(!decoded || decoded->top_down)co_return vfw::deliver_error(vfw::bad_format);
                                avi::put(output,20,decoded->image_size());
                            }
                            if(output.empty())co_return vfw::deliver_error(vfw::bad_format);
                            if(!second)co_return output.size();
                            m.write(GuestAddress{second},output);
                            co_return 0;
                        }
                        if(second){
                            const auto destination=avi::frame_layout(header_at(second));
                            if(!destination || !destination->raw())co_return vfw::deliver_error(vfw::bad_format);
                        }else if(message==decompress_begin)co_return vfw::deliver_error(vfw::bad_format);
                        if(message==decompress_begin){
                            if(video1 && !begin_video1(source,*from))co_return vfw::deliver_error(vfw::bad_format);
                            codec->decompressing=true;
                            codec->rle_decoded_format.clear();
                            codec->rle_output.clear();
                        }
                        co_return 0;
                    }
                    if(message!=decompress_frame)co_return vfw::deliver_error(vfw::unsupported);
                    if(!first || second<(x86_?24U:48U) || !codec->decompressing)
                        co_return vfw::deliver_error(vfw::bad_parameter);
                    const auto flags=static_cast<std::uint32_t>(m.load(GuestAddress{first},32));
                    if(flags&0x10000000U)co_return 0;
                    const auto slot=[&](unsigned n){return m.load(GuestAddress{first+unit*(1+n)},ptr_bits);};
                    const auto input_header=header_at(slot(0));
                    const auto input_layout=avi::frame_layout(input_header);
                    const auto output_layout=avi::frame_layout(header_at(slot(2)));
                    if(!input_layout || !output_layout || !output_layout->raw() || !vfw::accepts(backend,*input_layout) ||
                       !valid_depth(input_layout->depth))
                        co_return vfw::deliver_error(vfw::bad_format);
                    const auto input_size=input_layout->raw()?input_layout->image_size():avi::word(input_header,20);
                    if(!slot(1) || !slot(3) || !input_size || input_size>memory_limit)
                        co_return vfw::deliver_error(vfw::bad_size);
                    avi::Bytes packet(input_size),output(output_layout->image_size());
                    m.read(GuestAddress{slot(1)},packet);
                    if(video1){
                        const auto dib=avi::frame_layout(codec->video1_source);
                        if(!dib)co_return vfw::deliver_error(vfw::bad_format);
                        m.check(GuestAddress{slot(3)},output.size(),Access::Write);
                        const auto decoded=decode_video1(input_header,packet,!(flags&vfw::renderer_non_key));
                        if(!avi::frame_pixels(codec->video1_source,*dib,decoded,*output_layout,output,0,0,
                                              output_layout->width,output_layout->height))
                            co_return vfw::deliver_error(vfw::bad_format);
                        m.write(GuestAddress{slot(3)},output);
                        co_return 0;
                    }
                    if(rle){
                        auto format=avi::uncompressed_frame_header(input_header);
                        const auto dib=avi::frame_layout(format);
                        if(!dib || dib->top_down)co_return vfw::deliver_error(vfw::bad_format);
                        const bool delta=(flags&vfw::renderer_non_key) && format==codec->rle_decoded_format;
                        auto decoded=delta?codec->rle_output:avi::Bytes(dib->image_size());
                        if(!avi::decode_run_length(packet,*dib,decoded) ||
                           !avi::frame_pixels(format,*dib,decoded,*output_layout,output,0,0,
                                              output_layout->width,output_layout->height))
                            co_return vfw::deliver_error(vfw::bad_format);
                        m.write(GuestAddress{slot(3)},output);
                        codec->rle_decoded_format=std::move(format);
                        codec->rle_output=std::move(decoded);
                        co_return 0;
                    }
                    if(!avi::frame_pixels(input_header,*input_layout,packet,*output_layout,output,0,0,
                                          output_layout->width,output_layout->height))
                        co_return vfw::deliver_error(vfw::bad_format);
                    m.write(GuestAddress{slot(3)},output);
                    co_return 0;
                };
                const auto open=[&](std::uint32_t type,std::uint32_t handler,std::uint32_t mode,std::uint64_t input,
                                    std::uint64_t output)->Task<std::uint64_t>{
                    if(!type || !vfw::codec_mode(mode))co_return 0;
                    const auto drivers=catalog();
                    for(unsigned pass=0;pass<(input && handler?2U:1U);++pass){
                        for(const auto& driver:drivers){
                            const bool preferred=vfw::driver_matches_handler(driver,handler);
                            if(!vfw::matches(driver.type,type) || (handler && preferred!=(pass==0)))continue;
                            const auto handle=co_await allocation(driver,mode);
                            if(!handle)continue;
                            if(!input)co_return handle;
                            const auto message=mode==1 || mode==5?vfw::compress_query:
                                mode==8?vfw::draw_query_message:decompress_query;
                            if(!co_await send(handle,message,input,output))co_return handle;
                            (void)co_await close(handle);
                        }
                    }
                    co_return 0;
                };
                const auto decompress=[&](std::uint64_t handle,std::uint32_t flags,std::uint64_t input_format,
                    std::uint64_t input,std::uint64_t output_format,std::uint64_t output)->Task<std::uint64_t>{
                    avi::Bytes package(x86_?24U:48U);
                    avi::put(package,0,flags);
                    const std::array pointers{input_format,input,output_format,output};
                    for(unsigned n=0;n<pointers.size();++n)
                        for(unsigned i=0;i<unit;++i)package[unit*(1+n)+i]=static_cast<std::uint8_t>(pointers[n]>>(8*i));
                    co_return co_await send(handle,decompress_frame,guest_bytes(package),package.size());
                };
                try {
                    if(driver_entry){
                        const auto id=args(0),first=args(3),second=args(4);
                        const auto message=word_arg(2);
                        if(message==3){
                            constexpr std::uint64_t probe=0xffff0000;
                            if(!second && !driver_is_video1)co_return finish(probe);
                            const auto field=[&](unsigned offset){return static_cast<std::uint32_t>(m.load(GuestAddress{second+offset},32));};
                            auto driver=driver_is_video1?vfw::video1_driver():vfw::rle_driver();
                            if(second){
                                if(field(0)<(x86_?36U:56U) || !vfw::matches(field(4),vfw::video_type) ||
                                   (!driver_is_video1 && !vfw::rle_handler(field(8))))co_return finish(0);
                                if(!driver_is_video1 && !vfw::matches(field(8),vfw::mrle))driver.handler=field(8);
                                m.check(GuestAddress{second+(x86_?20U:24U)},unit,Access::Write);
                            }
                            const auto handle=co_await allocation(driver,second?field(16):0);
                            if(!handle)co_return finish(0);
                            store.codecs.at(handle)->standalone=true;
                            if(second)m.store(GuestAddress{second+(x86_?20U:24U)},ptr_bits,0);
                            co_return finish(handle);
                        }
                        const auto found=store.codecs.find(id);
                        const bool valid=found!=store.codecs.end() && found->second->standalone &&
                            found->second->driver.backend==(driver_is_video1?Backend::Video1:Backend::RunLength);
                        if(message==4){
                            if(valid)co_return finish(!(co_await close(id)));
                            co_return finish(!driver_is_video1 && id==0xffff0000);
                        }
                        if(message<0x4000)co_return finish(co_await api("winmm.dll","DefDriverProc",
                                                                        {id,args(1),message,first,second}));
                        if(!valid)co_return error(vfw::bad_handle);
                        co_return finish(co_await send(id,message,first,second));
                    }
#include "windows_vfw_drawdib.inc"
#include "windows_vfw_choose.inc"
#include "windows_vfw_compression_wrappers.inc"
                    if(name=="ICDrawBegin" || name=="ICDraw"){
                        const bool begin=name=="ICDrawBegin";
                        avi::Bytes data(begin?vfw::draw_begin_size(unit):vfw::draw_frame_size(unit));
                        avi::put(data,0,word_arg(1));
                        if(begin){
                            for(unsigned i=0;i<3;++i)vfw::put_pointer(data,(i+1)*unit,unit,args(2+i));
                            for(unsigned i=0;i<4;++i)avi::put(data,4*unit+i*4,word_arg(5+i));
                            vfw::put_pointer(data,4*unit+16,unit,args(9));
                            for(unsigned i=0;i<6;++i)avi::put(data,5*unit+16+i*4,word_arg(10+i));
                        }else{
                            vfw::put_pointer(data,unit,unit,args(2));
                            vfw::put_pointer(data,2*unit,unit,args(3));
                            avi::put(data,3*unit,word_arg(4));
                            avi::put(data,3*unit+4,word_arg(5));
                        }
                        const auto result=co_await send(args(0),begin?vfw::draw_begin_message:vfw::draw_frame_message,
                                                        guest_bytes(data),data.size());
                        co_return finish(static_cast<std::uint32_t>(result));
                    }
                    if(name=="ICOpenFunction"){
                        if(!word_arg(0) || !vfw::codec_mode(word_arg(2)) || !args(3))co_return finish(0);
                        const vfw::Driver driver{word_arg(0),word_arg(1),Backend::Callback,{}, {},{},args(3)};
                        co_return finish(co_await allocation(driver,word_arg(2)));
                    }
                    if(name=="ICOpen")co_return finish(co_await open(word_arg(0),word_arg(1),word_arg(2),0,0));
                    if(name=="ICLocate")co_return finish(co_await open(word_arg(0),word_arg(1),word_arg(4),args(2),args(3)));
                    if(name=="ICClose")co_return finish(co_await close(args(0)));
                    if(name=="ICSendMessage")co_return finish(co_await send(args(0),word_arg(1),args(2),args(3)));
                    if(name=="ICGetInfo"){
                        const auto found=store.codecs.find(args(0));
                        if(found==store.codecs.end() || found->second->busy)co_return error(vfw::bad_handle);
                        const auto output=args(1);
                        const auto capacity=word_arg(2);
                        if(!output)co_return finish(11); // MMSYSERR_INVALPARAM
                        const auto source=found->second;
                        if(capacity>=vfw::icinfo_size)m.store(GuestAddress{output+312},16,0);
                        const auto reply=co_await send(args(0),get_info,output,capacity);
                        if(capacity>=vfw::icinfo_size && reply && reply<=vfw::icinfo_size &&
                           !m.load(GuestAddress{output+312},16) && !source->driver.file.empty()){
                            const auto file=utf16_bytes(source->driver.file.substr(0,127));
                            m.write(GuestAddress{output+312},file);
                        }
                        co_return finish(reply);
                    }
                    if(name=="ICInfo"){
                        const auto type=word_arg(0),handler=word_arg(1);
                        std::uint32_t index=0;
                        if(!args(2))co_return finish(0);
                        for(const auto& driver:catalog()){
                            if(type && !vfw::matches(type,driver.type))continue;
                            if(vfw::driver_matches_handler(driver,handler) || index==handler){
                                m.write(GuestAddress{args(2)},vfw::pack_info(driver));
                                co_return finish(1);
                            }
                            ++index;
                        }
                        co_return finish(0);
                    }
                    if(name=="ICDecompress"){
                        co_return finish(co_await decompress(args(0),word_arg(1),args(2),args(3),args(4),args(5)));
                    }
                    // 編解碼器註冊與高階 DIB 介面沿用同一份 handle 與訊息路徑。
#include "windows_vfw_codec_wrappers.inc"
                }catch(const MemoryFault&){co_return returns_handle || returns_bool?finish(0):error(vfw::bad_parameter);}
                catch(const std::bad_alloc&){co_return returns_handle || returns_bool?finish(0):error(vfw::no_memory);}
                co_return error(vfw::unsupported);
            });
    }

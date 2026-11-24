// msacm32 的驅動程式、格式與濾波器目錄（由 windows_acm.cpp 的 invoke_acm 引入）。
// 內建 PCM 轉換、音量濾波、MS／IMA ADPCM、MPEG、G.711 與 GSM 6.10。目錄與串流共用格式及濾波器支援。
// HACMDRIVERID 為固定值，acmDriverOpen 另發 HACMDRIVER；acmDriverPriority 調整列舉順序與停用狀態（本程序內）。
// 結構依 msacm.h 以 1 位元組封裝；列舉以 start_callbacks 逐項呼叫，每項傳入自己的詳細資料複本，回呼傳回 FALSE 時停止。
if(name.starts_with("acmDriver") || name.starts_with("acmFormatTag") || name.starts_with("acmFormatDetails") ||
   name.starts_with("acmFormatEnum") || name.starts_with("acmFormatChoose") || name.starts_with("acmFilter") || name=="acmMetrics"){
    if(!acm_)acm_=std::make_shared<AcmState>();auto& acm=*acm_;
    const bool wide_out=name.ends_with('W');const unsigned pointer=x86_?4U:8U,cu=wide_out?2U:1U;
    struct DriverInfo {std::u16string_view short_name,long_name,features;std::uint32_t support;std::array<unsigned,2> tags;unsigned tag_count;};
    static constexpr std::array<DriverInfo,x64arm::acm::builtin_codec_modules.size()> catalog{{
        {u"PCM",u"PCM Converter",u"Converts PCM and IEEE float audio and adjusts volume.",0x2|x64arm::acm::filter_support,{1,3},2},
        {u"ADPCM",u"MS ADPCM Codec",u"Converts between PCM and Microsoft ADPCM.",0x1,{2,0},1},
        {u"MPEG Layer-3",u"MPEG Audio Decoder",u"Decodes MPEG audio layers I, II and III to PCM.",0x1,
             {x64arm::acm::mp3,x64arm::acm::mpeg},2},
        {u"G.711",u"G.711 A-law and mu-law Codec",u"Converts PCM, A-law, and mu-law audio.",0x1,
             {x64arm::acm::a_law,x64arm::acm::mu_law},2},
        {u"IMA ADPCM",u"IMA ADPCM Codec",u"Converts between PCM and IMA ADPCM WAVE blocks.",0x1,
             {audio::ima_adpcm_tag,0},1},
        {u"GSM 6.10",u"GSM 6.10 WAV49 Codec",u"Converts mono PCM and GSM WAV49 audio.",0x1,
             {x64arm::acm::gsm_tag,0},1}}};
    constexpr auto driver_base=x64arm::acm::builtin_driver_base;
    const auto builtin_index=[&](std::uint64_t token)->std::optional<unsigned>{
        if(token<driver_base || token>=driver_base+catalog.size())return {};
        const auto index=static_cast<unsigned>(token-driver_base);
        return acm.installed[index]?std::optional{index}:std::nullopt;
    };
    const auto support_of=[&](unsigned index){return catalog[index].support|(acm.enabled[index]?0U:0x80000000U);};
    // HACMDRIVERID、HACMDRIVER 或 HACMSTREAM（依來源格式）對應的驅動程式。
    const auto driver_of=[&](std::uint64_t handle)->std::optional<unsigned>{
        if(const auto index=builtin_index(handle))return index;
        if(const auto found=acm.drivers.find(handle);found!=acm.drivers.end())return found->second;
        if(const auto found=acm.streams.find(handle);found!=acm.streams.end() && !found->second->codec_instance)
            return builtin_codec_for(found->second->source.tag,found->second->destination.tag);
        return std::nullopt;};
    const auto format_size=[](unsigned tag)->unsigned{
        switch(tag){
        case 1:return 16;
        case 2:return 50;
        case audio::ima_adpcm_tag:return 20;
        case x64arm::acm::gsm_tag:return 20;
        case x64arm::acm::mp3:return 30;
        case x64arm::acm::mpeg:return 40;
        default:return 18;
        }
    };
    const auto tag_name=[](unsigned tag)->std::u16string_view{
        switch(tag){
        case 1:return u"PCM";
        case 2:return u"Microsoft ADPCM";
        case 3:return u"IEEE Float";
        case audio::ima_adpcm_tag:return u"IMA ADPCM";
        case x64arm::acm::gsm_tag:return u"GSM 6.10";
        case x64arm::acm::a_law:return u"G.711 A-law";
        case x64arm::acm::mu_law:return u"G.711 mu-law";
        case x64arm::acm::mpeg:return u"MPEG Audio";
        default:return u"MPEG Layer-3";
        }
    };
    struct Standard {std::vector<std::uint8_t> bytes;std::u16string text;};
    const auto put=[](std::vector<std::uint8_t>& bytes,std::size_t at,std::uint64_t value,unsigned size){for(unsigned i=0;i<size;++i)bytes.at(at+i)=static_cast<std::uint8_t>(value>>(8*i));};
    const auto describe=[](unsigned tag,unsigned channels,unsigned rate,unsigned bits,unsigned average){
        char text[96];const char* layout=channels==1?"Mono":"Stereo";
        if(x64arm::acm::is_mpeg_audio(tag))std::snprintf(text,sizeof text,"%u kBit/s, %u.%03u kHz, %s",average*8/1000,rate/1000,rate%1000,layout);
        else std::snprintf(text,sizeof text,"%u.%03u kHz, %u Bit, %s",rate/1000,rate%1000,bits,layout);
        return utf8_to_utf16(text);};
    const auto wave=[&](unsigned tag,unsigned channels,unsigned rate,unsigned average,unsigned align,unsigned bits){
        std::vector<std::uint8_t> bytes(format_size(tag));put(bytes,0,tag,2);put(bytes,2,channels,2);put(bytes,4,rate,4);put(bytes,8,average,4);put(bytes,12,align,2);put(bytes,14,bits,2);
        if(bytes.size()>16)put(bytes,16,bytes.size()-18,2);return bytes;};
    const auto standards=[&](unsigned tag,unsigned driver){
        std::vector<Standard> out;
        if(driver==x64arm::acm::gsm_codec){
            for(const unsigned rate:{8000U,11025U,22050U,44100U,48000U,96000U}){
                if(tag==1)out.push_back({wave(1,1,rate,rate*2,2,16),describe(1,1,rate,16,rate*2)});
                else{
                    const auto bytes=standard_gsm(rate);
                    out.push_back({std::vector<std::uint8_t>(bytes.begin(),bytes.end()),describe(tag,1,rate,0,
                        rate*x64arm::acm::gsm_block_bytes/x64arm::acm::gsm_samples)});
                }
            }
        }else if(tag==1 || tag==3){for(const unsigned rate:{8000U,11025U,22050U,44100U,48000U})for(const unsigned bits:tag==1?std::vector<unsigned>{8,16}:std::vector<unsigned>{32})for(const unsigned channels:{1U,2U}){
            const auto align=channels*bits/8;out.push_back({wave(tag,channels,rate,rate*align,align,bits),describe(tag,channels,rate,bits,rate*align)});}}
        else if(is_adpcm_format(tag)){
            for(const unsigned rate:{8000U,11025U,22050U,44100U})for(const unsigned channels:{1U,2U}){
                out.push_back({standard_adpcm(rate,channels,tag),describe(tag,channels,rate,4,0)});}}
        else if(x64arm::acm::is_g711(tag)){
            for(const unsigned rate:{8000U,11025U,22050U,44100U})for(const unsigned channels:{1U,2U}){
                out.push_back({wave(tag,channels,rate,rate*channels,channels,8),describe(tag,channels,rate,8,rate*channels)});
            }
        }else if(tag==x64arm::acm::mpeg){
            for(const unsigned layer:{1U,2U,4U})for(const unsigned channels:{1U,2U}){
                constexpr unsigned rate=44100,bitrate=128000;
                auto bytes=wave(tag,channels,rate,bitrate/8,1,0);
                put(bytes,18,layer,2);put(bytes,20,bitrate,4);put(bytes,24,channels==1?8U:1U,2);put(bytes,30,16,2);
                out.push_back({std::move(bytes),describe(tag,channels,rate,0,bitrate/8)});
            }
        }else{
            static constexpr std::array<std::array<unsigned,3>,9> rates{{{8000,1,8},{11025,1,16},{22050,1,32},{22050,2,64},{32000,2,96},{44100,2,128},{44100,2,192},{48000,2,128},{48000,2,256}}};
            for(const auto& [rate,channels,kbps]:rates){auto bytes=wave(0x55,channels,rate,kbps*125,1,0);put(bytes,18,1,2);put(bytes,20,2,4);put(bytes,24,(rate>=32000?144U:72U)*kbps*1000/rate,2);put(bytes,26,1,2);put(bytes,28,1393,2);
                out.push_back({std::move(bytes),describe(0x55,channels,rate,0,kbps*125)});}}
        return out;};
    const auto text_bytes=[&](std::vector<std::uint8_t>& bytes,std::size_t at,std::u16string_view text,unsigned chars){
        if(wide_out){for(std::size_t i=0;i+1<chars && i<text.size();++i)put(bytes,at+i*2,text[i],2);return;}
        auto narrow=utf16_to_ansi(text);if(narrow.size()>=chars)narrow.resize(chars-1);std::copy(narrow.begin(),narrow.end(),bytes.begin()+static_cast<std::ptrdiff_t>(at));};
    // 依呼叫端 cbStruct 寫入（至多完整大小），並回寫實際大小。
    const auto write_struct=[&](std::uint64_t address,std::vector<std::uint8_t> bytes){
        const auto size=std::min<std::size_t>(memory.load(GuestAddress{address},32),bytes.size());put(bytes,0,size,4);memory.write(GuestAddress{address},std::span(bytes).first(size));};
    const auto driver_details=[&](unsigned index){
        const unsigned head=x86_?40U:44U;std::vector<std::uint8_t> bytes(head+880*cu);const auto& info=catalog[index];
        put(bytes,0,bytes.size(),4);put(bytes,4,0x63647561,4);put(bytes,16,0x04000000,4);put(bytes,20,0x01000000,4);
        put(bytes,24,codec_driver?info.support:support_of(index),4);
        put(bytes,28,info.tag_count+(codec_driver && index?1U:0U),4);
        if(index==0)put(bytes,32,1,4);
        text_bytes(bytes,head,info.short_name,32);text_bytes(bytes,head+32*cu,info.long_name,128);text_bytes(bytes,head+(32+128+80+128)*cu,info.features,512);return bytes;};
    // ACMFORMATDETAILS：{cbStruct, dwFormatIndex, dwFormatTag, fdwSupport, pwfx, cbwfx, szFormat[128]}。
    const unsigned format_head=16+pointer+4;
    const auto format_details=[&](unsigned format_index,unsigned tag,std::uint32_t support,std::uint64_t pwfx,unsigned cbwfx,std::u16string_view text){
        std::vector<std::uint8_t> bytes(format_head+128*cu);put(bytes,0,bytes.size(),4);put(bytes,4,format_index,4);put(bytes,8,tag,4);put(bytes,12,support,4);put(bytes,16,pwfx,pointer);put(bytes,16+pointer,cbwfx,4);
        text_bytes(bytes,format_head,text,128);return bytes;};
    const auto volume_description=[](std::uint32_t gain){
        char text[48];
        std::snprintf(text,sizeof text,"%llu%%",static_cast<unsigned long long>(
            (std::uint64_t{gain}*100+x64arm::acm::unity_gain/2)/x64arm::acm::unity_gain));
        return utf8_to_utf16(text);
    };
    const auto all_drivers=[&](bool include_disabled,bool exclude_local){
        std::vector<std::pair<std::uint64_t,unsigned>> drivers;
        for(const auto token:acm.order){
            if(const auto index=builtin_index(token)){
                if(include_disabled || acm.enabled[*index])drivers.emplace_back(token,support_of(*index));
            }else if(const auto found=acm.registered.find(token);found!=acm.registered.end()){
                const auto& driver=*found->second;
                if((driver.enabled || include_disabled) && (driver.global || !exclude_local))
                    drivers.emplace_back(token,x64arm::acm::registered_support(driver));
            }
        }
        return drivers;
    };
    const auto notify_drivers=[&]{
        for(const auto& [token,driver]:acm.registered){
            (void)token;
            if(driver->window && wave_callbacks_.size()<4096 && gui_.windows.contains(driver->window))
                wave_callbacks_.push_back({resolve("user32.dll","PostMessageW").value,{driver->window,driver->message,0,0}});
        }
    };
    const auto opened=[&](std::uint64_t had)->std::uint64_t{
        if(!had)return 0;
        if(const auto found=acm.drivers.find(had);found!=acm.drivers.end())return driver_base+found->second;
        if(const auto found=acm.registered_instances.find(had);found!=acm.registered_instances.end() && !found->second->busy)
            return found->second->driver->token;
        throw AcmError{5};
    };
    const auto read_pointer=[&](std::uint64_t address){return memory.load(GuestAddress{address},pointer_bits);};
    struct OfferedTag {
        std::uint64_t driver;
        unsigned support,tag,index,size,count;
        std::u16string_view label;
        const x64arm::acm::RegisteredDriver::Tag* registered{};
    };
    const auto tags=[&](std::uint64_t only,bool filter){
        std::vector<OfferedTag> offers;
        const auto append_builtin=[&](unsigned index,std::uint64_t token,unsigned support){
            if(filter){
                if(index==0)offers.push_back({token,support,x64arm::acm::volume_tag,0,x64arm::acm::volume_size,
                                             std::size(x64arm::acm::standard_gains),u"Volume"});
                return;
            }
            const auto append=[&](unsigned tag,unsigned position){
                offers.push_back({token,support,tag,position,format_size(tag),
                                  static_cast<unsigned>(standards(tag,index).size()),tag_name(tag)});
            };
            const unsigned pcm_first=codec_driver && index?1U:0U;
            if(pcm_first)append(1,0);
            for(unsigned i=0;i<catalog[index].tag_count;++i)append(catalog[index].tags[i],i+pcm_first);
        };
        if(codec_driver){
            const auto index=*codec_driver;
            append_builtin(index,driver_base+index,catalog[index].support);
            return offers;
        }
        for(const auto& [token,support]:all_drivers(only!=0,false)){
            if(only && only!=token)continue;
            if(const auto index=builtin_index(token)){
                append_builtin(*index,token,support);
            }else{
                const auto& driver=*acm.registered.at(token);
                for(const auto& tag:filter?driver.filters:driver.formats)
                    offers.push_back({token,support,tag.tag,tag.index,tag.size,tag.standard_count,tag.label,&tag});
            }
        }
        return offers;
    };
    const auto tag_details=[&](const OfferedTag& tag){
        std::vector<std::uint8_t> bytes(24+48*cu);
        put(bytes,0,bytes.size(),4);
        put(bytes,4,tag.index,4);
        put(bytes,8,tag.tag,4);
        put(bytes,12,tag.size,4);
        put(bytes,16,tag.support,4);
        put(bytes,20,tag.count,4);
        text_bytes(bytes,24,tag.label,48);
        return bytes;
    };
    struct OfferedFormat {
        std::uint64_t driver;
        unsigned support,tag,index;
        std::u16string_view label;
        Standard standard;
    };
    const auto standard_for_tag=[&](const OfferedTag& tag,bool filter){
        std::vector<OfferedFormat> offers;
        if(tag.registered){
            for(const auto& item:tag.registered->standards)
                offers.push_back({tag.driver,tag.support,tag.tag,item.index,tag.label,{item.bytes,item.label}});
        }else if(filter){
            unsigned index=0;
            for(const auto gain:x64arm::acm::standard_gains)
                offers.push_back({tag.driver,tag.support,tag.tag,index++,tag.label,
                                  {x64arm::acm::volume_format(gain),volume_description(gain)}});
        }else{
            auto entries=standards(tag.tag,static_cast<unsigned>(tag.driver-driver_base));
            for(unsigned i=0;i<entries.size();++i)
                offers.push_back({tag.driver,tag.support,tag.tag,i,tag.label,std::move(entries[i])});
        }
        return offers;
    };
    const auto formats_matching=[&](std::uint64_t format,unsigned flags,std::uint64_t only){
        if(flags&~0x1ff0000U)throw AcmError{10};
        if((flags&0x3f0000U) && !format)throw AcmError{11};
        std::vector<OfferedFormat> offers;
        const auto field=[&](unsigned at,std::uint8_t bits){return static_cast<unsigned>(memory.load(GuestAddress{format+at},bits));};
        const bool converting=(flags&0x100000U)!=0;
        const auto source=converting && builtin_conversion_source(field(0,16))?
            std::optional{read_format(memory,format)}:std::nullopt;
        for(const auto& tag:tags(only,false)){
            if((flags&0x10000U) && tag.tag!=field(0,16))continue;
            if((flags&0x1c00000U) && !(tag.support&8U))continue;
            if((flags&0x200000U) && tag.tag!=1)continue;
            if(converting && !source && tag.tag!=1 && tag.tag!=3)continue;
            for(auto& item:standard_for_tag(tag,false)){
                if(item.standard.bytes.size()<16)continue;
                const auto value=[&](unsigned at,unsigned size){
                    unsigned result=0;
                    for(unsigned k=0;k<size;++k)result|=unsigned{item.standard.bytes[at+k]}<<(8*k);
                    return result;
                };
                if((flags&0x20000U) && value(2,2)!=field(2,16))continue;
                if((flags&0x40000U) && value(4,4)!=field(4,32))continue;
                if((flags&0x80000U) && value(14,2)!=field(14,16))continue;
                if((flags&0x200000U) && (tag.tag!=1 || value(14,2)!=16 || value(2,2)!=field(2,16) || value(4,4)!=field(4,32)))continue;
                if(source && !tag.registered){
                    const Format target{tag.tag,value(2,2),value(4,4),value(8,4),value(12,2),value(14,2),{}};
                    const auto codec=builtin_codec_for(source->tag,tag.tag);
                    if(!acm.enabled[codec] || !compatible_codec_formats(*source,target))continue;
                }
                offers.push_back(std::move(item));
            }
        }
        return offers;
    };
    const auto filters_matching=[&](std::uint64_t format,unsigned flags,std::uint64_t only){
        if(flags&~0x10000U)throw AcmError{10};
        if(flags && !format)throw AcmError{11};
        std::vector<OfferedFormat> offers;
        const auto requested=flags?static_cast<unsigned>(memory.load(GuestAddress{format+4},32)):0;
        for(const auto& tag:tags(only,true)){
            if(flags && requested!=tag.tag)continue;
            for(auto& item:standard_for_tag(tag,true))offers.push_back(std::move(item));
        }
        return offers;
    };
    // 回呼的詳細資料與格式各自有暫存副本，可使用至本次列舉結束。
    struct EnumerationItem {
        std::uint64_t driver;
        unsigned support;
        std::vector<std::uint8_t> details,format;
    };
    const auto enumerate=[&](std::uint64_t procedure,std::vector<EnumerationItem> items,std::uint64_t instance){
        if(!procedure)throw AcmError{11};
        memory.check(GuestAddress{procedure},1,Access::Execute);
        if(items.empty()){arg.return_to(state,0);return;}
        const auto aligned=[](std::size_t size){return (size+7)&~std::size_t{7};};
        std::size_t size=0;
        for(const auto& item:items)size+=aligned(item.details.size())+aligned(item.format.size());
        const auto buffer=allocate_heap(memory,size);
        if(!buffer)throw AcmError{7};
        try{
            std::vector<GuestCallback> callbacks;
            callbacks.reserve(items.size());
            auto at=buffer;
            for(auto& item:items){
                const auto detail_at=at;
                at+=aligned(item.details.size());
                if(!item.format.empty()){
                    put(item.details,16,at,pointer);
                    put(item.details,16+pointer,item.format.size(),4);
                    memory.write(GuestAddress{at},item.format);
                    at+=aligned(item.format.size());
                }
                memory.write(GuestAddress{detail_at},item.details);
                callbacks.push_back({procedure,{item.driver,detail_at,instance,item.support}});
            }
            start_callbacks(state,memory,std::move(callbacks),Continuation::AcmEnumeration,buffer);
        }catch(...){(void)free_heap(memory,buffer,state);throw;}
    };
#include "windows_acm_choose.inc"
    if(name=="acmMetrics"){
        // (hao, uMetric, pMetric)：計數依啟用狀態與支援類型。
        // MAX_SIZE_FORMAT 取啟用中（或指定）驅動程式的最大格式；DRIVER_SUPPORT／PRIORITY 與硬體裝置編號需指定驅動程式。
        if(!arg(2))throw AcmError{11};const auto metric=d(1);std::uint32_t value=0;
        const auto target=[&]()->std::optional<unsigned>{if(!arg(0))return std::nullopt;const auto index=driver_of(arg(0));if(!index)throw AcmError{5};return index;};
        if(metric==100 || metric==101 || metric==30 || metric==31){
            const auto index=target();if(!index)throw AcmError{5};if(metric==30 || metric==31)throw AcmError{512};
            value=metric==100?support_of(*index):static_cast<std::uint32_t>(std::find(acm.order.begin(),acm.order.end(),
                                                                                   driver_base+*index)-acm.order.begin())+1;
        }else if(metric==50 || metric==51){
            const auto only=target();
            if(metric==50)for(unsigned i=0;i<catalog.size();++i)if(only?*only==i:acm.enabled[i] && acm.installed[i])for(unsigned k=0;k<catalog[i].tag_count;++k)value=std::max(value,format_size(catalog[i].tags[k]));
            if(metric==51 && (only?*only==0:acm.enabled[0]))value=x64arm::acm::volume_size;
            if(!only)for(const auto& [token,driver]:acm.registered){
                (void)token;
                if(driver->enabled && !driver->busy)
                    for(const auto& item:metric==50?driver->formats:driver->filters)value=std::max(value,item.size);
            }
        }else{
            if(arg(0))throw AcmError{5};
            const auto installed=all_drivers(true,false);
            const auto count=[&](std::uint32_t flag,bool disabled,bool local=false){
                return static_cast<std::uint32_t>(std::count_if(installed.begin(),installed.end(),[&](const auto& driver){
                    const auto support=driver.second;
                    return disabled==bool(support&x64arm::acm::disabled_support) &&
                        (!local || (support&x64arm::acm::local_support)) && (!flag || (support&flag));
                }));
            };
            switch(metric){case 1:value=count(0,false);break;case 2:value=count(1,false);break;case 3:value=count(2,false);break;case 5:value=count(0,true);break;
            case 4:value=count(x64arm::acm::filter_support,false);break;
            case 6:value=count(8,false);break;
            case 20:value=count(0,false,true);break;case 21:value=count(1,false,true);break;
            case 22:value=count(2,false,true);break;case 23:value=count(4,false,true);break;
            case 24:value=count(0,true,true);break;default:throw AcmError{10};}
        }
        memory.store(GuestAddress{arg(2)},32,value);arg.return_to(state,0);return {};
    }
    if(name=="acmDriverEnum"){
        // (fnCallback, dwInstance, fdwEnum)：ACM_DRIVERENUMF_NOLOCAL、ACM_DRIVERENUMF_DISABLED。
        if(d(2)&~0xc0000000U)throw AcmError{10};
        if(!arg(0))throw AcmError{11};
        memory.check(GuestAddress{arg(0)},1,Access::Execute);
        std::vector<GuestCallback> callbacks;
        for(const auto& [token,support]:all_drivers((d(2)&0x80000000U)!=0,(d(2)&0x40000000U)!=0))
            callbacks.push_back({arg(0),{token,arg(1),support}});
        if(callbacks.empty()){arg.return_to(state,0);return {};}
        start_callbacks(state,memory,std::move(callbacks),Continuation::AcmEnumeration,0);
        return {};
    }
    if(name.starts_with("acmDriverDetails")){
        // (hadid, padd, fdwDetails)
        if(d(2))throw AcmError{10};const auto index=codec_driver?codec_driver:driver_of(arg(0));if(!index)throw AcmError{5};if(!arg(1) || memory.load(GuestAddress{arg(1)},32)<4)throw AcmError{11};
        write_struct(arg(1),driver_details(*index));arg.return_to(state,0);return {};
    }
    if(name=="acmDriverOpen"){
        if(!arg(0))throw AcmError{11};if(d(2))throw AcmError{10};const auto index=builtin_index(arg(1));
        if(!index)throw AcmError{5};if(!acm.enabled[*index])throw AcmError{3};if(acm.drivers.size()>=256)throw AcmError{4};
        const auto handle=acm.next_driver++;acm.drivers[handle]=*index;memory.store(GuestAddress{arg(0)},pointer_bits,handle);arg.return_to(state,0);return {};
    }
    if(name=="acmDriverClose"){
        // 仍有以此驅動程式開啟的串流時回報 ACMERR_BUSY。
        if(d(1))throw AcmError{10};if(!acm.drivers.contains(arg(0)))throw AcmError{5};
        for(const auto& entry:acm.streams)if(entry.second->driver==arg(0))throw AcmError{513};
        acm.drivers.erase(arg(0));arg.return_to(state,0);return {};
    }
    if(name=="acmDriverRemove"){
        if(d(1))throw AcmError{10};
        const auto index=builtin_index(arg(0));
        if(!index)throw AcmError{5};
        for(const auto& [handle,loaded]:acm.drivers){(void)handle;if(loaded==*index)throw AcmError{513};}
        for(const auto& [handle,stream]:acm.streams){(void)stream;if(driver_of(handle)==index)throw AcmError{513};}
        acm.installed[*index]=acm.enabled[*index]=false;
        std::erase(acm.order,arg(0));
        notify_drivers();
        arg.return_to(state,0);
        return {};
    }
    if(name=="acmDriverID"){
        if(d(2))throw AcmError{10};if(!arg(1))throw AcmError{11};const auto index=driver_of(arg(0));if(!index)throw AcmError{5};
        memory.store(GuestAddress{arg(1)},pointer_bits,driver_base+*index);arg.return_to(state,0);return {};
    }
    if(name=="acmDriverMessage"){
        // 只接受 DRV_QUERYCONFIGURE／DRV_CONFIGURE（可傳 HACMDRIVERID）與 ACMDM_USER 起的驅動程式自訂範圍（需 HACMDRIVER）。
        // 內建驅動程式沒有設定對話框：查詢回 0、設定同 DefDriverProc 回 0；自訂訊息回 MMSYSERR_NOTSUPPORTED。
        const auto message=d(1);const bool configure=message==7 || message==8;
        if(!acm.drivers.contains(arg(0)) && !(configure && driver_of(arg(0)) && !acm.streams.contains(arg(0))))throw AcmError{5};
        if(configure){arg.return_to(state,0);return {};}if(message>=0x4000 && message<0x6000)throw AcmError{8};throw AcmError{11};
    }
    if(name=="acmDriverPriority"){
        // (hadid, dwPriority, fdwPriority)：ENABLE 1、DISABLE 2、BEGIN 0x10000、END 0x20000；優先順序 0 不變、-1 移到最後。
        const auto flags=d(2);if((flags&~0x30003U) || (flags&3)==3 || (flags&0x30000)==0x30000)throw AcmError{10};
        if(flags&0x30000){if(arg(0) || d(1) || (flags&3))throw AcmError{11};arg.return_to(state,0);return {};}
        const auto index=builtin_index(arg(0));if(!index)throw AcmError{5};const auto priority=d(1);
        const auto global_begin=std::find_if(acm.order.begin(),acm.order.end(),[&](const auto token){
            const auto found=acm.registered.find(token);
            return found==acm.registered.end() || found->second->global;
        });
        if(priority!=UINT32_MAX && static_cast<std::size_t>(acm.order.end()-global_begin)<priority)throw AcmError{11};
        const auto locals=global_begin-acm.order.begin();
        if(flags&1)acm.enabled[*index]=true;if(flags&2)acm.enabled[*index]=false;
        if(priority){std::erase(acm.order,arg(0));acm.order.insert(priority==UINT32_MAX?acm.order.end():
            acm.order.begin()+locals+static_cast<std::ptrdiff_t>(priority-1),arg(0));}
        if(flags || priority)notify_drivers();
        arg.return_to(state,0);return {};
    }
    const bool filter=name.starts_with("acmFilter");
    const bool tag_operation=name.starts_with("acmFormatTag") || name.starts_with("acmFilterTag");
    if(name.find("Details")!=std::string_view::npos){
        const auto mode=d(2);
        const auto only=opened(arg(0));
        const auto details=arg(1);
        if(mode>(tag_operation?2U:1U))throw AcmError{10};
        if(!details || memory.load(GuestAddress{details},32)<(tag_operation?24U:format_head))throw AcmError{11};
        const auto offered=tags(only,filter);
        if(tag_operation){
            const auto wanted=static_cast<unsigned>(memory.load(GuestAddress{details+(mode?8:4)},32));
            const OfferedTag* match=nullptr;
            if(mode==0){
                if(wanted<offered.size())match=&offered[wanted];
            }else for(const auto& item:offered){
                if(mode==2 && !wanted){
                    if(!match || match->size<item.size)match=&item;
                }else if(item.tag==wanted){match=&item;break;}
            }
            if(!match)throw AcmError{512};
            write_struct(details,tag_details(*match));
        }else{
            const auto data=read_pointer(details+16);
            const auto capacity=static_cast<unsigned>(memory.load(GuestAddress{details+16+pointer},32));
            if(!data || capacity<(mode?filter?8U:16U:1U))throw AcmError{11};
            const auto index=static_cast<unsigned>(memory.load(GuestAddress{details+4},32));
            const auto requested=static_cast<unsigned>(mode==0?memory.load(GuestAddress{details+8},32):
                filter?memory.load(GuestAddress{data+4},32):memory.load(GuestAddress{data},16));
            const auto tag=std::find_if(offered.begin(),offered.end(),[&](const auto& item){return item.tag==requested;});
            if(tag==offered.end())throw AcmError{512};
            auto entries=standard_for_tag(*tag,filter);
            auto selected=index;
            auto size=capacity;
            std::u16string label;
            if(mode==0){
                const auto item=std::find_if(entries.begin(),entries.end(),[&](const auto& entry){return entry.index==index;});
                if(item==entries.end())throw AcmError{512};
                if(item->standard.bytes.size()>capacity)throw AcmError{filter?512U:11U};
                memory.write(GuestAddress{data},item->standard.bytes);
                size=static_cast<unsigned>(item->standard.bytes.size());
                label=item->standard.text;
            }else if(tag->registered){
                std::vector<std::uint8_t> format(std::min(capacity,tag->size));
                memory.read(GuestAddress{data},format);
                const auto item=std::find_if(entries.begin(),entries.end(),[&](const auto& entry){
                    return entry.standard.bytes.size()<=format.size() &&
                        std::equal(entry.standard.bytes.begin(),entry.standard.bytes.end(),format.begin());
                });
                if(item==entries.end())throw AcmError{512};
                selected=item->index;
                size=static_cast<unsigned>(item->standard.bytes.size());
                label=item->standard.text;
            }else if(filter){
                if(capacity<x64arm::acm::volume_size)throw AcmError{512};
                const auto gain=read_volume(memory,data);
                const auto match=std::find(std::begin(x64arm::acm::standard_gains),std::end(x64arm::acm::standard_gains),gain);
                selected=match==std::end(x64arm::acm::standard_gains)?UINT32_MAX:
                    static_cast<unsigned>(match-std::begin(x64arm::acm::standard_gains));
                size=x64arm::acm::volume_size;
                label=volume_description(gain);
            }else{
                const auto format=read_format(memory,data,capacity);
                label=describe(format.tag,format.channels,format.rate,format.bits,format.average);
                size=std::max(capacity,format_size(format.tag));
            }
            write_struct(details,format_details(selected,requested,tag->support,data,size,label));
        }
        arg.return_to(state,0);
        return {};
    }
    if(name.ends_with("EnumA") || name.ends_with("EnumW")){
        const auto flags=d(4);
        if(flags&~(tag_operation?0U:filter?0x10000U:0x1ff0000U))throw AcmError{10};
        const auto only=opened(arg(0)),address=arg(1);
        if(!address || memory.load(GuestAddress{address},32)<(tag_operation?24U:format_head))throw AcmError{11};
        std::vector<EnumerationItem> items;
        if(tag_operation){
            for(const auto& tag:tags(only,filter))items.push_back({tag.driver,tag.support,tag_details(tag),{}});
        }else{
            const auto data=read_pointer(address+16);
            if(filter && (!data || !memory.load(GuestAddress{address+16+pointer},32)))throw AcmError{11};
            auto offered=filter?filters_matching(data,flags,only):formats_matching(data,flags,only);
            for(auto& item:offered){
                auto info=format_details(item.index,item.tag,item.support,0,0,item.standard.text);
                items.push_back({item.driver,item.support,std::move(info),std::move(item.standard.bytes)});
            }
        }
        enumerate(arg(2),std::move(items),arg(3));
        return {};
    }
}

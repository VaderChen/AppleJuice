// msacm32 的驅動程式、格式與濾波器目錄（由 windows_acm.cpp 的 invoke_acm 引入）。
// 內建三個驅動程式，與 acmStreamOpen 支援的轉換一致：PCM 轉換器（WAVE_FORMAT_PCM、WAVE_FORMAT_IEEE_FLOAT）、MS ADPCM 解碼器與 MPEG Layer-3 解碼器；沒有濾波器。
// HACMDRIVERID 為固定值，acmDriverOpen 另發 HACMDRIVER；acmDriverPriority 調整列舉順序與停用狀態（本程序內）。
// 結構依 msacm.h 以 1 位元組封裝；列舉以 start_callbacks 逐項呼叫，每項傳入自己的詳細資料複本，回呼傳回 FALSE 時停止。
if(name.starts_with("acmDriver") || name.starts_with("acmFormatTag") || name.starts_with("acmFormatDetails") || name.starts_with("acmFormatEnum") || name.starts_with("acmFilter") || name=="acmMetrics"){
    if(!acm_)acm_=std::make_shared<AcmState>();auto& acm=*acm_;
    const bool wide_out=name.ends_with('W');const unsigned pointer=x86_?4U:8U,cu=wide_out?2U:1U;
    struct DriverInfo {std::u16string_view short_name,long_name,features;std::uint32_t support;std::array<unsigned,2> tags;unsigned tag_count;};
    static constexpr std::array<DriverInfo,3> catalog{{
        {u"PCM",u"PCM Converter",u"Converts PCM and IEEE float audio between sample rates, sample sizes and channel counts.",0x2,{1,3},2},
        {u"ADPCM",u"MS ADPCM Decoder",u"Decodes Microsoft ADPCM to PCM.",0x1,{2,0},1},
        {u"MPEG Layer-3",u"MPEG Layer-3 Decoder",u"Decodes MPEG audio layer 3 to PCM.",0x1,{0x55,0},1}}};
    constexpr std::uint64_t driver_base=0x6c000001;
    const auto support_of=[&](unsigned index){return catalog[index].support|(acm.enabled[index]?0U:0x80000000U);};
    // HACMDRIVERID、HACMDRIVER 或 HACMSTREAM（依來源格式）對應的驅動程式。
    const auto driver_of=[&](std::uint64_t handle)->std::optional<unsigned>{
        if(handle>=driver_base && handle<driver_base+catalog.size())return static_cast<unsigned>(handle-driver_base);
        if(const auto found=acm.drivers.find(handle);found!=acm.drivers.end())return found->second;
        if(const auto found=acm.streams.find(handle);found!=acm.streams.end())return found->second->source.tag==2?1U:found->second->source.tag==0x55?2U:0U;
        return std::nullopt;};
    const auto format_size=[](unsigned tag)->unsigned{return tag==1?16U:tag==3?18U:tag==2?50U:30U;};
    const auto tag_name=[](unsigned tag)->std::u16string_view{return tag==1?u"PCM":tag==3?u"IEEE Float":tag==2?u"Microsoft ADPCM":u"MPEG Layer-3";};
    // 標準格式：PCM 5 種取樣率 × 8／16 位元 × 單聲道／立體聲；float 32 位元；ADPCM 4 種取樣率；MP3 常見位元率。
    struct Standard {std::vector<std::uint8_t> bytes;std::u16string text;};
    const auto put=[](std::vector<std::uint8_t>& bytes,std::size_t at,std::uint64_t value,unsigned size){for(unsigned i=0;i<size;++i)bytes.at(at+i)=static_cast<std::uint8_t>(value>>(8*i));};
    const auto describe=[](unsigned tag,unsigned channels,unsigned rate,unsigned bits,unsigned average){
        char text[96];const char* layout=channels==1?"Mono":"Stereo";
        if(tag==0x55)std::snprintf(text,sizeof text,"%u kBit/s, %u.%03u kHz, %s",average*8/1000,rate/1000,rate%1000,layout);
        else std::snprintf(text,sizeof text,"%u.%03u kHz, %u Bit, %s",rate/1000,rate%1000,bits,layout);
        return utf8_to_utf16(text);};
    const auto wave=[&](unsigned tag,unsigned channels,unsigned rate,unsigned average,unsigned align,unsigned bits){
        std::vector<std::uint8_t> bytes(format_size(tag));put(bytes,0,tag,2);put(bytes,2,channels,2);put(bytes,4,rate,4);put(bytes,8,average,4);put(bytes,12,align,2);put(bytes,14,bits,2);
        if(bytes.size()>16)put(bytes,16,bytes.size()-18,2);return bytes;};
    const auto standards=[&](unsigned tag){
        std::vector<Standard> out;
        if(tag==1 || tag==3){for(const unsigned rate:{8000U,11025U,22050U,44100U,48000U})for(const unsigned bits:tag==1?std::vector<unsigned>{8,16}:std::vector<unsigned>{32})for(const unsigned channels:{1U,2U}){
            const auto align=channels*bits/8;out.push_back({wave(tag,channels,rate,rate*align,align,bits),describe(tag,channels,rate,bits,rate*align)});}}
        else if(tag==2){
            static constexpr std::array<std::int16_t,14> coefficients{256,0,512,-256,0,0,192,64,240,0,460,-208,392,-232};
            for(const unsigned rate:{8000U,11025U,22050U,44100U})for(const unsigned channels:{1U,2U}){
                const unsigned align=(rate<=11025?256U:rate<=22050?512U:1024U)*channels,samples=(align-7*channels)*8/(4*channels)+2;
                auto bytes=wave(2,channels,rate,rate*align/samples,align,4);put(bytes,18,samples,2);put(bytes,20,7,2);
                for(std::size_t i=0;i<coefficients.size();++i)put(bytes,22+i*2,static_cast<std::uint16_t>(coefficients[i]),2);
                out.push_back({std::move(bytes),describe(2,channels,rate,4,0)});}}
        else{
            static constexpr std::array<std::array<unsigned,3>,9> rates{{{8000,1,8},{11025,1,16},{22050,1,32},{22050,2,64},{32000,2,96},{44100,2,128},{44100,2,192},{48000,2,128},{48000,2,256}}};
            for(const auto& [rate,channels,kbps]:rates){auto bytes=wave(0x55,channels,rate,kbps*125,1,0);put(bytes,18,1,2);put(bytes,20,2,4);put(bytes,24,(rate>=32000?144U:72U)*kbps*1000/rate,2);put(bytes,26,1,2);put(bytes,28,1393,2);
                out.push_back({std::move(bytes),describe(0x55,channels,rate,0,kbps*125)});}}
        return out;};
    const auto text_bytes=[&](std::vector<std::uint8_t>& bytes,std::size_t at,std::u16string_view text,unsigned chars){
        if(wide_out){for(std::size_t i=0;i+1<chars && i<text.size();++i)put(bytes,at+i*2,text[i],2);return;}
        auto narrow=utf16_to_utf8(text);if(narrow.size()>=chars)narrow.resize(chars-1);std::copy(narrow.begin(),narrow.end(),bytes.begin()+static_cast<std::ptrdiff_t>(at));};
    // 依呼叫端 cbStruct 寫入（至多完整大小），並回寫實際大小。
    const auto write_struct=[&](std::uint64_t address,std::vector<std::uint8_t> bytes){
        const auto size=std::min<std::size_t>(memory.load(GuestAddress{address},32),bytes.size());put(bytes,0,size,4);memory.write(GuestAddress{address},std::span(bytes).first(size));};
    const auto driver_details=[&](unsigned index){
        const unsigned head=x86_?40U:44U;std::vector<std::uint8_t> bytes(head+880*cu);const auto& info=catalog[index];
        put(bytes,0,bytes.size(),4);put(bytes,4,0x63647561,4);put(bytes,16,0x04000000,4);put(bytes,20,0x01000000,4);put(bytes,24,support_of(index),4);put(bytes,28,info.tag_count,4);
        text_bytes(bytes,head,info.short_name,32);text_bytes(bytes,head+32*cu,info.long_name,128);text_bytes(bytes,head+(32+128+80+128)*cu,info.features,512);return bytes;};
    const auto tag_details=[&](unsigned index,unsigned tag,unsigned tag_index){
        std::vector<std::uint8_t> bytes(24+48*cu);put(bytes,0,bytes.size(),4);put(bytes,4,tag_index,4);put(bytes,8,tag,4);put(bytes,12,format_size(tag),4);put(bytes,16,support_of(index),4);
        put(bytes,20,standards(tag).size(),4);text_bytes(bytes,24,tag_name(tag),48);return bytes;};
    // ACMFORMATDETAILS：{cbStruct, dwFormatIndex, dwFormatTag, fdwSupport, pwfx, cbwfx, szFormat[128]}。
    const unsigned format_head=16+pointer+4;
    const auto format_details=[&](unsigned format_index,unsigned tag,std::uint32_t support,std::uint64_t pwfx,unsigned cbwfx,std::u16string_view text){
        std::vector<std::uint8_t> bytes(format_head+128*cu);put(bytes,0,bytes.size(),4);put(bytes,4,format_index,4);put(bytes,8,tag,4);put(bytes,12,support,4);put(bytes,16,pwfx,pointer);put(bytes,16+pointer,cbwfx,4);
        text_bytes(bytes,format_head,text,128);return bytes;};
    // 啟用中的驅動程式（依優先順序）與其格式標籤；had 指定時只取該驅動程式。
    const auto drivers_in_order=[&](bool include_disabled){std::vector<unsigned> list;for(const auto index:acm.order)if(include_disabled || acm.enabled[index])list.push_back(index);return list;};
    const auto tag_list=[&](std::optional<unsigned> only){std::vector<std::pair<unsigned,unsigned>> list;
        for(const auto index:drivers_in_order(only.has_value()))if(!only || *only==index)for(unsigned i=0;i<catalog[index].tag_count;++i)list.push_back({index,catalog[index].tags[i]});return list;};
    const auto opened=[&](std::uint64_t had)->std::optional<unsigned>{if(!had)return std::nullopt;const auto found=acm.drivers.find(had);if(found==acm.drivers.end())throw AcmError{5};return found->second;};
    const auto read_pointer=[&](std::uint64_t address){return memory.load(GuestAddress{address},pointer_bits);};
    // 每項回呼的暫存：依序放入詳細資料（與格式），參數為 {HACMDRIVERID, 詳細資料, dwInstance, fdwSupport}。
    const auto enumerate=[&](std::uint64_t procedure,std::vector<std::pair<unsigned,std::vector<std::uint8_t>>> items,std::uint64_t instance,bool with_details,std::vector<std::vector<std::uint8_t>> formats={}){
        if(!procedure)throw AcmError{11};memory.check(GuestAddress{procedure},1,Access::Execute);
        if(items.empty()){arg.return_to(state,0);return;}
        std::size_t size=0;for(std::size_t i=0;i<items.size();++i)size+=(items[i].second.size()+7)/8*8+(i<formats.size()?(formats[i].size()+7)/8*8:0);
        const auto buffer=with_details && size?allocate_heap(memory,size):0;if(with_details && size && !buffer)throw AcmError{7};
        std::vector<GuestCallback> callbacks;auto at=buffer;
        for(std::size_t i=0;i<items.size();++i){
            auto& [index,details]=items[i];std::uint64_t pointer_value=0;
            if(with_details){pointer_value=at;at+=(details.size()+7)/8*8;
                if(i<formats.size()){put(details,16,at,pointer);put(details,16+pointer,formats[i].size(),4);memory.write(GuestAddress{at},formats[i]);at+=(formats[i].size()+7)/8*8;}
                memory.write(GuestAddress{pointer_value},details);}
            callbacks.push_back(with_details?GuestCallback{procedure,{driver_base+index,pointer_value,instance,support_of(index)}}:GuestCallback{procedure,{driver_base+index,instance,support_of(index)}});
        }
        start_callbacks(state,memory,std::move(callbacks),Continuation::AcmEnumeration,buffer);};
    if(name=="acmMetrics"){
        // (hao, uMetric, pMetric)：計數依啟用狀態與支援類型（沒有濾波器、硬體與本程序加入的驅動程式）；
        // MAX_SIZE_FORMAT 取啟用中（或指定）驅動程式的最大格式；DRIVER_SUPPORT／PRIORITY 與硬體裝置編號需指定驅動程式。
        if(!arg(2))throw AcmError{11};const auto metric=d(1);std::uint32_t value=0;
        const auto target=[&]()->std::optional<unsigned>{if(!arg(0))return std::nullopt;const auto index=driver_of(arg(0));if(!index)throw AcmError{5};return index;};
        if(metric==100 || metric==101 || metric==30 || metric==31){
            const auto index=target();if(!index)throw AcmError{5};if(metric==30 || metric==31)throw AcmError{512};
            value=metric==100?support_of(*index):static_cast<std::uint32_t>(std::find(acm.order.begin(),acm.order.end(),*index)-acm.order.begin())+1;
        }else if(metric==50 || metric==51){
            const auto only=target();
            if(metric==50)for(unsigned i=0;i<catalog.size();++i)if(only?*only==i:acm.enabled[i])for(unsigned k=0;k<catalog[i].tag_count;++k)value=std::max(value,format_size(catalog[i].tags[k]));
        }else{
            if(arg(0))throw AcmError{5};
            const auto count=[&](std::uint32_t flag,bool disabled){std::uint32_t n=0;for(unsigned i=0;i<catalog.size();++i)if(acm.enabled[i]!=disabled && (!flag || (catalog[i].support&flag)))++n;return n;};
            switch(metric){case 1:value=count(0,false);break;case 2:value=count(1,false);break;case 3:value=count(2,false);break;case 5:value=count(0,true);break;
            case 4:case 6:case 20:case 21:case 22:case 23:case 24:value=0;break;default:throw AcmError{10};}
        }
        memory.store(GuestAddress{arg(2)},32,value);arg.return_to(state,0);return {};
    }
    if(name=="acmDriverEnum"){
        // (fnCallback, dwInstance, fdwEnum)：ACM_DRIVERENUMF_NOLOCAL、ACM_DRIVERENUMF_DISABLED。
        if(d(2)&~0xc0000000U)throw AcmError{10};std::vector<std::pair<unsigned,std::vector<std::uint8_t>>> items;
        for(const auto index:drivers_in_order((d(2)&0x80000000U)!=0))items.push_back({index,{}});
        enumerate(arg(0),std::move(items),arg(1),false);return {};
    }
    if(name.starts_with("acmDriverDetails")){
        // (hadid, padd, fdwDetails)
        if(d(2))throw AcmError{10};const auto index=driver_of(arg(0));if(!index)throw AcmError{5};if(!arg(1) || memory.load(GuestAddress{arg(1)},32)<4)throw AcmError{11};
        write_struct(arg(1),driver_details(*index));arg.return_to(state,0);return {};
    }
    if(name=="acmDriverOpen"){
        if(!arg(0))throw AcmError{11};if(d(2))throw AcmError{10};const auto index=arg(1)>=driver_base && arg(1)<driver_base+catalog.size()?std::optional<unsigned>(static_cast<unsigned>(arg(1)-driver_base)):std::nullopt;
        if(!index)throw AcmError{5};if(!acm.enabled[*index])throw AcmError{3};if(acm.drivers.size()>=256)throw AcmError{4};
        const auto handle=acm.next_driver++;acm.drivers[handle]=*index;memory.store(GuestAddress{arg(0)},pointer_bits,handle);arg.return_to(state,0);return {};
    }
    if(name=="acmDriverClose"){
        // 仍有以此驅動程式開啟的串流時回報 ACMERR_BUSY。
        if(d(1))throw AcmError{10};if(!acm.drivers.contains(arg(0)))throw AcmError{5};
        for(const auto& entry:acm.streams)if(entry.second->driver==arg(0))throw AcmError{513};
        acm.drivers.erase(arg(0));arg.return_to(state,0);return {};
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
        if(!(arg(0)>=driver_base && arg(0)<driver_base+catalog.size()))throw AcmError{5};const auto index=static_cast<unsigned>(arg(0)-driver_base);const auto priority=d(1);
        if(priority && priority!=UINT32_MAX && priority>catalog.size())throw AcmError{11};
        if(flags&1)acm.enabled[index]=true;if(flags&2)acm.enabled[index]=false;
        if(priority){std::erase(acm.order,index);acm.order.insert(priority==UINT32_MAX?acm.order.end():acm.order.begin()+static_cast<std::ptrdiff_t>(priority-1),index);}
        arg.return_to(state,0);return {};
    }
    if(name.starts_with("acmFormatTagDetails")){
        // (had, paftd, fdwDetails)：INDEX 0、FORMATTAG 1、LARGESTSIZE 2。
        const auto mode=d(2);if(mode>2)throw AcmError{10};const auto only=opened(arg(0));if(!arg(1) || memory.load(GuestAddress{arg(1)},32)<24)throw AcmError{11};
        const auto list=tag_list(only);const auto requested=static_cast<unsigned>(memory.load(GuestAddress{arg(1)+(mode?8:4)},32));std::optional<std::size_t> chosen;
        if(mode==0){if(requested<list.size())chosen=requested;}
        else for(std::size_t i=0;i<list.size();++i){if(mode==2 && !requested){if(!chosen || format_size(list[i].second)>format_size(list[*chosen].second))chosen=i;}else if(list[i].second==requested){chosen=i;break;}}
        if(!chosen)throw AcmError{512};
        const auto& [index,tag]=list[*chosen];unsigned tag_index=0;for(unsigned i=0;i<catalog[index].tag_count;++i)if(catalog[index].tags[i]==tag)tag_index=i;
        write_struct(arg(1),tag_details(index,tag,tag_index));arg.return_to(state,0);return {};
    }
    if(name.starts_with("acmFormatDetails")){
        // (had, pafd, fdwDetails)：INDEX 0 依 dwFormatTag／dwFormatIndex 取標準格式；FORMAT 1 描述 pwfx 指向的格式。
        const auto mode=d(2);if(mode>1)throw AcmError{10};const auto only=opened(arg(0));const auto details=arg(1);if(!details || memory.load(GuestAddress{details},32)<format_head)throw AcmError{11};
        const auto tag=static_cast<unsigned>(memory.load(GuestAddress{details+8},32)),format_index=static_cast<unsigned>(memory.load(GuestAddress{details+4},32));
        const auto pwfx=read_pointer(details+16);const auto cbwfx=static_cast<unsigned>(memory.load(GuestAddress{details+16+pointer},32));if(!pwfx)throw AcmError{11};
        const auto list=tag_list(only);std::optional<unsigned> driver;const auto wanted=mode==0?tag:static_cast<unsigned>(memory.load(GuestAddress{pwfx},16));
        for(const auto& [index,candidate]:list)if(candidate==wanted){driver=index;break;}if(!driver)throw AcmError{512};
        std::u16string text;unsigned size=cbwfx;
        if(mode==0){const auto items=standards(tag);if(format_index>=items.size())throw AcmError{512};const auto& item=items[format_index];if(cbwfx<item.bytes.size())throw AcmError{11};
            memory.write(GuestAddress{pwfx},item.bytes);size=static_cast<unsigned>(item.bytes.size());text=item.text;}
        else{const auto format=read_format(memory,pwfx);text=describe(format.tag,format.channels,format.rate,format.bits,format.average);size=std::max(size,format_size(format.tag));}
        auto bytes=format_details(format_index,wanted,support_of(*driver),pwfx,size,text);write_struct(details,std::move(bytes));arg.return_to(state,0);return {};
    }
    if(name.starts_with("acmFormatTagEnum")){
        // (had, paftd, fnCallback, dwInstance, fdwEnum)
        if(d(4))throw AcmError{10};const auto only=opened(arg(0));if(!arg(1) || memory.load(GuestAddress{arg(1)},32)<24)throw AcmError{11};
        std::vector<std::pair<unsigned,std::vector<std::uint8_t>>> items;
        for(const auto& [index,tag]:tag_list(only)){unsigned tag_index=0;for(unsigned i=0;i<catalog[index].tag_count;++i)if(catalog[index].tags[i]==tag)tag_index=i;items.push_back({index,tag_details(index,tag,tag_index)});}
        enumerate(arg(2),std::move(items),arg(3),true);return {};
    }
    if(name.starts_with("acmFormatEnum")){
        // (had, pafd, fnCallback, dwInstance, fdwEnum)：WFORMATTAG／NCHANNELS／NSAMPLESPERSEC／WBITSPERSAMPLE 依 pwfx 篩選；CONVERT 列出可轉成的 PCM／float，
        // SUGGEST 列出同聲道與取樣率的 16 位元 PCM；HARDWARE（及 INPUT／OUTPUT）沒有硬體驅動程式，不列出。
        const auto flags=d(4);if(flags&~0x1ff0000U)throw AcmError{10};const auto only=opened(arg(0));const auto details=arg(1);
        if(!details || memory.load(GuestAddress{details},32)<format_head)throw AcmError{11};const auto pwfx=read_pointer(details+16);
        if((flags&0x3f0000U) && !pwfx)throw AcmError{11};
        const auto field=[&](unsigned at,std::uint8_t bits){return static_cast<unsigned>(memory.load(GuestAddress{pwfx+at},bits));};
        std::vector<std::pair<unsigned,std::vector<std::uint8_t>>> items;std::vector<std::vector<std::uint8_t>> formats;
        if(!(flags&0x1c00000U)){
            std::optional<Format> source;if(flags&0x300000U)source=read_format(memory,pwfx);
            for(const auto& [index,tag]:tag_list(only)){
                if(source && tag!=1 && tag!=3)continue;
                const auto items_for_tag=standards(tag);
                for(std::size_t i=0;i<items_for_tag.size();++i){
                    const auto& item=items_for_tag[i];const auto value=[&](unsigned at,unsigned size){unsigned v=0;for(unsigned k=0;k<size;++k)v|=unsigned{item.bytes[at+k]}<<(8*k);return v;};
                    if((flags&0x10000U) && value(0,2)!=field(0,16))continue;
                    if((flags&0x20000U) && value(2,2)!=field(2,16))continue;if((flags&0x40000U) && value(4,4)!=field(4,32))continue;if((flags&0x80000U) && value(14,2)!=field(14,16))continue;
                    if((flags&0x200000U) && (tag!=1 || value(14,2)!=16 || value(2,2)!=source->channels || value(4,4)!=source->rate))continue;
                    items.push_back({index,format_details(static_cast<unsigned>(i),tag,support_of(index),0,0,item.text)});formats.push_back(item.bytes);
                }
            }
        }
        enumerate(arg(2),std::move(items),arg(3),true,std::move(formats));return {};
    }
    if(name.starts_with("acmFilterTagDetails") || name.starts_with("acmFilterDetails")){
        // 內建驅動程式沒有濾波器：檢查參數後回報 ACMERR_NOTPOSSIBLE。
        const bool tag=name.starts_with("acmFilterTag");if(d(2)>(tag?2U:1U))throw AcmError{10};(void)opened(arg(0));
        if(!arg(1) || memory.load(GuestAddress{arg(1)},32)<(tag?24U:16+pointer+4))throw AcmError{11};throw AcmError{512};
    }
    if(name.starts_with("acmFilterTagEnum") || name.starts_with("acmFilterEnum")){
        const bool tag=name.starts_with("acmFilterTagEnum");if(tag?d(4)!=0:(d(4)&~0x10000U)!=0)throw AcmError{10};(void)opened(arg(0));
        if(!arg(1) || memory.load(GuestAddress{arg(1)},32)<(tag?24U:16+pointer+4))throw AcmError{11};
        enumerate(arg(2),{},arg(3),true);return {};
    }
}

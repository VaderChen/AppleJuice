namespace resource_edit {
struct Invalid {};
struct Image {
    std::vector<std::uint8_t> bytes;std::size_t pe{},optional{},directories{},sections{};unsigned count{},file_alignment{},section_alignment{};
    std::uint32_t get(std::size_t at,unsigned n=4)const {if(at>bytes.size() || n>bytes.size()-at)throw Invalid{};std::uint32_t value=0;for(unsigned i=0;i<n;++i)value|=std::uint32_t{bytes[at+i]}<<(i*8);return value;}
    void put(std::size_t at,std::uint64_t value,unsigned n=4){if(at>bytes.size() || n>bytes.size()-at)throw Invalid{};for(unsigned i=0;i<n;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));}
    static std::uint32_t aligned(std::uint64_t n,unsigned a){if(!a || (a&(a-1)) || n>UINT32_MAX-a)throw Invalid{};return static_cast<std::uint32_t>((n+a-1)&~std::uint64_t(a-1));}
    explicit Image(std::vector<std::uint8_t> input):bytes(std::move(input)){
        if(get(0,2)!=0x5a4d)throw Invalid{};pe=get(60);if(get(pe)!=0x4550)throw Invalid{};
        count=get(pe+6,2);optional=pe+24;const auto size=get(pe+20,2),magic=get(optional,2);if(magic!=0x10b && magic!=0x20b)throw Invalid{};
        directories=optional+(magic==0x10b?96:112);sections=optional+size;
        if(size<(magic==0x10b?96:112)+24 || get(directories-4)<3 || !count || count>95 || sections+count*40>bytes.size())throw Invalid{};
        file_alignment=get(optional+36);section_alignment=get(optional+32);aligned(0,file_alignment);aligned(0,section_alignment);
        for(unsigned i=0;i<count;++i){const auto h=sections+i*40;const auto raw=get(h+20),length=get(h+16);if(raw>bytes.size() || length>bytes.size()-raw)throw Invalid{};}
    }
    std::size_t offset(std::uint32_t rva,std::uint32_t length)const {
        if(std::uint64_t(rva)+length<=get(optional+60) && std::uint64_t(rva)+length<=bytes.size())return rva;
        for(unsigned i=0;i<count;++i){const auto h=sections+i*40;const auto base=get(h+12),size=get(h+16),raw=get(h+20);if(rva>=base && rva-base<=size && length<=size-(rva-base))return raw+rva-base;}throw Invalid{};
    }
    template<class Resources>void read(Resources& out)const {
        const auto rva=get(directories+16),size=get(directories+20);if(!rva || !size)return;
        const auto at=[&](std::uint32_t relative,unsigned n){if(relative>size || n>size-relative)throw Invalid{};return offset(static_cast<std::uint32_t>(rva+relative),n);};
        using Name=std::variant<std::u16string,std::uint16_t>;std::vector<Name> path;std::set<unsigned> visited;
        std::function<void(unsigned)> walk=[&](unsigned relative){
            if(path.size()>2 || !visited.insert(relative).second)throw Invalid{};
            const auto directory=at(relative,16);const auto total=get(directory+12,2)+get(directory+14,2);at(relative,16+total*8);
            for(unsigned i=0;i<total;++i){const auto entry=directory+16+i*8;const auto key=static_cast<unsigned>(get(entry)),target=static_cast<unsigned>(get(entry+4));Name name;
                if(key&0x80000000U){const auto text=at(key&0x7fffffffU,2);const auto length=get(text,2);at(key&0x7fffffffU,2+length*2);std::u16string value;for(unsigned j=0;j<length;++j)value+=static_cast<char16_t>(get(text+2+j*2,2));name=std::move(value);}
                else {if(key>65535)throw Invalid{};name=static_cast<std::uint16_t>(key);}
                path.push_back(name);
                if(target&0x80000000U)walk(target&0x7fffffffU);
                else {if(path.size()!=3 || !std::holds_alternative<std::uint16_t>(name))throw Invalid{};const auto data=at(target,16);const auto length=get(data+4);const auto content=offset(static_cast<std::uint32_t>(get(data)),static_cast<std::uint32_t>(length));
                    typename Resources::mapped_type value;value.codepage=get(data+8);value.bytes.assign(bytes.data()+content,bytes.data()+content+length);out[{path[0],path[1],std::get<std::uint16_t>(name)}]=std::move(value);}
                path.pop_back();
            }
        };walk(0);
    }
    template<class Resources>std::vector<std::uint8_t> encode(const Resources& resources,std::uint32_t rva){
        using Name=std::variant<std::u16string,std::uint16_t>;struct Node {std::map<Name,Node> children;const typename Resources::mapped_type* value{};std::size_t offset{};};Node root;
        for(const auto& [key,value]:resources)root.children[std::get<0>(key)].children[std::get<1>(key)].children[std::get<2>(key)].value=&value;
        std::vector<std::uint8_t> out;const auto put=[&](std::size_t at,std::uint64_t value,unsigned n=4){for(unsigned i=0;i<n;++i)out.at(at+i)=static_cast<std::uint8_t>(value>>(8*i));};
        std::function<void(Node&)> allocate=[&](Node& node){node.offset=out.size();out.resize(out.size()+(node.value?16:16+node.children.size()*8));for(auto& [name,child]:node.children){(void)name;allocate(child);}};allocate(root);
        std::function<void(const Node&)> emit=[&](const Node& node){
            if(node.value){out.resize(aligned(out.size(),4));put(node.offset,std::uint64_t(rva)+out.size());put(node.offset+4,node.value->bytes.size());put(node.offset+8,node.value->codepage);out.insert(out.end(),node.value->bytes.begin(),node.value->bytes.end());return;}
            const auto names=static_cast<std::size_t>(std::count_if(node.children.begin(),node.children.end(),[](const auto& item){return std::holds_alternative<std::u16string>(item.first);}));if(names>65535 || node.children.size()-names>65535)throw Invalid{};
            put(node.offset+12,names,2);put(node.offset+14,node.children.size()-names,2);unsigned i=0;
            for(const auto& [name,child]:node.children){const auto entry=node.offset+16+i++*8;
                if(const auto* text=std::get_if<std::u16string>(&name)){out.resize(aligned(out.size(),2));const auto pos=out.size();put(entry,0x80000000U|pos);out.resize(pos+2+text->size()*2);put(pos,text->size(),2);for(unsigned n=0;n<text->size();++n)put(pos+2+n*2,(*text)[n],2);}
                else put(entry,std::get<std::uint16_t>(name));
                put(entry+4,child.offset|(child.value?0U:0x80000000U));emit(child);
            }
        };emit(root);return out;
    }
    template<class Resources>void update(const Resources& resources){
        // 後續提交重用自己的資源區段，避免每次更新都增加 PE section 並最終超過 loader 上限。
        const auto last=sections+(count-1)*40;
        if(std::equal(bytes.data()+last,bytes.data()+last+8,".rsrcAJ") && get(last+12)==get(directories+16)){
            const auto raw=get(last+20),old_size=get(last+16),base=get(last+12);auto payload=encode(resources,base);const auto size=aligned(payload.size(),file_alignment);
            bytes.erase(bytes.begin()+raw,bytes.begin()+raw+old_size);bytes.insert(bytes.begin()+raw,size,0);std::copy(payload.begin(),payload.end(),bytes.begin()+raw);
            if(get(pe+12)>=std::uint64_t(raw)+old_size && get(pe+12))put(pe+12,get(pe+12)-old_size+size);
            put(last+8,payload.size());put(last+16,size);put(optional+8,get(optional+8)-old_size+size);put(optional+56,aligned(std::uint64_t(base)+payload.size(),section_alignment));put(directories+20,payload.size());checksum();return;
        }
        std::uint32_t first=static_cast<std::uint32_t>(bytes.size()),end=0,rva=0;
        for(unsigned i=0;i<count;++i){const auto h=sections+i*40;const auto raw=get(h+20),size=get(h+16);if(size){first=std::min(first,static_cast<std::uint32_t>(raw));end=std::max(end,static_cast<std::uint32_t>(raw+size));}rva=std::max(rva,aligned(get(h+12)+std::max(get(h+8),size),section_alignment));}
        const auto header_end=sections+(count+1)*40;const auto extra=header_end>first?aligned(header_end-first,file_alignment):0;
        const auto debug_rva=get(directories-4)>6?get(directories+48):0,debug_size=get(directories-4)>6?get(directories+52):0;
        std::vector<std::size_t> debug;
        if(debug_rva)for(unsigned at=0;at+28<=debug_size;at+=28)debug.push_back(offset(debug_rva+at,28)+24);
        if(extra){
            bytes.insert(bytes.begin()+first,extra,0);
            for(unsigned i=0;i<count;++i){const auto h=sections+i*40;for(const auto field:{20U,24U,28U})if(get(h+field)>=first && get(h+field))put(h+field,get(h+field)+extra);}
            if(get(pe+12)>=first && get(pe+12))put(pe+12,get(pe+12)+extra);
            for(auto at:debug){if(at>=first)at+=extra;if(get(at)>=first && get(at))put(at,get(at)+extra);}
            end+=extra;put(optional+60,aligned(std::max<std::uint64_t>(get(optional+60)+extra,header_end),file_alignment));
        }
        auto payload=encode(resources,rva);const auto raw=aligned(end,file_alignment),raw_size=aligned(payload.size(),file_alignment);
        const auto inserted=raw-end+raw_size;bytes.insert(bytes.begin()+end,inserted,0);std::copy(payload.begin(),payload.end(),bytes.begin()+raw);
        if(get(pe+12)>=end && get(pe+12))put(pe+12,get(pe+12)+inserted);
        const auto h=sections+count*40;std::fill_n(bytes.data()+h,40,0);const std::string name=".rsrcAJ";std::copy(name.begin(),name.end(),bytes.data()+h);
        put(h+8,payload.size());put(h+12,rva);put(h+16,raw_size);put(h+20,raw);put(h+36,0x40000040);put(pe+6,count+1,2);
        put(optional+8,get(optional+8)+raw_size);put(optional+56,aligned(std::uint64_t(rva)+payload.size(),section_alignment));
        put(directories+16,rva);put(directories+20,payload.size());
        checksum();
    }
    void checksum(){
        if(get(directories-4)>4){put(directories+32,0);put(directories+36,0);} // 修改後原 Authenticode 已失效；保留 overlay 本身。
        put(optional+64,0);std::uint64_t checksum=0;for(std::size_t i=0;i<bytes.size();i+=2){checksum+=bytes[i]|(i+1<bytes.size()?unsigned{bytes[i+1]}<<8:0);checksum=(checksum&65535)+(checksum>>16);}checksum=(checksum&65535)+(checksum>>16);put(optional+64,checksum+bytes.size());
    }
};
}
std::optional<std::uint32_t> WindowsServices::invoke_resource_update(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;const bool wide=symbol.ends_with('W');
    const auto finish=[&](std::uint64_t value,std::uint32_t error=0){if(error)last_error_=error;arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto text=[&](std::uint64_t at){return wide?read_utf16(memory,GuestAddress{at},32767):utf8_to_utf16_lossy(read_ansi(memory,at,32767));};
    try{
        if(symbol.starts_with("Begin")){
            if(!arg(0))return finish(0,87);if(resource_updates_.size()>=128)return finish(0,8);refresh_file_security_context();
            ResourceUpdate edit;edit.path=utf16_to_utf8(text(arg(0)));auto opened=filesystem_.open_host(edit.path,0xc0000000U,3,0,7);if(opened.error)return finish(0,opened.error);
            if(opened.metadata.attributes&16)return finish(0,5);if(opened.metadata.size>256U*1024U*1024U)return finish(0,8);
            edit.file=opened.file;edit.original.resize(opened.metadata.size);const auto read=edit.file->read(edit.original,0);if(read.error || read.bytes!=edit.original.size())return finish(0,read.error?read.error:38);
            resource_edit::Image image(edit.original);if(!static_cast<std::uint32_t>(arg(1)))image.read(edit.resources);
            const auto id=runtime_.allocate_wait_handle();resource_updates_.emplace(id,std::move(edit));return finish(id);
        }
        const auto found=resource_updates_.find(arg(0));if(found==resource_updates_.end())return finish(0,6);auto& edit=found->second;
        if(symbol.starts_with("Update")){
            const auto name=[&](std::uint64_t at)->ResourceUpdateName{if(at<=65535)return static_cast<std::uint16_t>(at);auto value=text(at);if(value.empty() || value.size()>65535)throw resource_edit::Invalid{};
                if(value.front()==u'#'){unsigned id=0;if(value.size()==1)throw resource_edit::Invalid{};for(auto c:value.substr(1)){if(c<u'0' || c>u'9' || id>6553)throw resource_edit::Invalid{};id=id*10+(c-u'0');}if(id>65535)throw resource_edit::Invalid{};return static_cast<std::uint16_t>(id);}return value;};
            const auto key=std::tuple{name(arg(1)),name(arg(2)),static_cast<std::uint16_t>(arg(3))};const auto count=static_cast<std::uint32_t>(arg(5));
            if(!arg(4)){if(count)return finish(0,87);edit.resources.erase(key);return finish(1);}
            if(count>64U*1024U*1024U)return finish(0,8);ResourceUpdateValue value;value.bytes.resize(count);memory.read(GuestAddress{arg(4)},value.bytes);edit.resources.insert_or_assign(key,std::move(value));return finish(1);
        }
        if(static_cast<std::uint32_t>(arg(1))){resource_updates_.erase(found);return finish(1);}
        resource_edit::Image image(edit.original);image.update(edit.resources);
        if(edit.file->current_size()!=edit.original.size())return finish(0,32);std::vector<std::uint8_t> current(edit.original.size());const auto read=edit.file->read(current,0);if(read.error || read.bytes!=current.size())return finish(0,read.error?read.error:38);if(current!=edit.original)return finish(0,32);
        const auto temporary=edit.path+".resource-update-"+std::to_string(arg(0))+".tmp";
        const auto info=edit.file->information();if(info.error)return finish(0,info.error);
        const auto security=security::relative_descriptor(security::file_descriptor(security::read_native_file_security(*edit.file,true),7));
        auto output=filesystem_.open_host(temporary,0x40000000U,1,info.metadata.attributes,7,[&]{return security;},[&]{return OpenHostFile{info.key,edit.file,info.metadata,0,true};});if(output.error)return finish(0,output.error);
        auto result=output.file->write(image.bytes,0);std::uint32_t error=result.error?result.error:result.bytes!=image.bytes.size()?112U:output.file->set_times(info.metadata.created,info.metadata.accessed,0);
        if(!error)error=output.file->flush();output.file.reset();
        if(!error)error=filesystem_.rename(temporary,edit.path,true);if(error){(void)filesystem_.remove(temporary,false);return finish(0,error);}
        resource_updates_.erase(found);return finish(1);
    }catch(const MemoryFault&){return finish(0,998);}catch(const SecurityDescriptorError& error){return finish(0,error.code);}catch(const resource_edit::Invalid&){return finish(0,service.symbol.starts_with("Update")?87:193);}catch(const std::bad_alloc&){return finish(0,8);}
}

// 屬性儲存沿用客體提供的 IStorage／IStream；內建與應用程式自訂物件共用同一路徑。
if (service.family.starts_with("internal-prop-") || service.symbol=="StgCreatePropSetStg" ||
    service.symbol=="StgCreatePropStg" || service.symbol=="StgOpenPropStg") {
    return nested_continuation(state,memory,"屬性儲存",[this,service](GuestState& s,GuestMemory& m)
        -> service_callback::Task<std::optional<std::uint32_t>> {
        const WindowsCall arg(s,m,service);
        const auto done=[&](std::uint64_t result){arg.return_to(s,result);return std::optional<std::uint32_t>{};};
        const unsigned unit=x86_?4:8;
        const auto bits=static_cast<std::uint8_t>(8*unit);
        const auto name=service.symbol;
        const auto ptr=[&](std::uint64_t at){return m.load(GuestAddress{at},bits);};
        const auto guid=[&](std::uint64_t at){PropertyGuid id{};m.read(GuestAddress{at},id);return id;};
        const auto method=[&](std::uint64_t obj,unsigned slot,std::vector<std::uint64_t> args){
            args.insert(args.begin(),obj);
            return service_callback::call(storage::method(m,obj,slot),std::move(args));
        };
        GuestScratch scratch(*this,m,s);
        const auto temp=[&](size_t n){auto p=scratch.allocate(n);if(!p)throw std::bad_alloc{};return p;};
        const auto create=[&](StorageState::PropertyObject object,std::uint64_t output){
            auto& bridge=storage_bridge();
            constexpr std::array families{"internal-prop-storage","internal-prop-set","internal-prop-enum","internal-prop-set-enum"};
            const unsigned kind=(object.enumerator?2U:0U)+(object.collection?1U:0U);
            auto& table=bridge.property_tables[kind];
            if (!table) {
                std::vector<std::uint64_t> targets;
                const std::string_view family=families[kind];
                for(const auto& entry:storage_service_descriptors())
                    if(entry.family==family) targets.push_back(resolve(family,entry.symbol).value);
                const auto address=runtime_allocate(m,{},unit*targets.size());
                for(size_t i=0;i<targets.size();++i)m.store(GuestAddress{address+i*unit},bits,targets[i]);
                table=address;
            }
            const auto handle=allocate_heap(m,unit);
            if(!handle)throw std::bad_alloc{};
            try {
                m.store(GuestAddress{handle},bits,table);
                bridge.properties.emplace(handle,std::move(object));
                m.store(GuestAddress{output},bits,handle);
            } catch(...) {bridge.properties.erase(handle);(void)free_heap(m,handle,s);throw;}
        };
        const auto seek_start=[&](std::uint64_t stream){
            return method(stream,5,x86_?std::vector<std::uint64_t>{0,0,0,0}:std::vector<std::uint64_t>{0,0,0});
        };
        const auto load_stream=[&](std::uint64_t stream,property_set::Stream& result)->service_callback::Task<std::uint32_t>{
            const auto stat=temp(x86_?72:80);
            if(auto hr=co_await method(stream,12,{stat,1}))co_return hr;
            const auto length=m.load(GuestAddress{stat+(x86_?8U:16U)},64);
            if(length<48 || length>UINT32_MAX)co_return storage::invalid_header;
            if(auto hr=co_await seek_start(stream))co_return hr;
            const auto buffer=temp(65536),actual=temp(4);
            property_set::Bytes bytes;
            while(bytes.size()<length){
                const auto n=std::min<std::uint64_t>(65536,length-bytes.size());
                auto hr=co_await method(stream,3,{buffer,n,actual});
                if(hr>>31)co_return hr;
                if(m.load(GuestAddress{actual},32)!=n)co_return storage::read_fault;
                const auto offset=bytes.size();bytes.resize(offset+n);
                m.read(GuestAddress{buffer},std::span(bytes).subspan(offset));
            }
            try{result=property_set::decode(bytes);}catch(const property_set::Invalid&){co_return storage::invalid_header;}
            co_return 0;
        };
        const auto save_stream=[&](const StorageState::PropertyObject& object,unsigned flags)->service_callback::Task<std::uint32_t>{
            const auto stream=object.backend;
            auto bytes=property_set::encode(object.stream);
            const auto buffer=temp(bytes.size()),actual=temp(4);
            m.write(GuestAddress{buffer},bytes);
            if(auto hr=co_await seek_start(stream))co_return hr;
            if(auto hr=co_await method(stream,4,{buffer,bytes.size(),actual}))co_return hr;
            if(m.load(GuestAddress{actual},32)!=bytes.size())co_return storage::write_fault;
            if(auto hr=co_await method(stream,6,x86_?std::vector<std::uint64_t>{bytes.size(),0}:std::vector<std::uint64_t>{bytes.size()}))co_return hr;
            if(auto hr=co_await method(stream,8,{flags}))co_return hr;
            if(object.container && (object.mode&storage::transacted))co_return co_await method(object.container,9,{flags});
            co_return 0;
        };
        constexpr std::uint32_t invalid=0x80070057, pointer=0x80004003, no_interface=0x80004002;
        // 承接一份底層介面參照；非簡單屬性集另外持有 CONTENTS 串流。
        const auto attach=[&](std::uint64_t owned,PropertyGuid format,std::uint64_t clsid,unsigned flags,
                              unsigned mode,bool opening,std::uint64_t output)->service_callback::Task<std::uint32_t>{
            StorageState::PropertyObject object;
            object.flags=flags;object.mode=mode;
            object.container=(flags&1)?owned:0;
            object.backend=object.container?0:owned;
            std::uint32_t status=0;
            std::exception_ptr failure;
            try{
                if(object.container){
                    const auto slot=temp(unit),contents=scratch.text(u"CONTENTS");
                    const auto stream_mode=(mode&3U)|0x10U;
                    status=opening?co_await method(owned,4,{contents,0,stream_mode,0,slot}):
                                   co_await method(owned,3,{contents,stream_mode|storage::create,0,0,slot});
                    if(!status)object.backend=ptr(slot);
                    if(!status && !opening && clsid)status=co_await method(owned,15,{clsid});
                }
                if(!status && opening){
                    status=co_await load_stream(object.backend,object.stream);
                    const auto& sections=object.stream.sections;
                    const auto it=std::find_if(sections.begin(),sections.end(),[&](const auto& section){return section.format==format;});
                    if(!status && it==sections.end())status=storage::missing;
                    object.section=static_cast<size_t>(it-sections.begin());
                }else if(!status){
                    if(clsid)object.stream.clsid=guid(clsid);
                    object.stream.sections.push_back({format,{}});
                    const unsigned page=(flags&2)?65001:1200;
                    object.stream.sections.front().properties[1]={2,0,0,0,static_cast<std::uint8_t>(page),
                        static_cast<std::uint8_t>(page>>8),0,0};
                    if(flags&8){
                        object.stream.version=1;
                        object.stream.sections.front().properties[0x80000003]={19,0,0,0,1,0,0,0};
                    }
                    object.dirty=true;
                }
                if(!status)create(std::move(object),output);
            }catch(...){failure=std::current_exception();}
            if(status || failure){
                if(object.backend)(void)co_await method(object.backend,2,{});
                if(object.container)(void)co_await method(object.container,2,{});
            }
            if(failure)std::rethrow_exception(failure);
            co_return status;
        };
        try {
            if(service.family=="ole32.dll"){
                const bool collection=name=="StgCreatePropSetStg", opening=name=="StgOpenPropStg";
                const auto output=arg(collection?2:opening?4:5);
                if(!output)co_return done(pointer);
                m.store(GuestAddress{output},bits,0);
                if(!arg(0) || arg(collection?1:opening?3:4) || (!collection && !arg(1)))co_return done(invalid);
                const unsigned flags=collection?0:static_cast<unsigned>(arg(opening?2:3));
                const auto format=collection?PropertyGuid{}:guid(arg(1));
                PropertyGuid iid{static_cast<std::uint8_t>(collection || (flags&1)?11:12),0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
                const auto slot=temp(unit),id=temp(16);m.write(GuestAddress{id},iid);
                if(auto hr=co_await method(arg(0),0,{id,slot}))co_return done(hr);
                const auto owned=ptr(slot);
                if(!collection){
                    unsigned mode=0x12;
                    if(flags&1){
                        const auto stat=temp(x86_?72:80);
                        if(auto hr=co_await method(owned,17,{stat,1})){
                            (void)co_await method(owned,2,{});co_return done(hr);
                        }
                        mode=static_cast<unsigned>(m.load(GuestAddress{stat+(x86_?40U:48U)},32));
                    }
                    co_return done(co_await attach(owned,format,opening?0:arg(2),flags,mode,opening,output));
                }
                StorageState::PropertyObject object;
                object.collection=true;object.backend=owned;
                std::uint32_t status=0;
                try{create(std::move(object),output);}catch(const std::bad_alloc&){status=storage::no_memory;}
                if(status)(void)co_await method(owned,2,{});
                co_return done(status);
            }
            const auto handle=arg(0);
            auto& entries=storage_bridge().properties;
            auto it=entries.find(handle);
            if(it==entries.end())co_return done(pointer);
            auto& object=it->second;
            if(name=="AddRef")co_return done(++object.references);
            if(name=="Release"){
                if(--object.references)co_return done(object.references);
                auto owned=std::move(object);entries.erase(it);(void)free_heap(m,handle,s);
                if(owned.dirty)(void)co_await save_stream(owned,0);
                if(owned.backend)(void)co_await method(owned.backend,2,{});
                if(owned.container)(void)co_await method(owned.container,2,{});
                co_return done(0);
            }
            if(name=="QueryInterface"){
                if(!arg(1)||!arg(2))co_return done(pointer);
                m.store(GuestAddress{arg(2)},bits,0);
                auto id=guid(arg(1));
                PropertyGuid unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
                auto expected=unknown;expected[0]=static_cast<std::uint8_t>((object.collection?0x3a:0x38)+(object.enumerator?1:0));expected[1]=1;
                if(id!=unknown && id!=expected)co_return done(no_interface);
                ++object.references;m.store(GuestAddress{arg(2)},bits,handle);co_return done(0);
            }
#include "windows_property_enumeration.inc"
            if(object.collection && (name=="Create" || name=="Open" || name=="Delete")){
                if(!arg(1))co_return done(invalid);
                auto text=scratch.text(property_set_name(guid(arg(1))));
                if(name=="Delete")co_return done(co_await method(object.backend,12,{text}));
                bool opening=name=="Open";
                const auto output=arg(opening?3:5),mode=arg(opening?2:4);
                if(!output)co_return done(pointer);
                m.store(GuestAddress{output},bits,0);
                const auto slot=temp(unit);
                const auto fmt=guid(arg(1));
                auto flags=opening?0U:static_cast<unsigned>(arg(3));
                std::uint32_t hr=0;
                if(opening){
                    hr=co_await method(object.backend,4,{text,0,mode&~storage::transacted,0,slot});
                    if(hr==storage::missing){
                        flags|=1;
                        hr=co_await method(object.backend,6,{text,0,mode,0,0,slot});
                    }
                }else hr=co_await method(object.backend,(flags&1)?5:3,{text,mode,0,0,slot});
                if(hr)co_return done(hr);
                co_return done(co_await attach(ptr(slot),fmt,opening?0:arg(2),flags,static_cast<unsigned>(mode),opening,output));
            }
            if(object.collection)co_return done(storage::invalid_function);
            auto& section=object.stream.sections.at(object.section);
            if(name=="Revert"){
                if(object.container && (object.mode&storage::transacted)){
                    if(auto hr=co_await method(object.container,10,{}))co_return done(hr);
                    const auto slot=temp(unit),contents=scratch.text(u"CONTENTS");
                    if(auto hr=co_await method(object.container,4,{contents,0,(object.mode&3U)|0x10U,0,slot}))co_return done(hr);
                    const auto previous=std::exchange(object.backend,ptr(slot));
                    (void)co_await method(previous,2,{});
                }
                property_set::Stream loaded;
                if(auto hr=co_await load_stream(object.backend,loaded))co_return done(hr);
                auto found=std::find_if(loaded.sections.begin(),loaded.sections.end(),[&](const auto& other){return other.format==section.format;});
                if(found==loaded.sections.end())co_return done(storage::missing);
                object.section=static_cast<size_t>(found-loaded.sections.begin());object.stream=std::move(loaded);object.dirty=false;co_return done(0);
            }
            if(name=="Commit"){
                if(!object.dirty){
                    if(object.container && (object.mode&storage::transacted))
                        co_return done(co_await method(object.container,9,{arg(1)}));
                    co_return done(0);
                }
                const auto hr=co_await save_stream(object,static_cast<unsigned>(arg(1)));
                if(!hr)object.dirty=false;co_return done(hr);
            }
            const auto behavior=section.properties.find(0x80000003);
            const bool sensitive=behavior!=section.properties.end() && property_set::read(behavior->second,4)!=0;
            if(name=="Stat"){
                if(!arg(1))co_return done(pointer);
                m.fill(GuestAddress{arg(1)},0,64);m.write(GuestAddress{arg(1)},section.format);
                m.write(GuestAddress{arg(1)+16},object.stream.clsid);const auto flags=(object.flags&~10U) | (sensitive?8U:0U) | (property_set::dictionary_page(section)==1200?0U:2U);
                m.store(GuestAddress{arg(1)+32},32,flags);
                m.store(GuestAddress{arg(1)+60},32,object.stream.system);
                if(object.container){
                    const auto stat=temp(x86_?72:80);
                    if(auto hr=co_await method(object.container,17,{stat,1}))co_return done(hr);
                    std::array<std::uint8_t,24> times{};
                    m.read(GuestAddress{stat+(x86_?16U:24U)},times);m.write(GuestAddress{arg(1)+36},times);
                }
                co_return done(0);
            }
            if(!storage::writable(object.mode) && name!="ReadMultiple" && name!="ReadPropertyNames" && name!="Enum")
                co_return done(storage::access_denied);
            if(name=="SetTimes"){
                if(!object.container)co_return done(storage::invalid_function);
                co_return done(co_await method(object.container,14,{0,arg(1),arg(2),arg(3)}));
            }
            if(name=="SetClass"){
                if(!arg(1))co_return done(pointer);
                if(object.container)if(auto hr=co_await method(object.container,15,{arg(1)}))co_return done(hr);
                object.stream.clsid=guid(arg(1));object.dirty=true;co_return done(0);
            }
            const auto store_names=[&](const property_set::Names& names,unsigned page){
                auto version=object.stream.version;
                auto encoded=property_set::encode_names(names,page,version);
                if(!encoded)return false;
                section.properties.insert_or_assign(0,std::move(*encoded));
                object.stream.version=version;
                object.dirty=true;
                return true;
            };
            if(name=="ReadPropertyNames" || name=="WritePropertyNames" || name=="DeletePropertyNames"){
                const bool reading=name=="ReadPropertyNames",deleting=name=="DeletePropertyNames";
                const auto count=static_cast<unsigned>(arg(1));
                if(count && (!arg(2) || (!deleting && !arg(3))))co_return done(invalid);
                auto names=property_set::decode_names(section);
                std::uint32_t status=reading?1:0;
                if(reading && count)m.fill(GuestAddress{arg(3)},0,std::uint64_t{count}*unit);
                for(unsigned i=0;i<count;++i){
                    auto id=static_cast<std::uint32_t>(m.load(GuestAddress{arg(2)+4ULL*i},32));
                    if(deleting){names.erase(id);continue;}
                    const auto slot=arg(3)+std::uint64_t{i}*unit;
                    if(reading){
                        auto it_name=names.find(id);
                        if(it_name==names.end())continue;
                        auto data=utf16_bytes(it_name->second);
                        auto address=allocate_heap(m,data.size());
                        if(!address)co_return done(storage::no_memory);
                        m.write(GuestAddress{address},data);
                        gui_.task_allocations.insert(address);m.store(GuestAddress{slot},bits,address);status=0;
                    }else if(id!=UINT32_MAX){
                        const auto address=ptr(slot);
                        if(!address)co_return done(invalid);
                        auto text=read_utf16(m,GuestAddress{address});
                        auto existing=property_set::find_name(names,text,sensitive);
                        if(existing!=names.end() && existing->first!=id)names.erase(existing);
                        names.insert_or_assign(id,std::move(text));
                    }
                }
                if(!reading && count && !store_names(names,property_set::dictionary_page(section)))co_return done(invalid);
                if(!reading && count && (object.flags&4)){
                    auto hr=co_await save_stream(object,0);
                    if(hr)co_return done(hr);
                    object.dirty=false;
                }
                co_return done(status);
            }
            if(name=="ReadMultiple" || name=="WriteMultiple" || name=="DeleteMultiple"){
                bool reading=name=="ReadMultiple",deleting=name=="DeleteMultiple";
                const auto property_count=static_cast<unsigned>(arg(1)),stride=x86_?16U:24U;
                if(property_count && (!arg(2) || (!deleting && !arg(3))))co_return done(pointer);
#include "windows_property_indirect.inc"
#include "windows_property_vector.inc"
                const auto operate=[&]()->service_callback::Task<std::uint32_t>{
                auto& values=section.properties;
                std::map<std::uint32_t,property_set::Bytes> updates;
                std::map<std::uint32_t,std::uint64_t> sources;
                std::optional<property_set::Names> names;
                bool names_changed=false,version_one=false;
                constexpr std::uint32_t first_readonly=0x80000000;
                std::uint32_t status=reading?1:0;
                for(unsigned i=0;i<property_count;++i){
                    auto spec=arg(2)+2ULL*unit*i;
                    auto at=deleting?0:arg(3)+std::uint64_t{i}*stride;
                    if(reading)m.fill(GuestAddress{at},0,stride);
                    const auto kind=m.load(GuestAddress{spec},32);
                    std::uint32_t id=0;
                    if(kind==1){
                        id=static_cast<std::uint32_t>(m.load(GuestAddress{spec+unit},32));
                        if(!id){if(reading)continue;co_return storage::invalid_parameter;}
                        if(!reading && id==UINT32_MAX)continue;
                        if(!reading && id==1){
                            const auto has_values=[&](const auto& table){
                                auto first=table.lower_bound(2);
                                return first!=table.end() && first->first<first_readonly;
                            };
                            if(deleting || has_values(values) || has_values(updates) || m.load(GuestAddress{at},16)!=2)
                                co_return storage::invalid_parameter;
                            if(values.contains(0)){
                                if(!names)names=property_set::decode_names(section);
                                names_changed=true;
                            }
                        }
                        if(!reading && id>=first_readonly)co_return storage::invalid_parameter;
                    }else if(kind==0){
                        const auto address=ptr(spec+unit);
                        if(!address)co_return invalid;
                        if(!names)names=property_set::decode_names(section);
                        auto key=read_utf16(m,GuestAddress{address});
                        auto found=property_set::find_name(*names,key,sensitive);
                        if(found!=names->end())id=found->first;
                        else{
                            if(reading || deleting)continue;
                            id=static_cast<std::uint32_t>(arg(4));
                            if(id<2 || id>=first_readonly)co_return storage::invalid_parameter;
                            const auto advance=[&](const auto& table){
                                auto end=table.lower_bound(first_readonly);
                                if(end!=table.begin())id=std::max(id,std::prev(end)->first+1);
                            };
                            advance(values);advance(updates);advance(*names);
                            if(id>=first_readonly)co_return storage::invalid_parameter;
                            names->emplace(id,std::move(key));names_changed=true;
                        }
                    }else co_return invalid;
                    if(deleting){updates[id]={};continue;}
                    if(reading){
                        auto value=values.find(id);
                        if(value==values.end())continue;
                        status=0;
                        const auto& bytes=value->second;
                        auto type=property_set::read(bytes,0);
                        const auto size=property_set::scalar_width(type);
                        if(indirect(type)){
                            if(!object.container)co_return storage::invalid_header;
                            const auto expected=indirect_record(type,id,property_set::dictionary_page(section));
                            const size_t length_at=type==73?20:4;
                            const size_t end=length_at+4+property_set::read(expected,length_at);
                            if(bytes.size()<end || !std::equal(expected.data()+length_at,expected.data()+end,bytes.data()+length_at))
                                co_return storage::invalid_header;
                            const auto label=scratch.text(element_name(id)),slot=temp(unit);
                            const auto mode=(object.mode&3U)|0x10U;
                            const auto hr=storage_value(type)?co_await method(object.container,6,{label,0,mode,0,0,slot}):
                                                              co_await method(object.container,4,{label,0,mode,0,slot});
                            if(hr)co_return hr;
                            auto result=ptr(slot);
                            if(type==73){
                                auto wrapper=allocate_heap(m,16+unit);
                                if(!wrapper){(void)co_await method(result,2,{});co_return storage::no_memory;}
                                m.write(GuestAddress{wrapper},std::span(bytes).subspan(4,16));
                                m.store(GuestAddress{wrapper+16},bits,result);result=wrapper;
                            }
                            m.store(GuestAddress{at+8},bits,result);
                        }else if((type&0xf000U)==0x1000U){
                            unpack_vector(bytes,type,at,property_set::dictionary_page(section));
                        }else if(text_element(type)){
                            size_t pos=4;
                            const auto address=unpack_text(bytes,pos,type,property_set::dictionary_page(section));
                            m.store(GuestAddress{at+8},bits,address);
                        }else if(type==71){
                            const auto clip=allocate_heap(m,8+unit);if(!clip)co_return storage::no_memory;
                            m.fill(GuestAddress{clip},0,8+unit);
                            m.store(GuestAddress{at+8},bits,clip);m.store(GuestAddress{at},16,type);
                            size_t pos=4;unpack_clip(bytes,pos,clip);
                        }else if(size){
                            if(*size>bytes.size()-4)co_return storage::invalid_header;
                            m.write(GuestAddress{at+8},std::span(bytes).subspan(4,*size));
                        }else if(type==65 || type==72){
                            const size_t offset=type==72?4:8;
                            const std::uint64_t count=type==72?16:property_set::read(bytes,4);
                            const auto length=count;
                            if(offset>bytes.size() || length>bytes.size()-offset)co_return storage::invalid_header;
                            const auto payload=std::span(bytes).subspan(offset,static_cast<size_t>(length));
                            const auto address=length?allocate_heap(m,length):0;
                            if(length && !address)co_return storage::no_memory;
                            if(length)m.write(GuestAddress{address},payload);
                            if(type==65)m.store(GuestAddress{at+8},32,count);
                            m.store(GuestAddress{at+8+(type==65?unit:0)},bits,address);
                        }else co_return storage::invalid_function;
                        m.store(GuestAddress{at},16,type);
                    }else{
                        const auto type=static_cast<std::uint16_t>(m.load(GuestAddress{at},16));
                        if(indirect(type)){
                            if(!object.container)co_return storage::invalid_function;
                            const auto page=updates.contains(1)?property_set::read(updates.at(1),4,2):property_set::dictionary_page(section);
                            auto record=indirect_record(type,id,page);
                            auto source=ptr(at+8);
                            if(type==73){
                                if(!source)co_return invalid;
                                m.read(GuestAddress{source},std::span(record).subspan(4,16));source=ptr(source+16);
                            }
                            sources[id]=source;updates[id]=std::move(record);continue;
                        }
                        if((type&0xf000U)==0x1000U || text_element(type)){
                            const auto page=updates.contains(1)?property_set::read(updates.at(1),4,2):property_set::dictionary_page(section);
                            if(text_element(type)){
                                property_set::Bytes record(4);store_dword(record,0,type);
                                append_text(record,type,ptr(at+8),page);updates[id]=std::move(record);
                            }else updates[id]=pack_vector(type,at,page,version_one);
                            continue;
                        }
                        if(type==71){
                            property_set::Bytes record(4);store_dword(record,0,type);
                            append_clip(record,ptr(at+8));updates[id]=std::move(record);continue;
                        }
                        const auto width=property_set::scalar_width(type);
                        property_set::Bytes payload;
                        if(width){
                            payload.resize(*width);
                            m.read(GuestAddress{at+8},payload);
                        }else if(type==65 || type==72){
                            const auto address=ptr(at+8+(type==65?unit:0));
                            const auto length=type==72?16:m.load(GuestAddress{at+8},32);
                            if(length){
                                if(!address)co_return invalid;
                                m.check(GuestAddress{address},length,Access::Read);
                                payload.resize(length);m.read(GuestAddress{address},payload);
                            }
                        }else co_return storage::invalid_function;
                        const bool counted=type==65;
                        const size_t offset=counted?8:4;
                        property_set::Bytes bytes(offset+((payload.size()+3)&~size_t{3}));
                        bytes[0]=static_cast<std::uint8_t>(type);
                        if(counted){
                            auto count=payload.size();
                            for(unsigned j=0;j<4;++j)bytes[4+j]=static_cast<std::uint8_t>(count>>(8*j));
                        }
                        std::copy(payload.begin(),payload.end(),bytes.data()+offset);
                        updates[id]=std::move(bytes);
                    }
                }
                if(names_changed){
                    const auto page=updates.contains(1)?property_set::read(updates.at(1),4,2):property_set::dictionary_page(section);
                    if(!store_names(*names,page))co_return invalid;
                }
                for(auto& [id,bytes]:updates){
                    const auto prev=values.find(id);
                    bool had_old=prev!=values.end() && indirect(property_set::read(prev->second,0));
                    auto type=deleting?0U:property_set::read(bytes,0);
                    if(had_old || indirect(type)){
                        if(!object.container)co_return storage::invalid_header;
                        if(auto hr=co_await replace_element(id,type,indirect(type)?sources.at(id):0,had_old))co_return hr;
                    }
                    if(version_one || type==16 || type==22 || type==23 || type==0x1010 || type==73)object.stream.version=1;
                    if(deleting)values.erase(id);else values.insert_or_assign(id,std::move(bytes));
                    object.dirty=true;
                }
                if(!updates.empty()){
                    object.dirty=true;
                    if(object.flags&4){
                        auto hr=co_await save_stream(object,0);
                        if(hr)co_return hr;
                        object.dirty=false;
                    }
                }
                co_return status;
                };
                std::uint32_t hr=0;
                std::exception_ptr failure;
                if(reading && property_count)m.fill(GuestAddress{arg(3)},0,std::uint64_t{property_count}*stride);
                try{hr=co_await operate();}catch(...){failure=std::current_exception();}
                if(reading && (failure || (hr>>31))){
                    for(unsigned i=0;i<property_count;++i){
                        const auto at=arg(3)+std::uint64_t{i}*stride;
                        if(m.load(GuestAddress{at},16))(void)co_await service_callback::call(resolve("ole32.dll","PropVariantClear").value,{at});
                    }
                }
                if(failure)std::rethrow_exception(failure);
                co_return done(hr);
            }
            co_return done(storage::invalid_function);
        }catch(const storage::Error& error){co_return done(error.status);}
        catch(const property_set::Invalid&){co_return done(storage::invalid_header);}
        catch(const std::bad_alloc&){co_return done(storage::no_memory);}
    });
}

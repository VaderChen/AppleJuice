// 以 sandbox 登錄交易管理虛擬機的硬體設定檔；A/W、跨程序及重啟共用同一 GUID。
const WindowsCall arg(state,memory,service);const bool wide=service.symbol.ends_with('W');
const auto finish=[&](std::uint32_t error){if(error)last_error_=error;arg.return_to(state,error?0:1);return std::optional<std::uint32_t>{};};
if(!arg(0))return finish(87);
try{
    memory.check(GuestAddress{arg(0)},wide?244:124,Access::Write);
    if(!registry_file_.empty() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久硬體設定檔 I/O");
    auto& registry=runtime_.registry;RegistryTransaction transaction(registry,registry_file_,&registry_store_cache_);
    if(transaction.error)return finish(transaction.error);
    struct Rollback {RegistryState& state;RegistryState before;bool committed{};~Rollback(){if(!committed)std::swap(state,before);}} rollback{registry,registry};
    const std::u16string database=u"\x0002\\system\\currentcontrolset\\control\\idconfigdb";
    const auto create_key=[&](const std::u16string& path){for(auto end=path.find(u'\\');;end=path.find(u'\\',end+1)){registry.keys.try_emplace(path.substr(0,end));if(end==path.npos)break;}};
    bool changed=false;
    const auto set=[&](const std::u16string& path,std::u16string name,std::uint32_t type,std::vector<std::uint8_t> bytes){
        auto& values=registry.keys.at(path);auto existing=values.find(name);
        if(existing==values.end())++registry.values;else registry.bytes-=existing->second.bytes.size();registry.bytes+=bytes.size();
        values.insert_or_assign(std::move(name),RegistryValue{type,std::move(bytes)});changed=true;
    };
    if(!registry.keys.contains(database)){
        create_key(database);set(database,u"currentconfig",4,{1,0,0,0});
        const auto initial=database+u"\\hardware profile\\0001";create_key(initial);
        set(initial,u"dockstate",4,{7,0,0,0});
        set(initial,u"friendlyname",1,utf16_bytes(u"AppleJuice Hardware Profile"));
    }
    const auto number=registry.keys.at(database).find(u"currentconfig");
    if(number==registry.keys.at(database).end() || number->second.type!=4 || number->second.bytes.size()!=4){return finish(1015);}
    std::uint32_t id=0;for(unsigned i=0;i<4;++i)id|=std::uint32_t(number->second.bytes[i])<<(i*8);
    auto digits=std::to_string(id);if(digits.size()<4)digits.insert(0,4-digits.size(),'0');
    const auto path=database+u"\\hardware profile\\"+utf8_to_utf16(digits);
    if(!registry.keys.contains(path)){return finish(1015);}
    auto& profile=registry.keys.at(path);
    if(!profile.contains(u"hwprofileguid")){
        std::array<std::uint8_t,16> bytes{};if(!random_bytes(bytes)){return finish(31);}
        bytes[6]=static_cast<std::uint8_t>((bytes[6]&15U)|64U);bytes[8]=static_cast<std::uint8_t>((bytes[8]&63U)|128U);
        constexpr char16_t hex[]=u"0123456789ABCDEF";std::u16string guid=u"{";
        for(unsigned i=0;i<16;++i){if(i==4 || i==6 || i==8 || i==10)guid+=u'-';guid+=hex[bytes[i]>>4];guid+=hex[bytes[i]&15];}guid+=u'}';
        set(path,u"hwprofileguid",1,utf16_bytes(guid));
    }
    if(!profile.contains(u"friendlyname"))set(path,u"friendlyname",1,utf16_bytes(u"AppleJuice Hardware Profile"));
    const auto text=[&](const char16_t* name,std::size_t maximum)->std::optional<std::u16string>{
        const auto& value=profile.at(name);if(value.type!=1 || value.bytes.size()%2 || value.bytes.size()<2)return {};
        std::u16string out;for(std::size_t i=0;i<value.bytes.size();i+=2){const auto c=static_cast<char16_t>(value.bytes[i]|(unsigned(value.bytes[i+1])<<8));if(!c)return out;if(out.size()>=maximum-1)return {};out+=c;}return {};
    };
    const auto guid=text(u"hwprofileguid",39),name=text(u"friendlyname",80);
    if(!guid || !name || guid->size()!=38 || guid->front()!=u'{' || guid->back()!=u'}'){return finish(13);}
    std::uint32_t dock=7;if(const auto value=profile.find(u"dockstate");value!=profile.end() && value->second.type==4 && value->second.bytes.size()==4){dock=0;for(unsigned i=0;i<4;++i)dock|=std::uint32_t(value->second.bytes[i])<<(8*i);if(!dock)dock=7;}
    std::vector<std::uint8_t> output(wide?244:124);for(unsigned i=0;i<4;++i)output[i]=static_cast<std::uint8_t>(dock>>(8*i));
    if(wide){const auto a=utf16_bytes(*guid),b=utf16_bytes(*name);std::copy(a.begin(),a.end(),output.begin()+4);std::copy(b.begin(),b.end(),output.begin()+82);}
    else {const auto a=utf16_to_utf8(*guid),b=utf16_to_utf8(*name);if(a.size()>=39 || b.size()>=80){return finish(111);}std::copy(a.begin(),a.end(),output.begin()+4);std::copy(b.begin(),b.end(),output.begin()+43);}
    if(changed){if(const auto error=commit_registry_transaction(transaction)){return finish(error);}}
    else transaction.unchanged();
    rollback.committed=true;memory.write(GuestAddress{arg(0)},output);return finish(0);
}catch(const MemoryFault&){return finish(998);}catch(const std::invalid_argument&){return finish(13);}catch(const std::bad_alloc&){return finish(8);}

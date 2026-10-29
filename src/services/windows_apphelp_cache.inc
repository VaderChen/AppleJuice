// 於 invoke_registry 的獨立分支內引入；共用登錄交易與既有 I/O 排程契約。
using namespace apphelp_cache;
const WindowsCall call(state,memory,service);
const auto operation=static_cast<std::uint32_t>(call(0));
const auto saved_last_error=last_error_;
const auto finish=[&](std::uint32_t status){last_error_=saved_last_error;call.return_to(state,status);return std::optional<std::uint32_t>{};};
if(operation>=12)return finish(invalid);
const auto abi=layout(x86_);const unsigned unit=x86_?4:8;
std::vector<std::uint8_t> context(abi.size);
try {
    if(call(1)%4)return finish(0x80000002U);
    memory.read(GuestAddress{call(1)},context);
}catch(const MemoryFault&){return finish(0xc0000005U);}
// 同版 dispatcher 沒有服務 9 的實作分支；程序 shim 仍待完整契約。
if(operation==9)return finish(invalid);
if(operation==10)return finish(unsupported);
const bool lookup_operation=operation==0 || operation==11;
if(((operation>=6 && operation<=8) || !registry_file_.empty()) && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久 Apphelp 快取或 SDB I/O");
try {
    RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_);
    if(transaction.error && operation!=6)return finish(registry_native_status(transaction.error));
    const std::u16string storage=u"\x0002\\system\\currentcontrolset\\control\\session manager\\appcompatcache";
    auto& registry=runtime_.registry;
    const auto read_value=[&](std::u16string_view name){
        std::vector<std::uint8_t> bytes;const auto key=registry.keys.find(storage);
        if(key!=registry.keys.end())if(const auto found=key->second.find(std::u16string(name));found!=key->second.end()){
            if(found->second.type!=3 || found->second.bytes.empty())throw Error{corrupt};bytes=found->second.bytes;
        }
        return bytes;
    };
    const auto publish=[&](std::u16string name,std::u16string display,std::vector<std::uint8_t> encoded){
        const auto key=registry.keys.find(storage);
        if(key!=registry.keys.end())if(const auto old=key->second.find(name);old!=key->second.end())
            if(old->second.type==3 && old->second.bytes==encoded){transaction.unchanged();return;}
        auto before=registry;
        try {
            for(auto at=storage.find(u'\\');;at=storage.find(u'\\',at+1)){
                registry.keys.try_emplace(storage.substr(0,at));if(at==storage.npos)break;
            }
            auto& values=registry.keys.at(storage);const auto old=values.find(name);
            if(old==values.end())++registry.values;else registry.bytes-=old->second.bytes.size();
            registry.bytes+=encoded.size();values[std::move(name)]=RegistryValue{3,std::move(encoded),std::move(display)};
            registry.modified_times[storage]=runtime_.system_filetime();
            if(registry.keys.size()>4096 || registry.values>4096 || registry.bytes>16U*1024U*1024U)throw Error{quota};
            if(const auto error=commit_registry_transaction(transaction))throw Error{registry_native_status(error)};
        }catch(...){registry=std::move(before);throw;}
        poll_registry_notifications(false,storage,4,&memory);
    };
    const auto key_equal=[](std::u16string_view left,std::u16string_view right){
        return left.size()==right.size() && std::equal(left.begin(),left.end(),right.begin(),
            [](char16_t a,char16_t b){return unicode_case_unit(a,false)==unicode_case_unit(b,false);});
    };
    const auto read=[&](unsigned at,unsigned width=4){return get(context,at,width);};
    const auto output_address=read(abi.output,unit),capacity=read(abi.output_size);
    const auto unicode=[&](unsigned at,bool aligned=true){
        const auto length=read(at,2),maximum=read(at+2,2),buffer=read(at+unit,unit);
        if((length&1U) || length>maximum || !length || !buffer)throw Error{invalid};
        if(aligned && buffer%4)throw Error{0x80000002U};
        std::u16string text;text.reserve(static_cast<std::size_t>(length/2));
        for(std::uint64_t offset=0;offset<length;offset+=2)
            text.push_back(static_cast<char16_t>(memory.load(GuestAddress{buffer+offset},16)));
        return text;
    };
    const auto sdb_source=[&]{return filesystem_.open_reader(utf16_to_utf8(std::u16string(windows_directory)+u"\\AppPatch\\sysmain.sdb"));};
    const auto sdb_identity=[](const FileMetadata& value){return std::array<std::uint64_t,4>{value.size,value.modified,value.id,value.device};};
    const auto sdb_bytes=[&](const auto& source,const FileMetadata& before){
        if(before.size<42 || before.size>Sdb::maximum_size)throw Error{invalid};
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(before.size));
        const auto transfer=source.file->read(bytes,0);
        if(transfer.error)throw Error{registry_native_status(transfer.error)};
        if(transfer.bytes!=bytes.size())throw Error{corrupt};
        const auto after=source.file->information();if(after.error)throw Error{registry_native_status(after.error)};
        if(sdb_identity(after.metadata)!=sdb_identity(before))throw Error{0xc0000098U};return bytes;
    };
    if(operation==8){
#include "windows_apphelp_quirks.inc"
    }
    if(operation==6 || operation==7){
#include "windows_apphelp_cdb.inc"
    }
    const std::u16string value_name=u"applejuicecachev1";
    const auto previous=read_value(value_name);
    auto cache=Store::decode(previous,key_equal);++cache.statistics[operation==11?7:operation+1];
    std::uint32_t status{};std::vector<std::uint8_t> output;bool publish_size=false;
    initialize_tokens();const auto& token=runtime_.token_objects.at(effective_token_id());
    const auto member=[&](std::string_view name){const auto sid=sddl_sid(name);return security::token_sid_matches(token,sid,false) && (!token.restriction_check || security::token_sid_matches(token,sid,false,true));};
    const bool privileged=member("BA") || member("SY");
    try {
        if(operation==3){
            if(!privileged)throw Error{denied};
            const auto clears=cache.statistics[4];cache=Store{};cache.statistics[4]=clears;
        }else if(operation==4){
            if(!privileged)throw Error{denied};
            if(!output_address || capacity!=52)throw Error{invalid};
            memory.check(GuestAddress{output_address},52,Access::Write);
            for(const auto value:cache.statistics)append(output,value);
        }else if(operation==5){
            if(!privileged)throw Error{denied};
            output=cache.snapshot();publish_size=true;
            if(output.size()>capacity)status=0xc0000023U;
            else if(!output.empty())memory.check(GuestAddress{output_address},output.size(),Access::Write);
        }else{
            const auto base=operation==2?abi.update:operation==1?abi.remove:0U;
            auto mask=static_cast<std::uint32_t>(read(base));
            if(operation==2 && !privileged){mask&=unprivileged_update;if(!mask)throw Error{denied};}
            const auto alias=base+(lookup_operation?8U:abi.alias);
            const auto signature=base+(lookup_operation?abi.lookup_signature:abi.signature);
            const auto key=unicode(read(alias+unit,unit)?alias:signature);
            const auto handle=read(base+(lookup_operation?(x86_?16U:24U):abi.file),unit);
            std::uint64_t modified{};
            if(handle){
                const auto found=files_.find(handle);
                if(found==files_.end())throw Error{0xc0000008U};
                const auto rights=security::mapped_access(found->second.access,{0x120089U,0x120116U,0x1200a0U,0x1f01ffU});
                if(!(rights&0x80U))throw Error{denied};
                if(found->second.pipe || found->second.serial || found->second.hid || found->second.usb)throw Error{0xc0000010U};
                auto metadata=found->second.metadata;
                const auto native=found->second.native?found->second.native:found->second.identity;
                if(native){const auto info=native->information();if(info.error)throw Error{registry_native_status(info.error)};metadata=info.metadata;}
                modified=metadata.modified;
            }
            std::vector<std::uint8_t> update;
            if(operation==2){
                const auto size=read(base+abi.data_size),source=read(base+abi.data,unit);
                if(!size || size>65536)throw Error{invalid};
                update.resize(static_cast<std::size_t>(size));memory.read(GuestAddress{source},update);
            }
            auto& entry=cache.acquire(key,modified,key_equal);
            if(operation==2)cache.update(entry,mask,std::move(update));
            else if(operation==1)Store::remove(entry,mask);
            else{
                const auto environment_address=read(x86_?36U:64U,unit),environment_size=read(x86_?40U:72U);
                if(environment_size>Environment::maximum_size)throw Error{quota};
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(environment_size));
                if(environment_size)memory.read(GuestAddress{environment_address},bytes);
                const auto environment=Environment::capture(bytes,[](char16_t c){return unicode_case_unit(c,true);});
                const bool forced=(read(4)&0xbU) || environment.values[0].has_value();
                const auto executable_type=read(x86_?24U:40U,2);
                // AhcCacheLookup 的獨立 metadata 路徑；其餘 cold lookup 仍待 SDB 合成。
                if(!executable_type && !(mask&0x109fU) && (!Store::sufficient(entry,mask) || forced)){
                    const auto system=std::find_if(entry.records.begin(),entry.records.end(),[](const auto& record){return record.kind==0x200U;});
                    if(system!=entry.records.end()){
                        if(system->data.size()>4)throw Error{0xc0000095U};
                    }else{
                        std::uint32_t system_file=0;
                        if(!read(alias+unit,unit)){
                            const auto found=files_.find(handle);
                            if(found==files_.end())throw Error{0xc0000008U};
                            const auto rights=security::mapped_access(found->second.access,{0x120089U,0x120116U,0x1200a0U,0x1f01ffU});
                            if(!(rights&0x20000U))throw Error{denied};
                            if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許 Apphelp 檔案安全性 I/O");
                            const auto native=found->second.native?found->second.native:found->second.identity;
                            if(!native)throw Error{unsupported};
                            const auto descriptor=security::file_descriptor(security::read_native_file_security(*native,false),1);
                            system_file=descriptor.data[0]==sddl_sid("S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464");
                        }
                        std::vector<std::uint8_t> data;append(data,system_file);
                        cache.update(entry,0x200U,std::move(data));
                    }
                }
                // 未完成的 CreateProcess shim 合成不可誤走一般資料查詢。
                if(read(4)&0x10U)throw Error{unsupported};
                output=Store::records(entry,mask);
                if(output.empty()){++cache.statistics[9];throw Error{missing};}
                ++cache.statistics[8];publish_size=true;
                if(output.size()>capacity)status=0xc0000023U;
                else memory.check(GuestAddress{output_address},output.size(),Access::Write);
            }
        }
    }catch(const Error& error){status=error.status;output.clear();publish_size=false;}
     catch(const SecurityDescriptorError& error){status=registry_native_status(error.code);output.clear();publish_size=false;}
     catch(const MemoryFault&){status=0xc0000005U;output.clear();publish_size=false;}
    publish(value_name,u"AppleJuiceCacheV1",cache.encode());
    if(!status && !output.empty())memory.write(GuestAddress{output_address},output);
    if(publish_size){
        try{memory.store(GuestAddress{call(1)+abi.output_size},32,output.size());}
        catch(const MemoryFault&){if(lookup_operation)return finish(0xc0000005U);}
        // Lookup／LookupProcess 以最後複製大小欄位的狀態返回；SnapCache 保留原狀態。
        if(lookup_operation)status=0;
    }
    return finish(status);
}catch(const Error& error){return finish(error.status);}
 catch(const MemoryFault&){return finish(0xc0000005U);}
 catch(const std::bad_alloc&){return finish(0xc0000017U);}

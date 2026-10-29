// Windows 10 19041 的系統 SDB 路徑已合併；不同客體架構產生相同路徑後去重。
const auto normalize=[](std::u16string name){for(auto& c:name)c=unicode_case_unit(c,false);return name;};
std::u16string wanted;
if(operation==6){
    if(!output_address || capacity!=4)return finish(invalid);
    const auto at=x86_?124U:232U,length=static_cast<unsigned>(read(at,2));
    const auto buffer=read(at+unit,unit);
    if(!length || !buffer || !memory.load(GuestAddress{buffer},16))return finish(invalid);
    // AhcApiLookupCdb 複製 Length bytes，忽略 MaximumLength；奇數尾 byte 由終止字元覆蓋。
    if(length>65533)return finish(0xc0000095U);
    std::vector<std::uint8_t> name(length);memory.read(GuestAddress{buffer},name);
    for(unsigned n=0;n+1<length;n+=2)wanted.push_back(static_cast<char16_t>(get(name,n,2)));
    wanted=normalize(std::move(wanted));
    if(output_address%4)return finish(0x80000002U);
    memory.check(GuestAddress{output_address},4,Access::Write);
}
const auto result=[&](std::uint32_t status,std::uint32_t value=0){
    // 公開 API 在成功寫出結果後回傳 SUCCESS；內部未命中／錯誤以零旗標表示。
    if(operation==6){memory.store(GuestAddress{output_address},32,value);return finish(0);}
    return finish(status);
};
if(transaction.error)return result(registry_native_status(transaction.error));
// 交易鎖內先清除；後續開啟／解析失敗也不可留下原 CDB 內容。
if(operation==7)publish(u"applejuicecdbv1",u"AppleJuiceCdbV1",CompatibilityDatabase{}.encode());
try {
    const auto source=sdb_source();
    if(source.error || !source.file){
        transaction.unchanged();
        if(operation==7 || source.error==2 || source.error==3)return result(invalid);
        return result(source.error?registry_native_status(source.error):invalid);
    }
    const auto before=source.file->information();if(before.error)throw Error{registry_native_status(before.error)};
    const auto expected=sdb_identity(before.metadata);
    CompatibilityDatabase database;bool reload=true;
    if(operation==6){
        const auto previous=read_value(u"applejuicecdbv1");
        if(!previous.empty()){database=CompatibilityDatabase::decode(previous);reload=database.identity!=expected;}
    }
    if(reload){
        const auto bytes=sdb_bytes(source,before.metadata);
        database.entries=Sdb(bytes).executable_types(normalize);database.identity=expected;
        publish(u"applejuicecdbv1",u"AppleJuiceCdbV1",database.encode());
    }else transaction.unchanged();
    if(operation==7)return result(0);
    const auto found=database.entries.find(wanted);
    return found==database.entries.end()?result(missing):result(0,found->second);
}catch(const Error& error){return result(error.status);}
 catch(const std::bad_alloc&){return result(0xc0000017U);}

// WOW64 只展開 AHC_SERVICE_DATA，沒有轉換服務 8 的輸出；兩種 ABI 均寫 8 bytes。
if(!output_address || capacity!=8)return finish(invalid);
auto quirks=QuirksState::decode(read_value(u"applejuicequirksv1"));
const auto source=sdb_source();
if(source.error || !source.file)return finish(registry_native_status(source.error?source.error:2));
const auto info=source.file->information();if(info.error)return finish(registry_native_status(info.error));
const bool changed=!quirks.observed || quirks.observed_time!=info.metadata.modified;
std::vector<std::uint8_t> bytes;
const auto publish_quirks=[&]{publish(u"applejuicequirksv1",u"AppleJuiceQuirksV1",quirks.encode());};
if(changed){
    // DetectServicing 先記錄已觀測時間；後續失敗不重複清除，也不回退舊時間。
    if(quirks.attempt==UINT64_MAX)throw Error{quota};
    quirks.observed=true;quirks.observed_time=info.metadata.modified;++quirks.attempt;publish_quirks();
    auto cache=Store::decode(read_value(u"applejuicecachev1"),key_equal);
    for(auto& entry:cache.entries)Store::remove(entry,0x109fU);
    const auto clears=cache.statistics[4]+1;cache.statistics={52};cache.statistics[4]=clears;
    publish(u"applejuicecachev1",u"AppleJuiceCacheV1",cache.encode());
    publish(u"applejuicecdbv1",u"AppleJuiceCdbV1",CompatibilityDatabase{}.encode());
    bytes=sdb_bytes(source,info.metadata);
    CompatibilityDatabase database;database.identity=sdb_identity(info.metadata);
    database.entries=Sdb(bytes).executable_types([](std::u16string name){for(auto& c:name)c=unicode_case_unit(c,false);return name;});
    publish(u"applejuicecdbv1",u"AppleJuiceCdbV1",database.encode());
}
// 來源更新副作用先完成；輸出不能寫入時避免新增無法取回位址的 view。
memory.check(GuestAddress{output_address},8,Access::Write);
std::uint64_t address{};
if(!changed){
    if(!quirks.active)return finish(0xc0000001U);
    address=map_shared_table(memory,u"apphelp-quirks",quirks.generation(quirks.active),{},false);
    if(!address && last_error_!=2)return finish(registry_native_status(last_error_));
}
if(!address){
    if(bytes.empty())bytes=sdb_bytes(source,info.metadata);
    const auto table=Sdb(bytes).quirks_table([](char16_t value){return unicode_case_unit(value,true);});
    const auto generation=changed?quirks.attempt:quirks.active;
    const auto previous=quirks.active?quirks.generation(quirks.active):std::u16string{};
    address=map_shared_table(memory,u"apphelp-quirks",quirks.generation(generation),table,changed,[&]{
        if(changed){quirks.active=generation;quirks.size=table.size();publish_quirks();}
    },changed?std::u16string_view(previous):std::u16string_view{});
    if(!address)return finish(registry_native_status(last_error_));
}
memory.store(GuestAddress{output_address},64,address);
return finish(0);

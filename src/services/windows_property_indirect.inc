// MS-OLEPS：CONTENTS 保存間接名稱；同層的 prop<十進位 ID> 持有實際內容。
const auto indirect=[](unsigned type){return (type>=66 && type<=69) || type==73;};
const auto storage_value=[](unsigned type){return type==67 || type==69;};
const auto element_name=[](std::uint32_t id){return utf8_to_utf16("prop"+std::to_string(id));};
const auto indirect_record=[&](unsigned type,std::uint32_t id,unsigned page){
    auto text=element_name(id);
    property_set::Bytes bytes;
    if(page==1200)bytes=utf16_bytes(text);
    else{
        auto encoded=windows_crt_encode(text,page);
        if(!encoded)throw property_set::Invalid{};
        bytes=std::move(*encoded);bytes.push_back(0);
    }
    const size_t length_at=type==73?20:4;
    property_set::Bytes record(length_at+4+((bytes.size()+3)&~size_t{3}));
    record[0]=static_cast<std::uint8_t>(type);
    for(unsigned j=0;j<4;++j)record[length_at+j]=static_cast<std::uint8_t>(bytes.size()>>(8*j));
    std::copy(bytes.begin(),bytes.end(),record.data()+length_at+4);
    return record;
};
// 先在同一 IStorage 複製，再以 RenameElement 換入；CopyTo 失敗時舊物件仍可開啟。
const auto replace_element=[&](std::uint32_t id,unsigned type,std::uint64_t source,bool had_old)
    ->service_callback::Task<std::uint32_t>{
    const auto container=object.container,slot=temp(unit),canonical=scratch.text(element_name(id));
    if(!indirect(type))co_return co_await method(container,12,{canonical});
    const auto token=std::to_string(slot);
    const auto pending=scratch.text(utf8_to_utf16("~new"+token));
    const auto previous=scratch.text(utf8_to_utf16("~old"+token));
    const bool nested=storage_value(type);
    auto hr=co_await method(container,nested?5:3,{pending,0x12,0,0,slot});
    if(hr)co_return hr;
    const auto child=ptr(slot);
    std::exception_ptr failure;
    try{
        if(source && nested)hr=co_await method(source,7,{0,0,0,child});
        else if(source){
            const auto copied=temp(8),written=temp(8);
            // 區塊複製也適用於只接受有限請求長度的 IStream。
            do{
                m.store(GuestAddress{copied},64,0);m.store(GuestAddress{written},64,0);
                constexpr unsigned chunk=65536;
                auto args=x86_?std::vector<std::uint64_t>{child,chunk,0,copied,written}:
                               std::vector<std::uint64_t>{child,chunk,copied,written};
                hr=co_await method(source,7,std::move(args));
                if(hr>>31)break;
                const auto n=m.load(GuestAddress{copied},64);
                if(n>chunk || n!=m.load(GuestAddress{written},64)){hr=storage::write_fault;break;}
                hr=0;
                if(n<chunk)break;
            }while(true);
        }
    }catch(...){failure=std::current_exception();}
    (void)co_await method(child,2,{});
    bool moved=false;
    if(!failure && !hr && had_old){hr=co_await method(container,13,{canonical,previous});moved=!hr;}
    if(!failure && !hr)hr=co_await method(container,13,{pending,canonical});
    if(failure || hr){
        if(moved)(void)co_await method(container,13,{previous,canonical});
        (void)co_await method(container,12,{pending});
        if(failure)std::rethrow_exception(failure);
        co_return hr;
    }
    if(moved)co_return co_await method(container,12,{previous});
    co_return 0;
};

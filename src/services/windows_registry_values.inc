// 呼叫者已持有同一 RegistryTransaction；借用現有值，只保存本次查詢的描述與必要轉碼。
std::uint32_t WindowsServices::query_registry_multiple_values(GuestMemory& memory,const WindowsCall& arg,
        const RegistryHandle& handle,bool native,bool wide){
    if(!(handle.access&1U))return 5;
    const unsigned unit=x86_?4U:8U,bits=unit*8;
    const unsigned stride=native?(x86_?16U:24U):(x86_?16U:32U);
    const auto count=static_cast<std::uint32_t>(arg(2));
    if(!arg(4) || (count && !arg(1)))return 87;
    const auto capacity=memory.load(GuestAddress{arg(4)},32);
    if(!arg(3) && capacity)return 87;
    memory.check(GuestAddress{arg(4)},4,Access::Write);
    if(native && arg(5))memory.check(GuestAddress{arg(5)},4,Access::Write);
    struct Item {
        const RegistryValue* value{};
        std::vector<std::uint8_t> converted;
        std::uint32_t offset{},size{};
        bool encoded{};
        std::span<const std::uint8_t> bytes()const{return encoded?std::span<const std::uint8_t>{converted}:std::span<const std::uint8_t>{value->bytes};}
    };
    // Win32 規定 VALENT 與 payload 合計最多 1 MiB；Native 沿用服務的操作配置上限。
    constexpr std::uint64_t win32_limit=1024U*1024U;
    const std::uint64_t descriptors=std::uint64_t{count}*stride;
    if(!native && descriptors>win32_limit)return 8; // 超出本次原子操作的配置額度。
    if(count>output_limit/sizeof(Item))return 8;
    if(count){memory.check(GuestAddress{arg(1)},descriptors,Access::Read);memory.check(GuestAddress{arg(1)},descriptors,Access::Write);}
    std::vector<Item> items;items.reserve(count);
    const auto& values=runtime_.registry.keys.at(handle.path);
    const auto limit=native?std::uint64_t{output_limit}:win32_limit-descriptors;
    std::uint64_t required=0;
    for(std::uint32_t i=0;i<count;++i){
        const auto address=arg(1)+std::uint64_t{i}*stride;
        const auto name_pointer=memory.load(GuestAddress{address},static_cast<std::uint8_t>(bits));
        std::u16string name;
        if(native){
            if(!name_pointer)return 87;
            const auto length=memory.load(GuestAddress{name_pointer},16),maximum=memory.load(GuestAddress{name_pointer+2},16);
            const auto data=memory.load(GuestAddress{name_pointer+unit},static_cast<std::uint8_t>(bits));
            if((length&1U) || length>maximum || (length && !data))return 87;
            name.reserve(static_cast<std::size_t>(length/2));
            for(std::uint64_t at=0;at<length;at+=2){const auto c=static_cast<char16_t>(memory.load(GuestAddress{data+at},16));if(!c)return 87;name.push_back(c);}
        }else if(name_pointer)name=wide?read_utf16(memory,GuestAddress{name_pointer},32768):utf8_to_utf16(read_ansi(memory,name_pointer,32768));
        for(auto& c:name)c=unicode_case_unit(c,false);
        const auto found=values.find(name);if(found==values.end())return 2;
        Item item;item.value=&found->second;
        if(!wide && (item.value->type==1 || item.value->type==2 || item.value->type==7)){
            const auto& data=item.value->bytes;if(data.size()%2)return 13;
            // UTF-8 輸出至少為 UTF-16 bytes 的一半；先拒絕必定超額的輸入。
            if(data.size()>limit*2U)return 8;
            std::u16string chars;chars.reserve(data.size()/2);
            for(std::size_t at=0;at<data.size();at+=2)chars.push_back(static_cast<char16_t>(data[at]|(std::uint32_t{data[at+1]}<<8)));
            const auto converted=utf16_to_utf8(chars);item.converted.assign(converted.begin(),converted.end());item.encoded=true;
        }
        const auto size=item.bytes().size();const auto offset=native?(required+3U)&~UINT64_C(3):required;
        if(offset>limit || size>limit-offset)return 8;
        item.offset=static_cast<std::uint32_t>(offset);item.size=static_cast<std::uint32_t>(size);required=offset+size;
        items.push_back(std::move(item));
    }
    const bool overflow=capacity<required || (!native && !arg(3));
    std::uint64_t written=0;
    if(native || !overflow)for(const auto& item:items){
        if(item.offset+std::uint64_t{item.size}>capacity)break;
        written=item.offset+std::uint64_t{item.size};
    }
    if(written)memory.check(GuestAddress{arg(3)},written,Access::Write);
    if(native || !overflow){
        // 完整預檢後才發布；小緩衝的 Native 結果僅包含放得下的完整值。
        std::uint64_t previous=0;constexpr std::array<std::uint8_t,3> zero{};
        for(std::size_t i=0;i<items.size();++i){
            const auto& item=items[i];const auto address=arg(1)+i*stride;
            memory.store(GuestAddress{address+unit},32,item.size);
            if(native){memory.store(GuestAddress{address+unit+4},32,item.offset);memory.store(GuestAddress{address+unit+8},32,item.value->type);}
            else {memory.store(GuestAddress{address+unit*2},static_cast<std::uint8_t>(bits),arg(3)+item.offset);memory.store(GuestAddress{address+unit*3},32,item.value->type);}
            if(item.offset+std::uint64_t{item.size}<=written){
                if(item.offset>previous)memory.write(GuestAddress{arg(3)+previous},std::span(zero).first(static_cast<std::size_t>(item.offset-previous)));
                if(item.size)memory.write(GuestAddress{arg(3)+item.offset},item.bytes());previous=item.offset+item.size;
            }
        }
    }
    memory.store(GuestAddress{arg(4)},32,native?written:required);
    if(native && arg(5))memory.store(GuestAddress{arg(5)},32,required);
    return overflow?234U:0U;
}

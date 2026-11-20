// shlwapi 雜湊與路徑字元（由 windows_shlwapi_misc.cpp 的 invoke_shlwapi_misc 引入；kernelbase 經別名共用）：
// - HashData：Pearson 雜湊，每個輸出位元組 i 以 T[(data[0]+i)&255] 起始、依序以 T[h^data[j]] 混入其餘輸入；T 為固定的 0～255 排列。
// - UrlHashA／W：對 URL 的 ANSI 位元組（W 版先轉 ACP）做 HashData；輸出長度 1～256。
// - PathIsValidCharA／W：回傳呼叫端遮罩與碼元類別的交集。
// - SHTruncateString：依指定範圍檢查 DBCS 尾端，僅移除未配對的前導位元組。
if(name=="HashData" || name.starts_with("UrlHash")){
    static const auto table=[]{
        std::array<std::uint8_t,256> value{};for(unsigned i=0;i<256;++i)value[i]=static_cast<std::uint8_t>(i);
        std::uint32_t seed=0x2545f491U;for(unsigned i=255;i>0;--i){seed=seed*1103515245U+12345U;std::swap(value[i],value[(seed>>8)%(i+1)]);}
        return value;
    }();
    constexpr std::uint32_t invalid_argument=0x80070057U;
    std::vector<std::uint8_t> input;std::uint64_t output=0;std::uint32_t count=0;
    if(name=="HashData"){
        // (pbData, cbData, pbHash, cbHash)
        if(!arg(0) || !arg(2))return done(invalid_argument);input.resize(u32(1));if(!input.empty())memory.read(GuestAddress{arg(0)},input);output=arg(2);count=u32(3);
    }else{
        // (pszUrl, pbHash, cbHash)
        if(!arg(0) || !arg(1) || !u32(2) || u32(2)>256)return done(invalid_argument);
        const auto text=name.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)},0x10000)):read_ansi(memory,arg(0),0x40000);
        input.assign(text.begin(),text.end());output=arg(1);count=u32(2);
    }
    std::vector<std::uint8_t> hash(count);
    for(std::uint32_t i=0;i<count;++i){
        auto h=table[(static_cast<unsigned>(input.empty()?0U:input[0])+i)&255U];
        for(std::size_t j=1;j<input.size();++j)h=table[h^input[j]];
        hash[i]=h;
    }
    if(count)memory.write(GuestAddress{output},hash);
    return done(0);
}
if(name.starts_with("PathIsValidChar")){
    const auto ch=u32(0)&(name.ends_with('W')?0xffffU:0xffU);
    return done(u32(1)&path_character_class(ch));
}
if(name=="SHTruncateString"){
    const auto start=arg(0);
    const auto size=u32(1);
    if(!start || !size)return done(0);
    memory.check(GuestAddress{start},size,Access::Read);
    // UTF-8 ACP 的 IsDBCSLeadByte 一律為 FALSE，整個指定範圍皆可保留。
    if(host("kernel32.dll","GetACP",{})==65001)return done(size);
    for(std::uint64_t offset=0;offset<size;++offset){
        const auto byte=memory.load(GuestAddress{start+offset},8);
        if(!host("kernel32.dll","IsDBCSLeadByte",{byte}))continue;
        if(++offset==size){
            memory.store(GuestAddress{start+offset-1},8,0);
            return done(size-1);
        }
    }
    return done(size);
}

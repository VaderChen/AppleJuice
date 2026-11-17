// mfplat 的屬性與媒體型別輔助（由 windows_media_foundation.cpp 的匯出鏈引入）：
// - MFGetAttributesAsBlobSize／MFGetAttributesAsBlob／MFInitAttributesFromBlob：encode_attributes／decode_attributes 的格式；IUnknown 屬性不寫入。
// - MFWrapMediaType／MFUnwrapMediaType：新型別帶指定主要型別與子型別，原型別的屬性以同一格式存成 MF_MT_WRAPPED_TYPE。
// - MFCompareFullToPartialMediaType：主要型別相同，且部分型別的每個屬性在完整型別中都存在且值相同。
// - MFAverageTimePerFrameToFrameRate：常見幀率（NTSC 1000/1001 系列與整數幀率）誤差在 1 個 100 ns 內時回傳標準分數，否則為 10000000/平均時間（約分）。
// - MFllMulDiv：以 128 位元計算 (a*b+d)/c，超出範圍時飽和。MFHeapAlloc／MFHeapFree：程序堆積。MFCreatePathFromURL：沿用 PathCreateFromUrlW，結果以 CoTaskMemAlloc 配置。
if(name=="MFGetAttributesAsBlobSize" || name=="MFGetAttributesAsBlob" || name=="MFInitAttributesFromBlob" || name=="MFWrapMediaType" || name=="MFUnwrapMediaType" ||
   name=="MFCompareFullToPartialMediaType" || name=="MFAverageTimePerFrameToFrameRate" || name=="MFllMulDiv" || name=="MFHeapAlloc" || name=="MFHeapFree" || name=="MFCreatePathFromURL"){
    const Guid wrapped=mf::guid(0x4d3f7b23,0xd02f,0x4e6c,{0x9b,0xee,0xe4,0xbf,0x2c,0x6c,0x69,0x5d});
    const auto attributes_of=[&](std::uint64_t pointer){require(pointer!=0,pointer_error);auto item=entry(pointer).object;require(mf::attributes(item->kind),no_interface);return item;};
    const auto load_items=[&](Object& target,std::span<const std::uint8_t> bytes){
        const auto items=decode_attributes(bytes);require(items.has_value(),invalid);
        for(const auto& [key,value]:target.attributes)if(value.type==13)release(value.scalar);target.attributes.clear();
        for(const auto& [key,value]:*items){require(value.type!=13,invalid);set(target,key,value);}};
    if(name=="MFGetAttributesAsBlobSize"){require(arg(1)!=0,pointer_error);put(arg(1),encode_attributes(attributes_of(arg(0))->attributes,false).size());}
    else if(name=="MFGetAttributesAsBlob"){
        const auto bytes=encode_attributes(attributes_of(arg(0))->attributes,false);require(arg(1)!=0,pointer_error);
        require(d(2)>=bytes.size(),0x8007007aU);memory.write(GuestAddress{arg(1)},bytes);
    }else if(name=="MFInitAttributesFromBlob"){
        auto target=attributes_of(arg(0));require(arg(1)!=0 || !d(2),pointer_error);load_items(*target,data(arg(1),d(2)));
    }else if(name=="MFWrapMediaType"){
        out(arg(3));auto original=object(arg(0),mf::MediaType);const auto major=guid(arg(1)),sub=guid(arg(2));
        const auto p=make(mf::MediaType);auto item=object(p,mf::MediaType);
        Value v;v.type=72;v.id=major;set(*item,mf::MF_MT_MAJOR_TYPE,v);v.id=sub;set(*item,mf::MF_MT_SUBTYPE,v);
        Value blob;blob.type=0x1011;blob.blob=encode_attributes(original->attributes,false);set(*item,wrapped,blob);put(arg(3),p,bits);
    }else if(name=="MFUnwrapMediaType"){
        out(arg(1));auto item=object(arg(0),mf::MediaType);const auto& blob=get(*item,wrapped,0x1011);
        const auto p=make(mf::MediaType);try{load_items(*object(p,mf::MediaType),blob.blob);}catch(...){release(p);throw;}put(arg(1),p,bits);
    }else if(name=="MFCompareFullToPartialMediaType"){
        // 回傳 BOOL；任一參數無效視為不相符。
        bool match=false;
        if(arg(0) && arg(1) && bridge.objects.contains(arg(0)) && bridge.objects.contains(arg(1))){
            const auto full=entry(arg(0)).object,partial=entry(arg(1)).object;
            if(full->kind==mf::MediaType && partial->kind==mf::MediaType){
                const auto major=partial->attributes.find(mf::MF_MT_MAJOR_TYPE),other=full->attributes.find(mf::MF_MT_MAJOR_TYPE);
                match=major!=partial->attributes.end() && other!=full->attributes.end() && major->second==other->second &&
                    std::all_of(partial->attributes.begin(),partial->attributes.end(),[&](const auto& item){const auto found=full->attributes.find(item.first);return found!=full->attributes.end() && found->second==item.second;});
            }
        }
        result=match?1:0;
    }else if(name=="MFAverageTimePerFrameToFrameRate"){
        require(arg(1)!=0 && arg(2)!=0,pointer_error);const auto [numerator,denominator]=frame_rate_of(arg(0));put(arg(1),numerator);put(arg(2),denominator);
    }else if(name=="MFllMulDiv"){
        const auto a=static_cast<std::int64_t>(arg(0)),b=static_cast<std::int64_t>(arg(1)),c=static_cast<std::int64_t>(arg(2)),dd=static_cast<std::int64_t>(arg(3));
        __int128 value=c?(static_cast<__int128>(a)*b+dd)/c:(static_cast<__int128>(a)*b>=0?static_cast<__int128>(INT64_MAX):static_cast<__int128>(INT64_MIN));
        if(value>INT64_MAX)value=INT64_MAX;else if(value<INT64_MIN)value=INT64_MIN;result=static_cast<std::uint64_t>(static_cast<std::int64_t>(value));
    }else if(name=="MFHeapAlloc"){
        // (nSize, dwFlags, pszFile, line, eat)：HEAP_ZERO_MEMORY（8）時清零。
        const auto size=arg(0);if(size>256U*1024U*1024U){result=0;}
        else {result=allocate_heap(memory,std::max<std::uint64_t>(size,1));if(result && (d(1)&8U))memory.write(GuestAddress{result},std::vector<std::uint8_t>(static_cast<std::size_t>(std::max<std::uint64_t>(size,1))));}
    }else if(name=="MFHeapFree"){if(arg(0))(void)free_heap(memory,arg(0),state);}
    else if(name=="MFCreatePathFromURL"){
        require(arg(0)!=0 && arg(1)!=0,pointer_error);out(arg(1));
        GuestScratch scratch(*this,memory,state);const auto buffer=scratch.allocate(2*32768),length=scratch.allocate(4);memory.store(GuestAddress{length},32,32768);
        const auto status=static_cast<std::uint32_t>(nested_service(state,memory,"shlwapi.dll","PathCreateFromUrlW",{arg(0),buffer,length,0}));
        require(!status,status==0x80070057U?invalid:status);
        const auto bytes=utf16_bytes(read_utf16(memory,GuestAddress{buffer},32767));const auto path=allocate_heap(memory,bytes.size());if(!path)throw std::bad_alloc{};
        memory.write(GuestAddress{path},bytes);gui_.task_allocations.insert(path);put(arg(1),path,bits);
    }
}

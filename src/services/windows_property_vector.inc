// MS-OLEPS：定寬元素連排，只有向量尾端對齊；字串各自攜帶長度與填充。
const auto text_element=[](unsigned type){return type==8 || type==30 || type==31;};
const auto element_width=[](unsigned type){
    if(type==72)return 16U;
    if(type<2 || type==22 || type==23)return 0U;
    return property_set::scalar_width(type).value_or(0);
};
const auto store_dword=[](property_set::Bytes& bytes,size_t at,size_t value){
    if(value>UINT32_MAX)throw storage::Error{storage::medium_full};
    for(unsigned i=0;i<4;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));
};
const auto append_text=[&](property_set::Bytes& record,unsigned type,std::uint64_t pointer,unsigned page){
    const auto start=record.size();
    record.resize(start+4);
    if(!pointer)return;
    std::u16string text;
    if(type==30)text=utf8_to_utf16_lossy(read_ansi(m,pointer));
    else if(type==31)text=read_utf16(m,GuestAddress{pointer});
    else{
        const auto length=m.load(GuestAddress{pointer-4},32);
        if(length%2)throw storage::Error{invalid};
        text=read_utf16_counted(m,GuestAddress{pointer},static_cast<size_t>(length/2));
    }
    if(type==31 || page==1200){
        const auto bytes=utf16_bytes(text);
        store_dword(record,start,type==31?bytes.size()/2:bytes.size());
        record.insert(record.end(),bytes.begin(),bytes.end());
    }else{
        auto bytes=windows_crt_encode(text,page);
        if(!bytes)throw storage::Error{invalid};
        store_dword(record,start,bytes->size()+1);
        record.insert(record.end(),bytes->begin(),bytes->end());record.push_back(0);
    }
    record.resize((record.size()+3)&~size_t{3});
};
const auto unpack_text=[&](std::span<const std::uint8_t> record,size_t& pos,unsigned type,unsigned page){
    const size_t count=property_set::read(record,pos);pos+=4;
    if(!count)return std::uint64_t{};
    const size_t bytes=count*(type==31?2U:1U),padded=(bytes+3)&~size_t{3};
    if(padded>record.size()-pos)throw property_set::Invalid{};
    auto data=record.subspan(pos,bytes);pos+=padded;
    std::u16string text;
    if(type==31 || page==1200){
        if(bytes%2 || property_set::read(data,bytes-2,2))throw property_set::Invalid{};
        text.reserve(bytes/2-1);
        for(size_t i=0;i+2<bytes;i+=2)text.push_back(static_cast<char16_t>(property_set::read(data,i,2)));
    }else{
        if(data.back())throw property_set::Invalid{};
        const auto decoded=windows_nls_decode({reinterpret_cast<const char*>(data.data()),bytes-1},page,true);
        if(!decoded)throw property_set::Invalid{};
        text=*decoded;
    }
    auto payload=utf16_bytes(text);
    if(type==8){
        const auto address=allocate_bstr(m,payload.size()-2,payload);
        if(!address)throw std::bad_alloc{};
        return address;
    }
    if(type==30){
        const auto narrow=utf16_to_utf8_lossy(text);
        payload.assign(narrow.begin(),narrow.end());payload.push_back(0);
    }
    const auto address=allocate_heap(m,payload.size());
    if(!address)throw std::bad_alloc{};
    m.write(GuestAddress{address},payload);return address;
};
const auto append_clip=[&](property_set::Bytes& record,std::uint64_t clip){
    if(!clip)throw storage::Error{invalid};
    const size_t size=m.load(GuestAddress{clip},32);
    if(size<4)throw storage::Error{invalid};
    const auto source=ptr(clip+8);
    if(size>4)m.check(GuestAddress{source},size-4,Access::Read);
    const auto start=record.size();
    record.resize(start+4+((size+3)&~size_t{3}));
    store_dword(record,start,size);
    store_dword(record,start+4,m.load(GuestAddress{clip+4},32));
    if(size>4)m.read(GuestAddress{source},std::span(record).subspan(start+8,size-4));
};
const auto unpack_clip=[&](std::span<const std::uint8_t> record,size_t& pos,std::uint64_t clip){
    const size_t size=property_set::read(record,pos);pos+=4;
    const auto padded=(size+3)&~size_t{3};
    if(size<4 || padded>record.size()-pos)throw property_set::Invalid{};
    m.store(GuestAddress{clip},32,size);
    m.store(GuestAddress{clip+4},32,property_set::read(record,pos));
    if(size>4){
        const auto data=allocate_heap(m,size-4);
        if(!data)throw std::bad_alloc{};
        m.store(GuestAddress{clip+8},bits,data);
        m.write(GuestAddress{data},record.subspan(pos+4,size-4));
    }
    pos+=padded;
};
const auto pack_item=[&](property_set::Bytes& record,std::uint64_t item,unsigned page,bool& version_one){
    const auto type=static_cast<unsigned>(m.load(GuestAddress{item},16));
    const auto start=record.size();
    record.resize(start+4);store_dword(record,start,type);
    if(type==16)version_one=true;
    if(text_element(type))append_text(record,type,ptr(item+8),page);
    else if(type==71)append_clip(record,ptr(item+8));
    else{
        const auto width=element_width(type);
        if(!width)throw storage::Error{storage::invalid_function};
        record.resize(start+4+((width+3U)&~3U));
        const auto payload=type==72?ptr(item+8):item+8;
        m.read(GuestAddress{payload},std::span(record).subspan(start+4,width));
    }
};
const auto unpack_item=[&](std::span<const std::uint8_t> record,size_t& pos,unsigned page,std::uint64_t item){
    const auto type=property_set::read(record,pos);pos+=4;
    if(text_element(type)){
        m.store(GuestAddress{item+8},bits,unpack_text(record,pos,type,page));
    }else if(type==71){
        const auto clip=allocate_heap(m,8+unit);
        if(!clip)throw std::bad_alloc{};
        m.fill(GuestAddress{clip},0,8+unit);m.store(GuestAddress{item+8},bits,clip);
        m.store(GuestAddress{item},16,type);unpack_clip(record,pos,clip);
    }else{
        const auto width=element_width(type),padded=(width+3U)&~3U;
        if(!width || padded>record.size()-pos)throw property_set::Invalid{};
        auto payload=item+8;
        if(type==72){
            payload=allocate_heap(m,width);if(!payload)throw std::bad_alloc{};
            m.store(GuestAddress{item+8},bits,payload);
        }
        m.write(GuestAddress{payload},record.subspan(pos,width));pos+=padded;
    }
    m.store(GuestAddress{item},16,type);
};
const auto pack_vector=[&](unsigned type,std::uint64_t at,unsigned page,bool& version_one){
    const auto element=type&0xfffU,width=element_width(element);
    if(!width && !text_element(element) && element!=12 && element!=71)throw storage::Error{storage::invalid_function};
    const auto count=m.load(GuestAddress{at+8},32),elements=ptr(at+8+unit);
    if(count>1048576 || (count && !elements))throw storage::Error{invalid};
    property_set::Bytes record(8);
    store_dword(record,0,type);store_dword(record,4,count);
    if(width){
        const size_t bytes=count*width;
        record.resize(8+((bytes+3)&~size_t{3}));
        if(bytes)m.read(GuestAddress{elements},std::span(record).subspan(8,bytes));
    }else if(element==12){
        for(size_t i=0;i<count;++i)pack_item(record,elements+i*stride,page,version_one);
    }else if(element==71){
        for(size_t i=0;i<count;++i)append_clip(record,elements+i*(8+unit));
    }else{
        for(size_t i=0;i<count;++i)append_text(record,element,ptr(elements+i*unit),page);
    }
    return record;
};
const auto unpack_vector=[&](std::span<const std::uint8_t> record,unsigned type,std::uint64_t at,unsigned page){
    const auto element=type&0xfffU,width=element_width(element);
    if(!width && !text_element(element) && element!=12 && element!=71)throw storage::Error{storage::invalid_function};
    const size_t count=property_set::read(record,4);
    if(count>1048576 || count>(record.size()-8)/(width?width:4))throw property_set::Invalid{};
    m.store(GuestAddress{at},16,type);
    if(!count)return;
    const unsigned guest_width=width?width:element==12?stride:element==71?8+unit:unit;
    const size_t extent=count*guest_width;
    const auto elements=allocate_heap(m,extent);
    if(!elements)throw std::bad_alloc{};
    m.store(GuestAddress{at+8+unit},bits,elements);
    if(width){
        if(((extent+3)&~size_t{3})>record.size()-8)throw property_set::Invalid{};
        m.write(GuestAddress{elements},record.subspan(8,extent));m.store(GuestAddress{at+8},32,count);
    }else{
        m.fill(GuestAddress{elements},0,extent);
        size_t pos=8;
        for(size_t i=0;i<count;++i){
            const auto item=elements+i*guest_width;
            // 先公布已歸零的元素，內層配置失敗時由 PropVariantClear 統一回收。
            m.store(GuestAddress{at+8},32,i+1);
            if(element==12)unpack_item(record,pos,page,item);
            else if(element==71)unpack_clip(record,pos,item);
            else m.store(GuestAddress{item},bits,unpack_text(record,pos,element,page));
        }
    }
};

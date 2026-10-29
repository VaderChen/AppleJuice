constexpr off_t image_record_offset=32768;
constexpr std::size_t image_record_size=4096;
constexpr std::uint64_t image_record_magic=UINT64_C(0x31474d4941343658);
static_assert(mapping_detail::file_record_offset+sizeof(mapping_detail::FileRecord)<=image_record_offset);
static_assert(image_record_offset+image_record_size<=data_offset);
bool section_write(int fd,std::uint64_t offset,std::span<const std::uint8_t> bytes) {
    while(!bytes.empty()) {
        const auto count=::pwrite(fd,bytes.data(),bytes.size(),static_cast<off_t>(offset));
        if(count<0 && errno==EINTR)continue;if(count<=0)return false;
        offset+=static_cast<std::size_t>(count);bytes=bytes.subspan(static_cast<std::size_t>(count));
    }
    return true;
}
bool write_image_section(int fd,PeSectionImage& image) {
    std::array<std::uint8_t,image_record_size> bytes{};
    const auto put=[&](std::size_t index,std::uint64_t value){for(unsigned b=0;b<8;++b)bytes[index*8+b]=static_cast<std::uint8_t>(value>>(b*8));};
    const std::array<std::uint64_t,17> fields{image_record_magic,image.preferred_base,image.stack_reserve,image.stack_commit,
        image.image_size,image.entry,image.subsystem,image.subsystem_version,image.os_version,image.characteristics,image.dll_characteristics,
        image.machine,image.contains_code,image.loader_flags,image.file_size,image.checksum,image.ranges.size()};
    if(image.ranges.empty() || image.ranges.size()>97)return false;
    for(std::size_t i=0;i<fields.size();++i)put(i,fields[i]);
    for(std::size_t i=0;i<image.ranges.size();++i) {
        auto& range=image.ranges[i];const auto at=17+i*4;
        put(at,range.rva);put(at+1,range.size);put(at+2,range.permissions);put(at+3,range.shared?1:0);
        if(range.bytes.size()!=range.size || !section_write(fd,data_offset+range.rva,range.bytes))return false;
        std::vector<std::uint8_t>{}.swap(range.bytes);
    }
    return section_write(fd,image_record_offset,bytes);
}
std::optional<PeSectionImage> read_image_section(int fd,std::uint64_t size) {
    std::array<std::uint8_t,image_record_size> bytes{};std::size_t done=0;
    while(done<bytes.size()){
        const auto count=::pread(fd,bytes.data()+done,bytes.size()-done,image_record_offset+static_cast<off_t>(done));
        if(count<0 && errno==EINTR)continue;if(count<=0)return {};done+=static_cast<std::size_t>(count);
    }
    const auto get=[&](std::size_t index){std::uint64_t value=0;for(unsigned b=0;b<8;++b)value|=std::uint64_t{bytes[index*8+b]}<<(b*8);return value;};
    if(get(0)!=image_record_magic || get(4)!=size || !get(16) || get(16)>97 || get(5)>=size || (get(11)!=0x14c && get(11)!=0x8664))return {};
    for(unsigned i=4;i<16;++i)if(get(i)>UINT32_MAX)return {};
    PeSectionImage result;result.preferred_base=get(1);result.stack_reserve=get(2);result.stack_commit=get(3);
    result.image_size=static_cast<std::uint32_t>(size);result.entry=static_cast<std::uint32_t>(get(5));
    result.subsystem=static_cast<std::uint32_t>(get(6));result.subsystem_version=static_cast<std::uint32_t>(get(7));result.os_version=static_cast<std::uint32_t>(get(8));
    result.characteristics=static_cast<std::uint32_t>(get(9));result.dll_characteristics=static_cast<std::uint32_t>(get(10));result.machine=static_cast<std::uint32_t>(get(11));
    result.contains_code=static_cast<std::uint32_t>(get(12));result.loader_flags=static_cast<std::uint32_t>(get(13));result.file_size=static_cast<std::uint32_t>(get(14));result.checksum=static_cast<std::uint32_t>(get(15));
    std::uint64_t end=0;
    for(std::uint64_t i=0;i<get(16);++i) {
        const auto at=17+static_cast<std::size_t>(i)*4;
        const auto rva=get(at),count=get(at+1),permissions=get(at+2),shared=get(at+3);
        if(rva<end || rva%4096 || !count || count%4096 || rva>=size || count>size-rva || permissions>7 || shared>1 || (!i && rva))return {};
        result.ranges.push_back({static_cast<std::uint32_t>(rva),static_cast<std::uint32_t>(count),static_cast<Permissions>(permissions),shared!=0,{}});end=rva+count;
    }
    return result;
}
std::vector<std::uint8_t> image_section_information(const PeSectionImage& image,bool x86) {
    const unsigned unit=x86?4U:8U;std::vector<std::uint8_t> bytes(x86?48U:64U);
    const auto put=[&](unsigned at,std::uint64_t value,unsigned count){for(unsigned i=0;i<count;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
    // WOW64 對 64 位元映像的查詢保留 Machine，指標／堆疊使用 Windows 的相容值。
    const bool wide_image=x86 && image.machine==0x8664;
    put(0,wide_image?0x81231234U:image.preferred_base+image.entry,unit);
    put(unit*2,wide_image?0x100000U:image.stack_reserve,unit);put(unit*3,wide_image?0x10000U:image.stack_commit,unit);
    const auto at=unit*4;put(at,image.subsystem,4);put(at+4,image.subsystem_version,4);put(at+8,image.os_version,4);
    put(at+12,image.characteristics,2);put(at+14,image.dll_characteristics,2);put(at+16,image.machine,2);put(at+18,image.contains_code?1:0,1);
    put(at+20,image.loader_flags,4);put(at+24,image.file_size,4);put(at+28,image.checksum,4);return bytes;
}

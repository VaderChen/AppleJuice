// 來源檔的 SHA-256 搭配完整匯出前 tables 與實例參數；不以字型名稱或 mtime 當作磁碟鍵。
// 檔案狀態只用來避免同一程序重算已驗證過的 SHA，讀取前後仍驗證身分一致。
std::vector<std::uint8_t> font_source_digest(CTFontRef font){
    Cf<CFURLRef> url(static_cast<CFURLRef>(CTFontCopyAttribute(font,kCTFontURLAttribute)));
    if(!url || CFGetTypeID(url.get())!=CFURLGetTypeID())return {};
    std::array<UInt8,4096> path{};if(!CFURLGetFileSystemRepresentation(url.get(),true,path.data(),path.size()))return {};
    const int descriptor=::open(reinterpret_cast<const char*>(path.data()),O_RDONLY|O_CLOEXEC);if(descriptor<0)return {};
    struct Close{int fd;~Close(){::close(fd);}} close{descriptor};struct stat status{};
    if(::fstat(descriptor,&status) || !S_ISREG(status.st_mode) || status.st_size<=0 || status.st_size>static_cast<off_t>(native_font_collection_limit))return {};
    const auto same=[](const struct stat& a,const struct stat& b){return a.st_dev==b.st_dev && a.st_ino==b.st_ino && a.st_size==b.st_size &&
        a.st_mtimespec.tv_sec==b.st_mtimespec.tv_sec && a.st_mtimespec.tv_nsec==b.st_mtimespec.tv_nsec &&
        a.st_ctimespec.tv_sec==b.st_ctimespec.tv_sec && a.st_ctimespec.tv_nsec==b.st_ctimespec.tv_nsec;};
    struct Source{struct stat status;std::vector<std::uint8_t> digest;};static std::vector<Source> sources;
    for(const auto& source:sources)if(same(source.status,status))return source.digest;
    CryptoHash hash("SHA256",false,{});std::array<std::uint8_t,65536> buffer{};std::uint64_t bytes=0;
    for(;;){const auto count=::read(descriptor,buffer.data(),buffer.size());if(count<0 && errno==EINTR)continue;if(count<0)return {};if(!count)break;
        bytes+=static_cast<std::uint64_t>(count);if(bytes>native_font_collection_limit)return {};hash.update(std::span(buffer).first(static_cast<std::size_t>(count)));}
    struct stat after{};if(::fstat(descriptor,&after) || !same(status,after) || bytes!=static_cast<std::uint64_t>(status.st_size))return {};
    auto result=hash.digest();if(sources.size()>=64)sources.erase(sources.begin());sources.push_back({status,result});return result;
}
std::vector<std::uint8_t> font_resource_identity(const NativeFontFace& face,CTFontRef font,const FontTables& tables){
    const auto source=font_source_digest(font);
    const bool complete=std::any_of(tables.begin(),tables.end(),[](const auto& entry){return entry.first==0x676c7966 || entry.first==0x43464620 || entry.first==0x43464632 || entry.first==0x73626978 || entry.first==0x43424454 || entry.first==0x45424454;});
    // 原生私有輪廓若無可驗證來源，僅用程序內不可變實例快取。
    if(source.empty() && !complete)return {};
    CryptoHash hash("SHA256",false,{});
    const auto scalar=[&](const auto& value){hash.update({reinterpret_cast<const std::uint8_t*>(&value),sizeof(value)});};
    const auto text_part=[&](std::string_view value){scalar(static_cast<std::uint64_t>(value.size()));hash.update({reinterpret_cast<const std::uint8_t*>(value.data()),value.size()});};
    text_part("x64arm-font-resource-1");text_part(X64ARM_FONT_EXPORT_REVISION);
    text_part([NSProcessInfo processInfo].operatingSystemVersionString.UTF8String);
    hash.update(source); // macOS build 身分已涵蓋內建 CoreText 版本。
    scalar(face.em);scalar(face.simulations);scalar(CTFontGetSize(font));scalar(CTFontGetSymbolicTraits(font));scalar(CTFontGetGlyphCount(font));
    const auto matrix=CTFontGetMatrix(font);for(const auto value:{matrix.a,matrix.b,matrix.c,matrix.d,matrix.tx,matrix.ty})scalar(value);
    Cf<CFStringRef> postscript(CTFontCopyPostScriptName(font));const auto name=text(postscript.get());
    scalar(static_cast<std::uint64_t>(name.size()));hash.update({reinterpret_cast<const std::uint8_t*>(name.data()),name.size()*2});
    Cf<CFTypeRef> optical(CTFontCopyAttribute(font,kCTFontOpticalSizeAttribute));double optical_value=0;
    const bool has_optical=optical && CFGetTypeID(optical.get())==CFNumberGetTypeID() && CFNumberGetValue(static_cast<CFNumberRef>(optical.get()),kCFNumberDoubleType,&optical_value);
    scalar(has_optical);scalar(optical_value);
    Cf<CFDictionaryRef> variation(CTFontCopyVariation(font));std::map<std::int64_t,double> axes;
    if(variation){const auto count=static_cast<std::size_t>(CFDictionaryGetCount(variation.get()));std::vector<const void*> keys(count),values(count);CFDictionaryGetKeysAndValues(variation.get(),keys.data(),values.data());
        for(std::size_t i=0;i<count;++i){std::int64_t axis=0;double value=0;
            if(CFGetTypeID(keys[i])!=CFNumberGetTypeID() || CFGetTypeID(values[i])!=CFNumberGetTypeID() || !CFNumberGetValue(static_cast<CFNumberRef>(keys[i]),kCFNumberSInt64Type,&axis) || !CFNumberGetValue(static_cast<CFNumberRef>(values[i]),kCFNumberDoubleType,&value))return {};
            axes.emplace(axis,value);}}
    scalar(static_cast<std::uint64_t>(axes.size()));for(const auto& [axis,value]:axes){scalar(axis);scalar(value);}
    // tables 已按 tag 排序；各段有明確長度，串接不產生鍵歧義。
    for(const auto& [tag,bytes]:tables){scalar(tag);scalar(static_cast<std::uint64_t>(bytes.size()));hash.update(bytes);}
    return hash.digest();
}
std::filesystem::path font_resource_directory(){
    if(const auto* enabled=std::getenv("X64ARM_FONT_CACHE");enabled && std::string_view(enabled)=="0")return {};
    if(const auto* path=std::getenv("X64ARM_FONT_CACHE_DIR");path && *path)return path;
    return generated_resource_cache_directory();
}

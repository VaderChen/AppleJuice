// 原生 Metal IR 以完整原始碼、編譯選項及系統版本識別；引擎 UUID 改變不會使它失效。
// 離線產生工具負責限額與原子發布；執行時只讀取，損毀或不相容便回到原生編譯。
// 頂點位置在深度預先繪製與著色 pass 間必須一致；兩個編譯入口共用同一份選項與 MSL 調整。
// 預設保留位置一致性；X64ARM_METAL_POSITION_INVARIANCE=0 僅供診斷對照。
// D3D 著色器的 Metal 編譯選項。同一管線的各變體（細分、幾何、片段連結、alpha test、非點、雙來源）必須共用同一設定，
// 位置計算才會一致（深度預先繪製與 EQUAL 測試），所以由產生它的著色器路徑決定（modern）。
// 預設：現代路徑（D3D12 的 SM5.1／DXIL，經 vkd3d→SPIRV-Cross）用 relaxed 數學模式＋快速超越函式——允許 FMA 與重組、保留 Inf／NaN，
// D3D 的 precise 運算由 SPIRV-Cross 包成 optnone 輔助函式不受影響；電馭叛客 Benchmark 熱快取 44.7→54.2（+21%），對馬 GPU −12%。
// D3D9／HLSLcc 舊路徑維持 safe（fastMathEnabled=NO），待各自回歸後再決定。
// X64ARM_METAL_SHADER_MATH 覆寫全部路徑：safe、fastfunc（safe＋快速超越函式）、relaxed。
inline MTLCompileOptions* metal_d3d_compile_options(bool modern=false){
    static const int forced=[] {const auto* text=std::getenv("X64ARM_METAL_SHADER_MATH");if(!text)return -1;const std::string_view value(text);return value=="relaxed"?2:value=="fastfunc"?1:0;}();
    const int mode=forced>=0?forced:modern?2:0;
    auto options=[MTLCompileOptions new];
    if(@available(macOS 15.0,*)){
        if(mode){options.mathMode=mode==2?MTLMathModeRelaxed:MTLMathModeSafe;options.mathFloatingPointFunctions=MTLMathFloatingPointFunctionsFast;return options;}
    }
    options.fastMathEnabled=NO;return options;
}
struct MetalLibraryCompilation {
    std::string source;MTLCompileOptions* options{};
};
MetalLibraryCompilation metal_library_compilation(std::string_view source,MTLCompileOptions* options){
    static const bool enabled=[] {const auto* text=std::getenv("X64ARM_METAL_POSITION_INVARIANCE");return !text || std::string_view(text)!="0";}();
    if(!enabled)return {};
    static const std::regex vertex(R"(\bvertex\s+)");
    if(!std::regex_search(source.begin(),source.end(),vertex))return {};
    static const std::regex position(R"(\[\[\s*position\s*\]\])");
    std::string prepared=std::regex_replace(std::string(source),position,"[[position, invariant]]");
    if(prepared==source && source.find("invariant")==std::string_view::npos)return {};
    MTLCompileOptions* copy=options?[options copy]:[MTLCompileOptions new];copy.preserveInvariance=YES;
    static const bool trace=std::getenv("X64ARM_TRACE_GRAPHICS_ERRORS")!=nullptr;
    static std::atomic<unsigned> reported{0};
    if(trace && reported.fetch_add(1,std::memory_order_relaxed)<4)std::fprintf(stderr,"metal_position_invariance prepared=1 preserve=1\n");
    return {std::move(prepared),copy};
}
struct MetalLibraryCacheRequest {
    std::string key,prefix;std::array<unsigned char,CC_SHA256_DIGEST_LENGTH> digest{};
    unsigned language{},math{},functions{};bool invariance{};
    explicit operator bool()const{return !key.empty();}
};
std::array<unsigned char,CC_SHA256_DIGEST_LENGTH> metal_library_digest(std::string_view bytes){
    std::array<unsigned char,CC_SHA256_DIGEST_LENGTH> result{};
    CC_SHA256(bytes.data(),static_cast<CC_LONG>(bytes.size()),result.data());return result;
}
MetalLibraryCacheRequest metal_library_request(std::string_view source,MTLCompileOptions* options){
    MetalLibraryCacheRequest request;
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_METAL_LIBRARY_CACHE");return !value || std::string_view(value)!="0";}();
    if(!enabled || source.empty() || source.size()>16U*1024U*1024U)return request;
    if(@available(macOS 15.0,*)){
        if(!options)options=[MTLCompileOptions new];
        // 其餘設定尚無離線對應時直接沿用完整原生路徑，不遺漏巨集、外部函式或連結契約。
        if(options.preprocessorMacros.count || options.libraries.count || options.libraryType!=MTLLibraryTypeExecutable ||
           options.optimizationLevel!=MTLLibraryOptimizationLevelDefault ||
           options.compileSymbolVisibility!=MTLCompileSymbolVisibilityDefault || options.allowReferencingUndefinedSymbols)return request;
        request.language=static_cast<unsigned>(options.languageVersion);request.math=static_cast<unsigned>(options.mathMode);
        request.functions=static_cast<unsigned>(options.mathFloatingPointFunctions);
        request.invariance=options.preserveInvariance;
        // 保留既有預設選項的快取身分；位置一致性另列入編譯契約，不與未啟用的 IR 混用。
        request.prefix="x64arm-metal-library-v1\n"+std::to_string(request.language)+":"+std::to_string(request.math)+":"+std::to_string(request.functions)+(request.invariance?":invariant":"")+"\n"+
            std::string(NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String)+"\n";
        auto identity=request.prefix;identity.append(source);request.digest=metal_library_digest(identity);
        static constexpr char hex[]="0123456789abcdef";
        for(const auto byte:request.digest){request.key.push_back(hex[byte>>4]);request.key.push_back(hex[byte&15]);}
    }
    return request;
}
std::filesystem::path metal_library_cache_directory(){
    if(const auto* path=std::getenv("X64ARM_METAL_LIBRARY_CACHE_DIR");path && *path)return path;
    const auto root=generated_resource_cache_directory();return root.empty()?root:root/"metal-libraries-v1";
}
void metal_library_cache_note(bool hit,bool invalid=false){
    static const bool trace=std::getenv("X64ARM_METAL_LIBRARY_CACHE_TRACE")!=nullptr;
    if(!trace)return;
    static std::atomic<std::uint64_t> hits{},misses{},invalids{};
    if(hit)++hits;else ++misses;if(invalid)++invalids;
    const auto count=hits.load()+misses.load();
    if(count==1 || count%256==0)std::fprintf(stderr,"metal_library_cache requests=%llu hits=%llu misses=%llu invalid=%llu\n",
        static_cast<unsigned long long>(count),static_cast<unsigned long long>(hits.load()),
        static_cast<unsigned long long>(misses.load()),static_cast<unsigned long long>(invalids.load()));
}
id<MTLLibrary> metal_library_cached(id<MTLDevice> device,const MetalLibraryCacheRequest& request){
    if(!request)return nil;
    const auto directory=metal_library_cache_directory();if(directory.empty())return nil;
    const auto path=directory/(request.key+".ajmetallib");
    const int fd=::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0){metal_library_cache_note(false);return nil;}
    struct stat metadata{};std::vector<unsigned char> bytes;
    if(::fstat(fd,&metadata)==0 && S_ISREG(metadata.st_mode) && metadata.st_uid==::geteuid() &&
       metadata.st_size>72 && metadata.st_size<=64*1024*1024){
        bytes.resize(static_cast<std::size_t>(metadata.st_size));std::size_t offset=0;
        while(offset<bytes.size()){const auto count=::read(fd,bytes.data()+offset,bytes.size()-offset);if(count<=0)break;offset+=static_cast<std::size_t>(count);}
        if(offset!=bytes.size())bytes.clear();
    }
    ::close(fd);
    if(bytes.size()<=72 || std::memcmp(bytes.data(),"AJMTLIB1",8) ||
       std::memcmp(bytes.data()+8,request.digest.data(),32)) {metal_library_cache_note(false,true);return nil;}
    const std::string_view payload(reinterpret_cast<const char*>(bytes.data()+72),bytes.size()-72);
    const auto digest=metal_library_digest(payload);
    if(std::memcmp(bytes.data()+40,digest.data(),32)){metal_library_cache_note(false,true);return nil;}
    NSError* error=nil;
    auto data=dispatch_data_create(payload.data(),payload.size(),nullptr,DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    auto library=[device newLibraryWithData:data error:&error];metal_library_cache_note(library!=nil,library==nil);return library;
}
void metal_library_capture(const MetalLibraryCacheRequest& request,std::string_view source){
    static const char* directory=std::getenv("X64ARM_METAL_LIBRARY_CAPTURE_DIR");
    if(!request || !directory || !*directory)return;
    try{
        const std::filesystem::path root(directory);std::error_code error;std::filesystem::create_directories(root,error);if(error)return;
        const auto metal=root/(request.key+".metal"),metadata=root/(request.key+".json");if(std::filesystem::exists(metadata,error))return;
        auto text=[NSData dataWithBytes:source.data() length:source.size()];
        if(![text writeToFile:[NSString stringWithUTF8String:metal.c_str()] atomically:YES])return;
        auto values=@{@"format":@1,@"key":[NSString stringWithUTF8String:request.key.c_str()],
            @"identity_prefix":[NSString stringWithUTF8String:request.prefix.c_str()],@"language":@(request.language),
            @"math":@(request.math),@"functions":@(request.functions),@"preserve_invariance":@(request.invariance)};
        auto json=[NSJSONSerialization dataWithJSONObject:values options:0 error:nil];
        [json writeToFile:[NSString stringWithUTF8String:metadata.c_str()] atomically:YES];
    }catch(const std::exception&){} // 診斷檔案失敗不影響執行。
}
id<MTLLibrary> metal_library_with_source(id<MTLDevice> device,std::string_view source,MTLCompileOptions* options,NSError** error){
    const auto compilation=metal_library_compilation(source,options);
    if(!compilation.source.empty()){source=compilation.source;options=compilation.options;}
    const auto request=metal_library_request(source,options);
    if(auto library=metal_library_cached(device,request))return library;
    metal_library_capture(request,source);
    return [device newLibraryWithSource:[[NSString alloc] initWithBytes:source.data() length:source.size() encoding:NSUTF8StringEncoding] options:options error:error];
}

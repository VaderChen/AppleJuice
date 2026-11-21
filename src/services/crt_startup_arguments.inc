std::uint32_t WindowsServices::configure_crt_argv(GuestState& state,GuestMemory& memory,
                                               bool wide,bool expand){
    const unsigned index=wide?1U:0U;
    const unsigned unit=x86_?4U:8U;
    const auto bits=static_cast<std::uint8_t>(unit*8U);
    const auto argv_cell=GuestAddress{wide?runtime_.wargv_cell:runtime_.argv_cell};
    std::uint32_t argc=runtime_.initial_argc;
    auto argv=runtime_.initial_argv[index];
    std::uint64_t allocated=0;
    if(expand){
        try{
            std::vector<std::u16string> arguments;
            for(auto input:parse_windows_arguments(read_utf16(memory,command_line_address_))){
                auto separator=input.find_last_of(u"\\/");
                if(separator==input.npos && input.size()>=2 && input[1]==u':')separator=1;
                const auto prefix=separator==input.npos?0U:separator+1;
                const auto pattern=std::u16string_view(input).substr(prefix);
                const auto first=arguments.size();
                if(pattern.find_first_of(u"*?")!=pattern.npos){
                    try{
                        const auto path=full_windows_environment_path(input,runtime_.current_directory,runtime_.environment);
                        if(auto [error,search]=search_directory(utf16_to_utf8(path));!error){
                            for(const auto& match:search.entries){
                                if(match.name=="." || match.name=="..")continue;
                                arguments.push_back(input.substr(0,prefix)+utf8_to_utf16(match.name));
                            }
                        }
                    }catch(const std::invalid_argument&){
                        // Windows 對無法列舉的路徑保留原樣的引數。
                    }
                }
                if(first==arguments.size())arguments.push_back(std::move(input));
            }
            if(arguments.size()>=static_cast<std::size_t>(INT32_MAX))return 12;
            std::vector<std::vector<std::uint8_t>> strings;
            strings.reserve(arguments.size());
            std::uint64_t size=(arguments.size()+1U)*unit;
            for(const auto& argument:arguments){
                auto bytes=wide?utf16_bytes(argument):bytes_of(utf16_to_utf8(argument));
                if(size>UINT64_MAX-bytes.size())return 12;
                size+=bytes.size();
                strings.push_back(std::move(bytes));
            }
            allocated=allocate_heap(memory,size);
            if(!allocated)return 12;
            argv=allocated;
            argc=static_cast<std::uint32_t>(strings.size());
            auto cursor=argv+(strings.size()+1U)*unit;
            for(std::size_t n=0;n<strings.size();++n){
                memory.store(GuestAddress{argv+n*unit},bits,cursor);
                memory.write(GuestAddress{cursor},strings[n]);
                cursor+=strings[n].size();
            }
            memory.store(GuestAddress{argv+strings.size()*unit},bits,0);
        }catch(const std::bad_alloc&){
            if(allocated)(void)free_heap(memory,allocated,state);
            return 12;
        }catch(...){
            if(allocated)(void)free_heap(memory,allocated,state);
            throw;
        }
    }
    // 先完整建立新向量，再發布至 CRT 資料格。僅釋放本例程建立的舊向量。
    memory.store(GuestAddress{runtime_.argc_cell},32,argc);
    memory.store(argv_cell,bits,argv);
    const auto previous=std::exchange(runtime_.expanded_argv[index],allocated);
    if(previous)(void)free_heap(memory,previous,state);
    return 0;
}

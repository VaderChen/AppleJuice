    if(name=="DllRegisterServer" || name=="DllUnregisterServer"){
        const WindowsCall args(state,memory,service);
        const bool install=name=="DllRegisterServer";
        const std::uint64_t root=x86_?0x80000000ULL:0xffffffff80000000ULL;
        auto host=[&](std::string_view symbol,std::initializer_list<std::uint64_t> params){
            return static_cast<unsigned>(nested_service(state,memory,"advapi32.dll",symbol,params));
        };
        auto guid=[](unsigned id){return windows_guid_text(ole_standard_guid(id));};
        struct Entry{std::u16string key,name,value;};
        std::vector<Entry> entries;
        // 類別與 Proxy 的登錄來自 avifil32.idl，格式與壓縮器對應來自 avifile.rgs。
        constexpr std::pair<unsigned,std::u16string_view> classes[]={
            {0x20000,u"Microsoft AVI Files"},{0x20001,u"AVI Compressed Stream"},{0x20003,u"Microsoft Wave File"},
            {0x2000d,u"IAVIStream & IAVIFile Proxy"},{0x2000f,u"ACM Compressed Audio Stream"}};
        for(const auto& [id,label]:classes){
            const auto key=u"CLSID\\"+guid(id);
            entries.push_back({key,{},std::u16string(label)});
            entries.push_back({key+u"\\InprocServer",{},u"avifile.dll"});
            entries.push_back({key+u"\\InprocServer32",{},u"C:\\Windows\\System32\\avifil32.dll"});
            entries.push_back({key+u"\\InprocServer32",u"ThreadingModel",u"Both"});
        }
        for(unsigned id:{0x20020U,0x20021U}){
            const auto key=u"Interface\\"+guid(id);
            entries.push_back({key,{},id==0x20020?u"IAVIFile":u"IAVIStream"});
            entries.push_back({key+u"\\NumMethods",{},id==0x20020?u"10":u"14"});
            entries.push_back({key+u"\\ProxyStubClsid32",{},guid(0x2000d)});
        }
        constexpr std::pair<std::u16string_view,unsigned> handlers[]={
            {u"Compressors\\auds",0x2000f},{u"Compressors\\vids",0x20001},{u"Extensions\\AU",0x20003},
            {u"Extensions\\AVI",0x20000},{u"Extensions\\WAV",0x20003},{u"RIFFHandlers\\AVI",0x20000},{u"RIFFHandlers\\WAVE",0x20003}};
        for(const auto& [suffix,id]:handlers)entries.push_back({u"AVIFile\\"+std::u16string(suffix),{},guid(id)});
        std::set<std::u16string,std::greater<>> cleanup;
        unsigned status=0;
        for(const auto& item:entries){
            GuestScratch scratch(*this,memory,state);
            const auto path=scratch.text(item.key),value_name=item.name.empty()?0:scratch.text(item.name);
            if(!path || (!item.name.empty() && !value_name)){status=14;break;}
            if(install){
                const auto data=scratch.text(item.value);
                if(!data){status=14;break;}
                status=host("RegSetKeyValueW",{root,path,value_name,1,data,(item.value.size()+1)*2});
            }else{
                // 撤銷時保留已由其他處理器接管的值，再由深至淺清理空機碼。
                const auto fields=scratch.allocate(8),data=scratch.allocate((item.value.size()+1)*2);
                if(!fields || !data){status=14;break;}
                memory.store(GuestAddress{fields},32,(item.value.size()+1)*2);
                status=host("RegGetValueW",{root,path,value_name,0xffff,fields+4,data,fields});
                if(!status && memory.load(GuestAddress{fields+4},32)==1 &&
                   memory.load(GuestAddress{fields},32)==(item.value.size()+1)*2 &&
                   read_utf16(memory,GuestAddress{data})==item.value)
                    status=host("RegDeleteKeyValueW",{root,path,value_name});
                if(status==2 || status==3 || status==234)status=0;
                for(auto key=item.key;key.find(u'\\')!=key.npos;key.resize(key.rfind(u'\\')))cleanup.insert(key);
            }
            if(status)break;
        }
        for(const auto& key:cleanup){
            if(status)break;
            GuestScratch scratch(*this,memory,state);
            const auto path=scratch.text(key),buf=scratch.allocate(16);
            if(!path || !buf){status=14;break;}
            status=host("RegOpenKeyExW",{root,path,0,0x20019,buf});
            if(status==2 || status==3){status=0;continue;}
            if(status)break;
            const auto handle=memory.load(GuestAddress{buf},x86_?32:64);
            status=host("RegQueryInfoKeyW",{handle,0,0,0,buf+8,0,0,buf+12,0,0,0,0});
            host("RegCloseKey",{handle});
            if(!status && !memory.load(GuestAddress{buf+8},32) && !memory.load(GuestAddress{buf+12},32))
                status=host("RegDeleteKeyW",{root,path});
        }
        args.return_to(state,status?0x80070000U|(status&0xffffU):0U);return std::nullopt;
    }

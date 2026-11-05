// 客體自行實作的 ITypeLib 也依 COM 契約讀取 metadata，不依賴內建物件身分。
return service_callback::invoke<std::optional<std::uint32_t>>(
    this,threads_.current,state,memory,"RegisterTypeLib",
    [this](GuestState& s,GuestMemory& m,std::uint64_t target,std::vector<std::uint64_t> values,std::uint64_t id){start_callbacks(s,m,{{target,std::move(values)}},Continuation::NestedResult,id);},
    [this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
        const auto method=[&](std::uint64_t object,unsigned slot){return m.load(GuestAddress{m.load(GuestAddress{object},bits)+slot*unit},bits);};
        struct Cleanup {WindowsServices& self;GuestState& state;GuestMemory& memory;std::uint64_t scratch{};std::vector<std::uint64_t> strings{};
            ~Cleanup(){for(const auto text:strings)self.free_bstr(state,memory,text);if(scratch){self.runtime_.type_libraries.views.erase(scratch);(void)self.free_heap(memory,scratch,state);}}} cleanup{*this,s,m};
        std::vector<GuestCallback> releases;
        std::uint32_t result=0;
        try{
            if(!call(1))failure(0x80070057U);const auto path=read_utf16(m,GuestAddress{call(1)});if(path.empty())failure(0x80070057U);
            const auto help=call(2)?read_utf16(m,GuestAddress{call(2)}):std::u16string{};
            cleanup.scratch=allocate_heap(m,64);if(!cleanup.scratch)failure(nomemory);const auto scratch=cleanup.scratch;
            const auto clear=[&](){m.write(GuestAddress{scratch},std::array<std::uint8_t,64>{});};
            const auto checked=[&](std::uint64_t value){if(value&0x80000000U)failure(static_cast<std::uint32_t>(value));};
            const auto text=[&](unsigned index){const auto pointer=m.load(GuestAddress{scratch+index*unit},bits);if(!pointer)return std::string{};cleanup.strings.push_back(pointer);return utf16_to_utf8(read_utf16(m,GuestAddress{pointer}));};
            auto model=std::make_shared<typelib::Library>();clear();
            checked(co_await service_callback::call(method(call(0),7),{call(0),scratch}));
            const auto attr=m.load(GuestAddress{scratch},bits);if(!attr)failure(invalid);
            releases.push_back({method(call(0),12),{call(0),attr}});
            m.read(GuestAddress{attr},model->guid);model->lcid=static_cast<std::uint32_t>(m.load(GuestAddress{attr+16},32));
            model->system=static_cast<std::uint16_t>(m.load(GuestAddress{attr+20},32));model->major=static_cast<std::uint16_t>(m.load(GuestAddress{attr+24},16));
            model->minor=static_cast<std::uint16_t>(m.load(GuestAddress{attr+26},16));model->flags=static_cast<std::uint16_t>(m.load(GuestAddress{attr+28},16));
            clear();checked(co_await service_callback::call(method(call(0),9),{call(0),UINT32_MAX,scratch,0,0,scratch+unit}));
            model->doc.name=text(0);model->help_file=text(1);
            const auto count=co_await service_callback::call(method(call(0),3),{call(0)});if(count>65536)failure(invalid);
            for(std::uint64_t i=0;i<count;++i){
                clear();checked(co_await service_callback::call(method(call(0),4),{call(0),i,scratch}));const auto info=m.load(GuestAddress{scratch},bits);if(!info)failure(invalid);
                releases.push_back({method(info,2),{info}});
                clear();checked(co_await service_callback::call(method(info,3),{info,scratch}));const auto data=m.load(GuestAddress{scratch},bits);if(!data)failure(invalid);
                releases.push_back({method(info,19),{info,data}});
                typelib::TypeInfo type;m.read(GuestAddress{data},type.guid);const unsigned instance=x86_?36U:40U;
                type.kind=static_cast<std::uint16_t>(m.load(GuestAddress{data+instance+4},32));type.flags=static_cast<std::uint16_t>(m.load(GuestAddress{data+instance+18},16));
                clear();checked(co_await service_callback::call(method(info,12),{info,UINT32_MAX,scratch,0,0,0}));type.doc.name=text(0);model->types.push_back(std::move(type));
                for(unsigned n=0;n<2;++n){auto release=std::move(releases.back());releases.pop_back();(void)co_await service_callback::call(release.target,std::move(release.arguments));}
            }
            runtime_.type_libraries.views.emplace(scratch,TypeLibraryView{model,utf16_to_utf8(path),scratch,-1,1});
            register_type_library(scratch,path,help);
        }catch(const typelib::Error& error){result=error.code;}catch(const MemoryFault&){result=0x80070057U;}catch(const std::bad_alloc&){result=nomemory;}
        while(!releases.empty()){auto release=std::move(releases.back());releases.pop_back();(void)co_await service_callback::call(release.target,std::move(release.arguments));}
        call.return_to(s,result);co_return std::nullopt;
    });

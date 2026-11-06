const auto byte_stream_begin=[this](GuestState& s,GuestMemory& m,std::uint64_t target,std::vector<std::uint64_t> args,std::uint64_t id){
    start_callbacks(s,m,{{target,std::move(args)}},Continuation::NestedResult,id);
};
const auto external_stream_metadata=[&](std::uint64_t pointer){
    require(pointer!=0,pointer_error);
    return service_callback::invoke<std::shared_ptr<FileSourceState>>(this,threads_.current,state,memory,"mf-source-metadata",byte_stream_begin,
        [this,pointer](GuestState& s,GuestMemory& m)->service_callback::Task<std::shared_ptr<FileSourceState>>{
            const auto scratch=allocate_heap(m,65568);if(!scratch)throw std::bad_alloc{};
            struct Scratch {WindowsServices& owner;GuestState& state;GuestMemory& memory;std::uint64_t pointer;~Scratch(){(void)owner.free_heap(memory,pointer,state);}} cleanup{*this,s,m,scratch};
            GuestByteStreamAccess access{m,pointer,scratch,static_cast<std::uint8_t>(x86_?32:64)};
            GuestByteStreamAccess::check(co_await service_callback::call(access.method(3),{pointer,scratch}));
            if((m.load(GuestAddress{scratch},32)&5)!=5)throw Failure{0xc00d36b2U};
            const auto size=co_await access.position(4),base=co_await access.position(6);if(base>size)throw Failure{0xc00d36c4U};
            std::array<std::uint8_t,8> head{};if(size-base>=8)co_await access.read(base,head);
            if(mf::mp4_signature(head)){
                auto movie=std::make_shared<FileSourceState>();movie->base=base;movie->external_stream=pointer;
                try{movie->movie=co_await mf::read_mp4_movie_async(size-base,[&](std::uint64_t at,std::span<std::uint8_t> bytes){return access.read(base+at,bytes);});}
                catch(const std::invalid_argument&){throw Failure{0xc00d36c4U};}
                co_return movie;
            }
            const auto parsed=co_await parse_pcm_wave_stream_async(size-base,[&](std::uint64_t at,std::span<std::uint8_t> bytes){return access.read(base+at,bytes);});
            if(!parsed)throw Failure{0xc00d36c4U};auto source=std::make_shared<FileSourceState>();source->wave=*parsed;source->base=base;source->external_stream=pointer;co_return source;
        });
};
const auto external_stream_samples=[&](std::uint64_t pointer,std::uint64_t at,std::size_t size){
    return service_callback::invoke<std::vector<std::uint8_t>>(this,threads_.current,state,memory,"mf-source-read",byte_stream_begin,
        [this,pointer,at,size](GuestState& s,GuestMemory& m)->service_callback::Task<std::vector<std::uint8_t>>{
            const auto scratch=allocate_heap(m,65568);if(!scratch)throw std::bad_alloc{};
            struct Scratch {WindowsServices& owner;GuestState& state;GuestMemory& memory;std::uint64_t pointer;~Scratch(){(void)owner.free_heap(memory,pointer,state);}} cleanup{*this,s,m,scratch};
            GuestByteStreamAccess access{m,pointer,scratch,static_cast<std::uint8_t>(x86_?32:64)};std::vector<std::uint8_t> bytes(size);co_await access.read(at,bytes);co_return bytes;
        });
};

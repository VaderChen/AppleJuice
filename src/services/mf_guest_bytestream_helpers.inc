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
            // 開頭 16 bytes 足以辨認 ASF Header Object GUID；MP4 與 RIFF 只看前 8 bytes。
            std::array<std::uint8_t,16> head{};if(size-base>=8)co_await access.read(base,std::span<std::uint8_t>(head).first(size-base>=16?16:8));
            // 診斷：X64ARM_TRACE_MEDIA_ERRORS=1 時列出外部串流的大小與開頭位元組，辨認尚未支援的容器。
            if(const auto* trace=std::getenv("X64ARM_TRACE_MEDIA_ERRORS");trace && std::string_view(trace)=="1")
                std::fprintf(stderr,"[media-foundation] 外部串流 size=%llu base=%llu head=%02x %02x %02x %02x %02x %02x %02x %02x\n",static_cast<unsigned long long>(size),static_cast<unsigned long long>(base),head[0],head[1],head[2],head[3],head[4],head[5],head[6],head[7]);
            // 診斷：X64ARM_TRACE_MEDIA_DUMP=<檔案> 時把外部串流開頭（最多 256 KiB）寫到該檔，供辨認容器內的編碼格式。
            if(const auto* dump=std::getenv("X64ARM_TRACE_MEDIA_DUMP");dump && *dump){
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::min<std::uint64_t>(size-base,256U*1024U)));
                for(std::size_t at=0;at<bytes.size();at+=32768)co_await access.read(base+at,std::span<std::uint8_t>(bytes).subspan(at,std::min<std::size_t>(32768,bytes.size()-at)));
                if(auto* file=std::fopen(dump,"wb")){std::fwrite(bytes.data(),1,bytes.size(),file);std::fclose(file);}
            }
            if(mf::asf_signature(head)){
                auto movie=std::make_shared<FileSourceState>();movie->base=base;movie->external_stream=pointer;
                try{movie->asf=co_await mf::read_asf_movie_async(size-base,[&](std::uint64_t at,std::span<std::uint8_t> bytes){return access.read(base+at,bytes);});}
                catch(const std::invalid_argument&){throw Failure{mf::asf_invalid_data};}
                co_return movie;
            }
            if(mf::mp4_signature(std::span<const std::uint8_t,16>(head).first<8>())){
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
// 包裝客體 IStream 的 ByteStream：每個 API 以一個續行同步呼叫客體方法並取回 HRESULT 與輸出。
// 同一個 API 進入點只能建立一個同種續行；需要多個客體呼叫時在同一個續行內依序完成。
enum class StreamOp {Open,OpenUnknown,Stat,SetSize,Read,Write,Commit};
const auto guest_stream_io=[&](std::uint64_t pointer,StreamOp op,std::uint64_t position=0,std::uint64_t buffer=0,std::uint32_t count=0){
    return service_callback::invoke<StreamOutcome>(this,threads_.current,state,memory,"mf-istream",byte_stream_begin,
        [this,pointer,op,position,buffer,count](GuestState& s,GuestMemory& m)->service_callback::Task<StreamOutcome>{
            const auto scratch=allocate_heap(m,128);if(!scratch)throw std::bad_alloc{};
            struct Scratch {WindowsServices& owner;GuestState& state;GuestMemory& memory;std::uint64_t pointer;~Scratch(){(void)owner.free_heap(memory,pointer,state);}} cleanup{*this,s,m,scratch};
            const auto width=static_cast<std::uint8_t>(x86_?32:64);
            GuestStreamAccess access{m,pointer,scratch,width};StreamOutcome r;
            switch(op){
            case StreamOp::OpenUnknown:{
                // IUnknown 能查出 IStream 時與 MFCreateMFByteStreamOnStream 同一條路；查詢取得的引用即包裝持有的引用。
                m.write(GuestAddress{scratch+80},mf::iid_ComStream);m.store(GuestAddress{scratch+96},width,0);
                r.status=co_await service_callback::call(access.method(0),{pointer,scratch+80,scratch+96});
                const auto stream=m.load(GuestAddress{scratch+96},width);if((r.status&0x80000000U) || !stream)co_return r;
                access.pointer=stream;(void)co_await access.seek(0);r=co_await access.describe();r.object=stream;break;
            }
            // 建立時把 IStream 定位到開頭並讀取模式；兩者失敗都不影響建立。
            case StreamOp::Open:(void)co_await access.seek(0);r=co_await access.describe();break;
            case StreamOp::Stat:r=co_await access.describe();break;
            case StreamOp::SetSize:r.status=co_await access.resize(position);break;
            case StreamOp::Read:case StreamOp::Write:r=co_await access.transfer(op==StreamOp::Write,position,buffer,count);break;
            case StreamOp::Commit:r.status=co_await access.commit();break;
            }
            co_return r;
        });
};
// MFCreateStreamOnMFByteStream 的 IStream 包裝：把 IStream 方法轉成對 IMFByteStream 的呼叫。
// 引擎自己的 ByteStream 與客體實作走同一份 vtable；任何失敗的 HRESULT 以 Failure 交回。
enum class WrapOp {Read,Write,Seek,SetSize,Commit,Stat,CopyTo};
struct WrapOutcome {std::uint64_t value{},second{};std::uint32_t capabilities{};};
const auto wrapped_stream_io=[&](std::uint64_t pointer,WrapOp op,std::uint64_t first=0,std::uint64_t second=0){
    return service_callback::invoke<WrapOutcome>(this,threads_.current,state,memory,"mf-stream-wrapper",byte_stream_begin,
        [this,pointer,op,first,second](GuestState& s,GuestMemory& m)->service_callback::Task<WrapOutcome>{
            const auto scratch=allocate_heap(m,op==WrapOp::CopyTo?65568:64);if(!scratch)throw std::bad_alloc{};
            struct Scratch {WindowsServices& owner;GuestState& state;GuestMemory& memory;std::uint64_t pointer;~Scratch(){(void)owner.free_heap(memory,pointer,state);}} cleanup{*this,s,m,scratch};
            const auto width=static_cast<std::uint8_t>(x86_?32:64);
            GuestByteStreamAccess access{m,pointer,scratch,width};WrapOutcome r;
            const auto check=[](std::uint32_t status){GuestByteStreamAccess::check(status);};
            const auto wide=[&](std::uint64_t value)->std::vector<std::uint64_t>{if(width==32)return {pointer,static_cast<std::uint32_t>(value),value>>32};return {pointer,value};};
            switch(op){
            case WrapOp::Read:case WrapOp::Write:
                m.store(GuestAddress{scratch},32,0);
                check(co_await service_callback::call(access.method(op==WrapOp::Read?9:12),{pointer,first,second,scratch}));
                r.value=m.load(GuestAddress{scratch},32);if(r.value>second)throw Failure{0x80004005U};break;
            case WrapOp::Seek:{
                // first＝位移、second＝STREAM_SEEK_*；結尾基準取自 GetLength。
                std::uint64_t base=0;if(second==1)base=co_await access.position(6);else if(second==2)base=co_await access.position(4);
                const auto move=std::bit_cast<std::int64_t>(first);std::uint64_t target=0;
                if(move<0){const auto amount=UINT64_C(0)-first;if(amount>base)throw Failure{0x80030001U};target=base-amount;}
                else {if(first>static_cast<std::uint64_t>(INT64_MAX)-std::min<std::uint64_t>(base,INT64_MAX))throw Failure{0x80030001U};target=base+first;}
                check(co_await service_callback::call(access.method(7),wide(target)));r.value=target;break;
            }
            case WrapOp::SetSize:check(co_await service_callback::call(access.method(5),wide(first)));break;
            case WrapOp::Commit:check(co_await service_callback::call(access.method(16),{pointer}));break;
            case WrapOp::Stat:
                m.store(GuestAddress{scratch},32,0);check(co_await service_callback::call(access.method(3),{pointer,scratch}));
                r.capabilities=static_cast<std::uint32_t>(m.load(GuestAddress{scratch},32));r.value=co_await access.position(4);break;
            case WrapOp::CopyTo:{
                // first＝目的 IStream、second＝位元組數；value／second 分別是已讀與已寫的位元組數。
                const auto write=m.load(GuestAddress{m.load(GuestAddress{first},width)+4*(width/8U)},width);const auto total=second;
                while(r.value<total){
                    const auto count=std::min<std::uint64_t>(total-r.value,65536);m.store(GuestAddress{scratch+16},32,0);
                    check(co_await service_callback::call(access.method(9),{pointer,scratch+32,count,scratch+16}));
                    const auto received=m.load(GuestAddress{scratch+16},32);if(!received)break;if(received>count)throw Failure{0x80004005U};
                    r.value+=received;m.store(GuestAddress{scratch+16},32,0);
                    check(co_await service_callback::call(write,{first,scratch+32,received,scratch+16}));
                    const auto written=std::min<std::uint64_t>(m.load(GuestAddress{scratch+16},32),received);r.second+=written;
                    if(written<received)throw Failure{0x80030070U};
                }
                break;
            }
            }
            co_return r;
        });
};

// 位於 MF 共用狀態命名空間內；透過現有客體回呼續行，不另建執行緒或私有排程器。
struct GuestByteStreamAccess {
    GuestMemory& memory;std::uint64_t pointer{},scratch{};std::uint8_t bits{};
    std::uint64_t method(unsigned slot)const {return memory.load(GuestAddress{memory.load(GuestAddress{pointer},bits)+slot*(bits/8U)},bits);}
    static void check(std::uint32_t status){if(status&0x80000000U)throw Failure{status};}
    service_callback::Task<std::uint64_t> position(unsigned slot){
        check(co_await service_callback::call(method(slot),{pointer,scratch}));co_return memory.load(GuestAddress{scratch},64);
    }
    service_callback::Task<unsigned> read(std::uint64_t offset,std::span<std::uint8_t> bytes){
        std::vector<std::uint64_t> args{pointer,offset};if(bits==32){args[1]=static_cast<std::uint32_t>(offset);args.push_back(offset>>32);}
        check(co_await service_callback::call(method(7),std::move(args)));
        while(!bytes.empty()){
            const auto count=std::min<std::size_t>(bytes.size(),65536);memory.store(GuestAddress{scratch+16},32,0);
            check(co_await service_callback::call(method(9),{pointer,scratch+32,count,scratch+16}));
            const auto received=memory.load(GuestAddress{scratch+16},32);if(!received || received>count)throw Failure{0x80070026U};
            memory.read(GuestAddress{scratch+32},bytes.first(received));bytes=bytes.subspan(received);
        }co_return 0;
    }
};
// 客體 IStream 的存取；ole32／shlwapi 建立的宿主 IStream 也經同一份 vtable 進入其服務。
// status 是最後一個客體方法的 HRESULT，由呼叫端決定如何回報。
struct StreamOutcome {std::uint32_t status{};std::uint64_t value{},object{};std::uint32_t mode{};};
struct GuestStreamAccess {
    GuestMemory& memory;std::uint64_t pointer{},scratch{};std::uint8_t bits{};
    std::uint64_t method(unsigned slot)const {return memory.load(GuestAddress{memory.load(GuestAddress{pointer},bits)+slot*(bits/8U)},bits);}
    // x86 的 64-bit 參數占兩個堆疊欄位。
    std::vector<std::uint64_t> wide(std::uint64_t value)const {
        if(bits==32)return {pointer,static_cast<std::uint32_t>(value),value>>32};return {pointer,value};
    }
    service_callback::Call seek(std::uint64_t position){auto args=wide(position);args.push_back(0);args.push_back(0);return service_callback::call(method(5),std::move(args));}
    service_callback::Call resize(std::uint64_t size){return service_callback::call(method(6),wide(size));}
    service_callback::Call commit(){return service_callback::call(method(8),{pointer,0});}
    // Stat(STATFLAG_NONAME)：value 為 cbSize，mode 為 grfMode。
    service_callback::Task<StreamOutcome> describe(){
        const std::size_t size=bits==32?72:80;memory.fill(GuestAddress{scratch},0,size);StreamOutcome r;
        r.status=co_await service_callback::call(method(12),{pointer,scratch,1});
        if(!(r.status&0x80000000U)){r.value=memory.load(GuestAddress{scratch+(bits==32?8U:16U)},64);r.mode=static_cast<std::uint32_t>(memory.load(GuestAddress{scratch+(bits==32?40U:48U)},32));}
        co_return r;
    }
    // 先定位到 ByteStream 自己維護的位置，再把呼叫端的 buffer 直接交給客體 Read／Write。
    service_callback::Task<StreamOutcome> transfer(bool writing,std::uint64_t position,std::uint64_t buffer,std::uint32_t count){
        StreamOutcome r;r.status=co_await seek(position);if(r.status&0x80000000U)co_return r;
        memory.store(GuestAddress{scratch},32,0);
        r.status=co_await service_callback::call(method(writing?4:3),{pointer,buffer,count,scratch});
        if(!(r.status&0x80000000U)){r.value=memory.load(GuestAddress{scratch},32);if(r.value>count)throw Failure{0x80004005U};}
        co_return r;
    }
};

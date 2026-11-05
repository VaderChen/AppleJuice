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

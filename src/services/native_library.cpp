#include "x64arm/native_library.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <sstream>
#include <zlib.h>

namespace x64arm {
namespace {
constexpr WindowsServiceDescriptor zlib_functions[]{
    {"zlib1.dll","compress",4,0,0,8,true},
    {"zlib1.dll","compress2",5,0,0,24,true},
    {"zlib1.dll","compressBound",1,0,0,1,true},
    {"zlib1.dll","uncompress",4,0,0,8,true},
    {"zlib1.dll","uncompress2",4,0,0,0,true},
    {"zlib1.dll","adler32",3,0,0,5,true},
    {"zlib1.dll","adler32_z",3,0,0,1,true},
    {"zlib1.dll","crc32",3,0,0,5,true},
    {"zlib1.dll","crc32_z",3,0,0,1,true},
    {"zlib1.dll","adler32_combine",3,0,0,7,true},
    {"zlib1.dll","adler32_combine64",3,4,0,3,true},
    {"zlib1.dll","crc32_combine",3,0,0,7,true},
    {"zlib1.dll","crc32_combine64",3,4,0,3,true}
};
struct Provider {std::string_view name,dll;};
constexpr Provider providers[]{{"zlib","zlib1.dll"},{"v8","x64arm_v8.dll"}};
const Provider* provider(std::string_view dll){
    for(const auto& item:providers)if(item.dll==dll)return &item;
    return nullptr;
}
}
std::span<const WindowsServiceDescriptor> native_library_service_descriptors(){
    static const auto functions=[] {std::vector<WindowsServiceDescriptor> result(std::begin(zlib_functions),std::end(zlib_functions));
        result.push_back({"x64arm_v8.dll","X64ArmV8Dispatch",1,0,0,0,true});return result;}();
    return functions;
}
bool native_library_module(std::string_view dll){return provider(dll)!=nullptr;}
void NativeLibraries::configure(std::string_view names){
    std::set<std::string> selected;
    while(!names.empty()){
        const auto comma=names.find(',');const auto name=names.substr(0,comma);
        if(std::none_of(std::begin(providers),std::end(providers),[&](const auto& p){return p.name==name;}))
            throw std::invalid_argument("不支援的原生函式庫代理："+std::string(name));
        selected.emplace(name);
        if(comma==names.npos)break;
        names.remove_prefix(comma+1);
        if(names.empty())throw std::invalid_argument("原生函式庫清單不可有空白項目");
    }
    enabled_=std::move(selected);
}
bool NativeLibraries::enabled(std::string_view dll)const{
    const auto* p=provider(dll);return p && enabled_.contains(std::string(p->name));
}
std::uint64_t NativeLibraries::invoke(GuestMemory& memory,const WindowsCall& arg,const WindowsServiceDescriptor& service){
    if(!enabled(service.family))throw HostCallFault("原生函式庫代理未啟用");
    const auto name=service.symbol;
    if(std::none_of(std::begin(zlib_functions),std::end(zlib_functions),[&](const auto& f){return f.symbol==name && f.family==service.family;}))
        throw HostCallFault("原生函式庫代理尚未實作此入口");
    const auto u32=[&](unsigned n){return static_cast<std::uint32_t>(arg(n));};
    const auto status=[](int result){return static_cast<std::uint32_t>(result);};
    if(name=="compressBound"){
        ++calls_;return static_cast<std::uint32_t>(::compressBound(u32(0)));
    }
    const bool adler=name.starts_with("adler32"),crc=name.starts_with("crc32");
    if(adler || crc){
        if(name.find("_combine")!=name.npos){
            static_assert(sizeof(z_off_t)>=8);
            const auto length=name.ends_with("64")?std::bit_cast<std::int64_t>(arg(2)):static_cast<std::int64_t>(std::bit_cast<std::int32_t>(u32(2)));
            ++calls_;return static_cast<std::uint32_t>(adler?::adler32_combine(u32(0),u32(1),static_cast<z_off_t>(length)):
                ::crc32_combine(u32(0),u32(1),static_cast<z_off_t>(length)));
        }
        // z_size_t 隨客體指標寬度；一般 uInt 固定 32 位元，NULL buffer 依 zlib 契約回傳初始值。
        const auto length=name.ends_with("_z")?arg(2):u32(2);const auto address=arg(1);
        if(!address){++calls_;return static_cast<std::uint32_t>(adler ? ::adler32(0,nullptr,0) : ::crc32(0,nullptr,0));}
        memory.check(GuestAddress{address},static_cast<std::size_t>(length),Access::Read);
        if(!length){const std::uint8_t empty{};++calls_;return static_cast<std::uint32_t>(adler ? ::adler32(u32(0),&empty,0) : ::crc32(u32(0),&empty,0));}
        auto result=static_cast<uLong>(u32(0));std::uint64_t done=0;
        while(done<length){
            const auto part=memory.readable_prefix(GuestAddress{address+done},static_cast<std::size_t>(std::min(UINT64_C(65536),length-done)));
            result=adler ? ::adler32(result,part.data(),static_cast<uInt>(part.size())) : ::crc32(result,part.data(),static_cast<uInt>(part.size()));
            done+=part.size();
        }
        ++calls_;input_bytes_+=length;return static_cast<std::uint32_t>(result);
    }
    const bool compressing=name=="compress" || name=="compress2",consumed=name=="uncompress2";
    // Windows 的 uLong／uLongf 在兩種客體 ABI 均為 32 位元；絕不將 guest 的 DWORD* 當成 host uLong*。
    const auto length_address=GuestAddress{arg(1)};
    const auto capacity=static_cast<std::uint32_t>(memory.load(length_address,32));
    memory.check(length_address,4,Access::Write);
    const auto source_length=consumed?static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)},32)):u32(3);
    if(consumed)memory.check(GuestAddress{arg(3)},4,Access::Write);
    memory.check(GuestAddress{arg(2)},source_length,Access::Read);
    memory.check(GuestAddress{arg(0)},capacity,Access::Write);
    // 分離連續宿主緩衝區；寫回經 GuestMemory，保留映射檢查、跨區段與自修改碼通知。
    // 此原型先驗證語義；沒有用未追蹤的原生寫入換取零複製。
    try{
        std::vector<std::uint8_t> input(std::max(source_length,1U)),output(std::max(capacity,1U));
        memory.read(GuestAddress{arg(2)},std::span(input).first(source_length));
        auto* source=arg(2)?input.data():nullptr;auto* destination=arg(0)?output.data():nullptr;
        uLong output_size=capacity,input_size=source_length;int result;
        if(compressing)result=::compress2(destination,&output_size,source,source_length,name=="compress2"?std::bit_cast<std::int32_t>(u32(4)):Z_DEFAULT_COMPRESSION);
        else if(consumed)result=::uncompress2(destination,&output_size,source,&input_size);
        else result=::uncompress(destination,&output_size,source,source_length);
        if(output_size>capacity || input_size>source_length)throw HostCallFault("原生 zlib 回傳長度超過客體緩衝區");
        const bool produced=result==Z_OK || result==Z_BUF_ERROR || result==Z_DATA_ERROR || result==Z_NEED_DICT;
        ++calls_;input_bytes_+=compressing?source_length:input_size;if(produced)output_bytes_+=output_size;
        if(produced)memory.write(GuestAddress{arg(0)},std::span(output).first(static_cast<std::size_t>(output_size)));
        memory.store(length_address,32,output_size);
        if(consumed)memory.store(GuestAddress{arg(3)},32,input_size);
        return status(result);
    }catch(const std::bad_alloc&){return status(Z_MEM_ERROR);}
}
std::string NativeLibraries::diagnostics()const{
    if(!enabled_.contains("zlib"))return {};
    std::ostringstream out;out<<"native_library=zlib host_version="<<::zlibVersion()<<" calls="<<calls_
        <<" input_bytes="<<input_bytes_<<" output_bytes="<<output_bytes_<<'\n';return out.str();
}
}

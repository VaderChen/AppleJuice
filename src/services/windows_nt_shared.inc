// 包含於 windows_nt_native.cpp（在各 NT／Rtl 家族 .inc 之前）：NT 物件與 Rtl 轉接共用的參數轉換，不在各家族重複實作。
namespace {
// NT 物件名稱對應 Win32 具名物件：\BaseNamedObjects\X 是全域命名空間，\Sessions\<n>\BaseNamedObjects\X 是 session 本機命名空間。
std::optional<std::u16string> win32_object_name(std::u16string name){
    constexpr std::u16string_view global=u"\\BaseNamedObjects\\",sessions=u"\\Sessions\\";
    const auto fold=[](std::u16string_view text){std::u16string lower(text);for(auto& c:lower)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return lower;};
    const auto lower=fold(name);
    if(lower.starts_with(fold(global))){name.erase(0,global.size());if(fold(name).starts_with(u"global\\") || fold(name).starts_with(u"local\\"))return name;return u"Global\\"+name;}
    if(lower.starts_with(fold(sessions))){
        auto at=sessions.size();while(at<name.size() && name[at]>=u'0' && name[at]<=u'9')++at;
        constexpr std::u16string_view local=u"\\BaseNamedObjects\\";
        if(at==sessions.size() || !fold(std::u16string_view(name).substr(at)).starts_with(fold(local)))return std::nullopt;
        name.erase(0,at+local.size());return name;
    }
    return std::nullopt;
}
// 相對（負值）或絕對（正值，系統時間）的 100 ns 期限轉成 Win32 毫秒；NULL 或最小負值為無限。
std::uint32_t nt_timeout_milliseconds(const GuestMemory& memory,std::uint64_t pointer,std::uint64_t now){
    if(!pointer)return UINT32_MAX;
    const auto value=static_cast<std::int64_t>(memory.load(GuestAddress{pointer},64));
    if(value==INT64_MIN)return UINT32_MAX;
    std::uint64_t ticks=0;
    if(value<0)ticks=static_cast<std::uint64_t>(-value);
    else ticks=static_cast<std::uint64_t>(value)>now?static_cast<std::uint64_t>(value)-now:0;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(0xfffffffeU,ticks/10000+(ticks%10000!=0)));
}
// OBJECT_ATTRIBUTES → Win32 建立／開啟函式的引數：SECURITY_ATTRIBUTES 與 Win32 名稱放在暫存區。
// 失敗時回 NTSTATUS；成功時 security 為 SECURITY_ATTRIBUTES 位址、name 為名稱位址（無名稱時 0）、flags 為 OBJ_* 屬性。
struct Win32ObjectRequest {std::uint64_t security{},name{};std::uint32_t flags{};bool inherit() const {return (flags&2U)!=0;}};
// resolve 把已套用根目錄的 NT 名稱轉成 Win32 名稱（物件命名空間：跟隨符號連結、動態目錄與私有命名空間）。
using NamespaceResolver=std::function<std::uint32_t(std::u16string&,bool)>;
std::uint32_t win32_object_request(GuestMemory& memory,WindowsServices::GuestScratch& scratch,const RuntimeState& runtime,std::uint64_t address,bool x86,bool creating,Win32ObjectRequest& request,const NamespaceResolver& resolve,bool exclusive=false){
    const auto attributes=object_attributes(memory,address,x86,creating,false,exclusive);
    if((attributes.flags&0x22U)==0x22U)return 0xc000000dU;
    const auto rooted=rooted_object_name(runtime,attributes,creating);
    std::u16string object;
    if(!rooted.empty()){
        object=rooted;if(const auto status=resolve(object,creating))return status;
        if(object.empty())return 0xc0000033U;
    }else if(!creating)return 0xc000003bU;
    const unsigned unit=x86?4U:8U;const auto block=scratch.allocate(64+object.size()*2+2);if(!block)return 0xc0000017U;
    memory.store(GuestAddress{block},32,x86?12U:24U);memory.store(GuestAddress{block+unit},x86?32:64,attributes.security);
    memory.store(GuestAddress{block+2*unit},32,(attributes.flags&2U)!=0);
    request.security=block;request.flags=attributes.flags;request.name=object.empty()?0:block+64;
    if(request.name)memory.write(GuestAddress{request.name},utf16_bytes(object));
    return 0;
}
}

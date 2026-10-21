#include "x64arm/windows_services.hpp"
#include <algorithm>
#include <bit>
#include <set>

namespace x64arm {
namespace {
// MSVC x86 使用絕對指標；x64 的 RTTI／throw metadata 使用 32-bit image RVA。
// 結構依 Clang MicrosoftCXXABI 的編譯器輸出，不依物件名稱或呼叫位址判斷。
enum ExceptionKind : unsigned { BadCast,BadTypeid,NonRtti,Exception,BadAlloc,BadFunction,LogicError,LengthError,OutOfRange,RuntimeError,RegexError,InvalidArgument,OverflowError,SystemError,IosFailure,BadException };
struct ExceptionType {std::string_view name,message;unsigned parent;bool error_code;};
constexpr unsigned no_parent=UINT32_MAX;
constexpr std::array<ExceptionType,16> exception_types{{
    {".?AVbad_cast@std@@","Bad dynamic_cast",Exception,false},
    {".?AVbad_typeid@std@@","Attempted typeid of null pointer",Exception,false},
    {".?AV__non_rtti_object@std@@","Object has no readable RTTI",BadTypeid,false},
    {".?AVexception@std@@","Unknown exception",no_parent,false},
    {".?AVbad_alloc@std@@","bad allocation",Exception,false},
    {".?AVbad_function_call@std@@","bad function call",Exception,false},
    {".?AVlogic_error@std@@","logic error",Exception,false},
    {".?AVlength_error@std@@","length error",LogicError,false},
    {".?AVout_of_range@std@@","out of range",LogicError,false},
    {".?AVruntime_error@std@@","runtime error",Exception,false},
    {".?AVregex_error@std@@","regex_error",RuntimeError,true},
    {".?AVinvalid_argument@std@@","invalid argument",LogicError,false},
    {".?AVoverflow_error@std@@","overflow error",RuntimeError,false},
    {".?AVsystem_error@std@@","system error",RuntimeError,true},
    {".?AVfailure@ios_base@std@@","ios_base failure",SystemError,true},
    {".?AVbad_exception@std@@","bad exception",Exception,false},
}};
// 公開 error_type 值與訊息語義；what 文字不依賴宿主 libc++ 的 enum 編碼。
constexpr std::array<std::string_view,15> regex_messages{
    "regex_error(error_collate): invalid collating element", "regex_error(error_ctype): invalid character class",
    "regex_error(error_escape): invalid escape", "regex_error(error_backref): invalid back reference",
    "regex_error(error_brack): mismatched square brackets", "regex_error(error_paren): mismatched parentheses",
    "regex_error(error_brace): mismatched braces", "regex_error(error_badbrace): invalid repeat range",
    "regex_error(error_range): invalid character range", "regex_error(error_space): insufficient compilation memory",
    "regex_error(error_badrepeat): repeat operator has no preceding expression", "regex_error(error_complexity): match complexity limit exceeded",
    "regex_error(error_stack): insufficient matching memory", "regex_error(error_parse)", "regex_error(error_syntax)"
};
struct InvalidRtti {};
std::int32_t signed_word(std::uint64_t value){return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));}
std::uint64_t offset(std::uint64_t base,std::int64_t delta){
    if(delta>=0){if(static_cast<std::uint64_t>(delta)>UINT64_MAX-base)throw InvalidRtti{};return base+static_cast<std::uint64_t>(delta);}
    const auto distance=static_cast<std::uint64_t>(-(delta+1))+1;if(distance>base)throw InvalidRtti{};return base-distance;
}
struct RttiReader {
    const GuestMemory& memory;bool small;std::uint64_t image{};
    unsigned unit()const{return small?4U:8U;}
    std::uint64_t word(std::uint64_t p)const{return memory.load(GuestAddress{p},32);}
    std::uint64_t pointer(std::uint64_t p)const{return memory.load(GuestAddress{p},small?32:64);}
    std::uint64_t reference(std::uint64_t p)const{return offset(small?0:image,static_cast<std::int64_t>(word(p)));}
    std::string name(std::uint64_t type)const{
        if(!type)throw InvalidRtti{};
        std::string value;const auto begin=offset(type,2*unit());
        for(std::uint64_t i=0;i<memory.limit();++i){const auto ch=memory.load(GuestAddress{offset(begin,static_cast<std::int64_t>(i))},8);if(!ch)return value;value.push_back(static_cast<char>(ch));}
        throw InvalidRtti{};
    }
    std::uint64_t adjust(std::uint64_t object,std::uint64_t pmd)const{
        const auto member=signed_word(word(pmd)),vbptr=signed_word(word(pmd+4)),vbase=signed_word(word(pmd+8));
        if(vbptr>=0){const auto cell=offset(object,vbptr);object=offset(cell,signed_word(word(offset(pointer(cell),vbase))));}
        else if(vbptr!=-1)throw InvalidRtti{};
        return offset(object,member);
    }
};
struct RttiNode {
    std::uint64_t descriptor{},type{},object{};std::size_t parent{},end{};std::uint32_t flags{};std::string name;
};
std::vector<RttiNode> hierarchy(const RttiReader& reader,std::uint64_t table,std::uint64_t object){
    reader.memory.check(GuestAddress{table},16,Access::Read);
    const auto count=reader.word(table+8),array=reader.reference(table+12);
    if(!count || count>reader.memory.limit()/sizeof(RttiNode))throw InvalidRtti{};
    reader.memory.check(GuestAddress{array},static_cast<std::size_t>(count)*4,Access::Read);
    std::vector<RttiNode> result;result.reserve(static_cast<std::size_t>(count));std::vector<std::size_t> ancestors;
    for(std::size_t i=0;i<count;++i){
        const auto descriptor=reader.reference(array+i*4);reader.memory.check(GuestAddress{descriptor},24,Access::Read);
        const auto bases=reader.word(descriptor+4);if(bases>=count-i)throw InvalidRtti{};
        while(!ancestors.empty() && result[ancestors.back()].end<=i)ancestors.pop_back();
        const auto end=i+1+static_cast<std::size_t>(bases);
        if((i==0 && end!=count) || (i && (ancestors.empty() || end>result[ancestors.back()].end)))throw InvalidRtti{};
        const auto type=reader.reference(descriptor);
        result.push_back({descriptor,type,reader.adjust(object,descriptor+8),ancestors.empty()?i:ancestors.back(),end,static_cast<std::uint32_t>(reader.word(descriptor+20)),reader.name(type)});
        ancestors.push_back(i);
    }
    return result;
}
bool public_base(const RttiReader& reader,const std::vector<RttiNode>& tree,std::size_t derived,std::size_t base){
    if(base<derived || base>=tree[derived].end)return false;
    if(base==derived)return true;
    if(tree[derived].flags&64U){
        // 新 ABI 附有目標類別自己的 hierarchy，避免完整物件的 private 路徑
        // 錯誤地遮蔽目標類別內部仍屬 public 的 base。
        const auto own=reader.reference(tree[derived].descriptor+24);
        const auto count=reader.word(own+8),array=reader.reference(own+12);const auto index=base-derived;
        if(index>=count)throw InvalidRtti{};
        const auto descriptor=reader.reference(array+index*4);
        return (reader.word(descriptor+20)&(1U|4U|8U))==0;
    }
    // 舊 metadata 仍保留每一條直接繼承邊的 access bit。
    for(auto i=base;i!=derived;i=tree[i].parent)if(tree[i].flags&4U)return false;
    return true;
}
std::uint64_t dynamic_cast_pointer(const RttiReader& reader,std::uint64_t hierarchy_address,std::uint64_t complete,std::uint64_t source,std::uint64_t source_type,std::uint64_t target_type){
    const auto tree=hierarchy(reader,hierarchy_address,complete);
    const auto source_name=reader.name(source_type),target_name=reader.name(target_type);
    std::vector<std::size_t> sources,targets;
    for(std::size_t i=0;i<tree.size();++i){
        if(tree[i].name==source_name && tree[i].object==source)sources.push_back(i);
        if(tree[i].name==target_name)targets.push_back(i);
    }
    std::set<std::uint64_t> down;
    for(const auto target:targets)for(const auto from:sources)if(public_base(reader,tree,target,from))down.insert(tree[target].object);
    if(down.size()==1)return *down.begin();
    if(down.size()>1)return 0;
    const bool visible=std::any_of(sources.begin(),sources.end(),[&](auto from){return (tree[from].flags&(1U|8U))==0;});
    if(!visible)return 0;
    std::set<std::uint64_t> all,accessible;
    for(const auto target:targets){all.insert(tree[target].object);if((tree[target].flags&(1U|8U))==0)accessible.insert(tree[target].object);}
    // 同一個 virtual base 可以出現在多條路徑；依實際 subobject 位址消除重複。
    return all.size()==1 && accessible.size()==1?*accessible.begin():0;
}
}

void WindowsServices::initialize_msvc_exception_types(GuestMemory& memory){
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
        auto& data=runtime_.cxx_exceptions;
        if(!data.image.base){
            // 每個程序共用型別目錄、繼承圖、throw metadata 與唯讀 helper 程式頁。
            // RTTI 與 STL 拋出入口沿用同一個執行緒例外物件及 unwind 生命週期。
            constexpr unsigned code_page=16384,image_size=20480,metadata_start=0x1000,metadata_stride=0x300;
            const auto base=runtime_allocate(memory,{},image_size,4096);
            memory.write(GuestAddress{base},std::array<std::uint8_t,image_size>{});
            const auto put=[&](std::uint64_t at,std::uint64_t value,unsigned width=32){memory.store(GuestAddress{base+at},static_cast<std::uint8_t>(width),value);};
            const auto relative=[&](std::uint64_t at,std::uint64_t target){put(at,x86_?base+target:target);};
            const auto text=[&](std::uint64_t at,std::string_view value){std::vector<std::uint8_t> bytes(value.begin(),value.end());bytes.push_back(0);memory.write(GuestAddress{base+at},bytes);};
            const auto immediate=[&](std::vector<std::uint8_t>& code,std::uint64_t value){for(unsigned b=0;b<unit;++b)code.push_back(static_cast<std::uint8_t>(value>>(8*b)));};
            text(0x40,"invalid random_device value");
            for(unsigned i=0;i<exception_types.size();++i)text(0x100+i*0x80+2*unit,exception_types[i].name);
            constexpr std::array helpers{"ExceptionDestroy","ExceptionDelete","ExceptionWhat"};
            for(unsigned i=0;i<helpers.size();++i){
                std::vector<std::uint8_t> code;if(!x86_)code.push_back(0x48);code.push_back(0xb8);
                immediate(code,resolve("x64arm.msvc",helpers[i]).value);code.insert(code.end(),{0xff,0xe0});memory.write(GuestAddress{base+code_page+i*32},code);
            }
            for(unsigned i=0;i<exception_types.size();++i){
                const auto block=metadata_start+i*metadata_stride,table=block+0x20,locator=block+0xb0,vtable=block+0xd0;
                std::vector<unsigned> chain;for(auto type=i;type!=no_parent;type=exception_types[type].parent)chain.push_back(type);
                const auto count=static_cast<unsigned>(chain.size());relative(block+4,code_page);relative(block+12,table);put(table,count);
                for(unsigned j=0;j<count;++j){
                    const auto type=chain[j],item=block+0x40+j*28;
                    relative(table+4+j*4,item);relative(item+4,0x100+type*0x80);put(item+12,UINT32_MAX);
                    put(item+20,(type==SystemError || type==IosFailure?5U:exception_types[type].error_code?4U:3U)*unit);relative(item+24,code_page+96+type*32);
                }
                const auto hierarchy=block+0x180,base_array=block+0x190;
                put(hierarchy+8,count);relative(hierarchy+12,base_array);
                for(unsigned j=0;j<count;++j){
                    const auto type=chain[j],item=block+0x1a0+j*28;
                    relative(base_array+j*4,item);relative(item,0x100+type*0x80);put(item+4,count-j-1);put(item+12,UINT32_MAX);put(item+20,64);
                    relative(item+24,metadata_start+type*metadata_stride+0x180);
                }
                put(locator,x86_?0:1);relative(locator+12,0x100+i*0x80);relative(locator+16,hierarchy);if(!x86_)relative(locator+20,locator);
                put(vtable,base+locator,bits);put(vtable+unit,base+code_page+32,bits);put(vtable+2*unit,base+code_page+64,bits);
                text(block+0x100,exception_types[i].message);
                data.throw_info[i]=base+block;data.vtable[i]=base+vtable+unit;data.message[i]=base+block+0x100;
                // 每個 copy ctor 只設定目標型別 vptr，再進入共用資料複製。
                // catch(exception value) 必須切片成 exception，不能沿用來源 derived vptr。
                std::vector<std::uint8_t> code;
                if(x86_){code.insert(code.end(),{0xc7,0x01});immediate(code,data.vtable[i]);}
                else {code.insert(code.end(),{0x48,0xb8});immediate(code,data.vtable[i]);code.insert(code.end(),{0x48,0x89,0x01});}
                if(!x86_)code.push_back(0x48);code.push_back(0xb8);immediate(code,resolve("x64arm.msvc","ExceptionCopy").value);code.insert(code.end(),{0xff,0xe0});
                memory.write(GuestAddress{base+code_page+96+i*32},code);
            }
            memory.protect(GuestAddress{base+code_page},4096,read_execute);
            data.image.base=base;data.image.size=image_size;
        }
}
void WindowsServices::throw_msvc_exception(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service,MsvcExceptionKind type,std::uint64_t custom_message,std::uint32_t error_code,std::uint64_t category,bool random_error){
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);const auto kind=static_cast<unsigned>(type);
    if(kind>=exception_types.size())throw HostCallFault("MSVC 例外型別無效");const WindowsCall call(state,memory,service);
        initialize_msvc_exception_types(memory);auto& data=runtime_.cxx_exceptions;
        auto message=random_error?data.image.base+0x40:kind==BadFunction?0:data.message[kind];bool owned=false;
        if(custom_message){message=crt_duplicate_string(memory,custom_message);owned=message!=0;}
        if(kind==RegexError){
            const auto value=error_code<regex_messages.size()?regex_messages[error_code]:std::string_view("regex_error");
            message=allocate_heap(memory,value.size()+1);owned=message!=0;
            if(message){std::vector<std::uint8_t> bytes(value.begin(),value.end());bytes.push_back(0);memory.write(GuestAddress{message},bytes);}
        }
        auto caller=state;call.return_to(caller,0);
        const std::array<std::uint64_t,4> parameters{0x19930520U,0,data.throw_info[kind],data.image.base};
        begin_exception(caller,memory,0xe06d7363U,1,std::span(parameters).first(x86_?3U:4U));
        const auto object=exceptions_.stack.back().record+0x1800;memory.store(GuestAddress{exceptions_.stack.back().record+(x86_?24U:40U)},bits,object);
        memory.store(GuestAddress{object},bits,data.vtable[kind]);memory.store(GuestAddress{object+unit},bits,message);memory.store(GuestAddress{object+2*unit},bits,owned?1:0);
        if(exception_types[kind].error_code)memory.store(GuestAddress{object+3*unit},32,error_code);
        if(kind==SystemError || kind==IosFailure)memory.store(GuestAddress{object+4*unit},bits,category);
        state=caller;
}

std::optional<std::uint32_t> WindowsServices::invoke_rtti(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);const auto symbol=service.symbol;
    if(service.family=="x64arm.msvc" && (symbol=="ExceptionDelete" || symbol=="ExceptionDestroy" || symbol=="ExceptionCopy" || symbol=="ExceptionWhat")){
        const bool deleting=symbol=="ExceptionDelete",what=symbol=="ExceptionWhat",copy=symbol=="ExceptionCopy";
        const WindowsCall method(state,memory,x86_?(deleting || copy?1U:0U):(deleting || copy?2U:1U));
        const auto object=x86_?static_cast<std::uint32_t>(state.reg(Gpr::Rcx)):method(0);
        const auto text=memory.load(GuestAddress{object+unit},bits);
        if(what){const auto& data=runtime_.cxx_exceptions;const auto vtable=memory.load(GuestAddress{object},bits);method.return_to(state,vtable==data.vtable[BadFunction]?data.message[BadFunction]:text?text:data.message[Exception]);return std::nullopt;}
        if(copy){
            const auto source=method(x86_?0U:1U),vtable=memory.load(GuestAddress{object},bits);
            const auto& tables=runtime_.cxx_exceptions.vtable;const auto found=std::find(tables.begin(),tables.end(),vtable);
            if(found==tables.end())throw HostCallFault("內建 C++ 例外複製缺少型別 metadata");
            const auto kind=static_cast<std::size_t>(found-tables.begin());
            crt_exception_data(state,memory,source+unit,object+unit,false);
            if(exception_types[kind].error_code)memory.store(GuestAddress{object+3*unit},32,memory.load(GuestAddress{source+3*unit},32));
            if(kind==SystemError || kind==IosFailure)memory.store(GuestAddress{object+4*unit},bits,memory.load(GuestAddress{source+4*unit},bits));
        }else {
            const auto flags=deleting?method(x86_?0U:1U):0;
            crt_exception_data(state,memory,object+unit,object+unit,true);
            if(flags&1U)(void)free_heap(memory,object,state);
        }
        method.return_to(state,object);return std::nullopt;
    }
    const WindowsCall call(state,memory,service);
    const auto ensure_exception_types=[&]{initialize_msvc_exception_types(memory);};
    const auto throw_exception=[&](unsigned kind,std::uint64_t custom_message=0,std::uint32_t error_code=0,bool random_error=false){throw_msvc_exception(state,memory,service,static_cast<MsvcExceptionKind>(kind),custom_message,error_code,0,random_error);};
    // VC10～VC12 exception 物件採 vptr、訊息指標、擁有權欄位；
    // x86 成員使用 ECX this 與 callee-pop，x64 使用統一暫存器 ABI。
    if(symbol.starts_with("??0exception@") || symbol.starts_with("??1exception@") || symbol.starts_with("??0bad_") || symbol.starts_with("??1bad_") || symbol.starts_with("?what@exception@") || symbol.starts_with("?_type_info_dtor_internal_method@")){
        const bool ctor=symbol.starts_with("??0"),copy=ctor && (symbol.find("AEBV01")!=std::string_view::npos || symbol.find("ABV01")!=std::string_view::npos);
        const bool borrowed=ctor && (symbol.find("AEBQEBDH@Z")!=std::string_view::npos || symbol.find("ABQBDH@Z")!=std::string_view::npos);
        const WindowsCall method(state,memory,(x86_?(ctor?1U:0U):(ctor?2U:1U))+(borrowed?1U:0U));
        const auto object=x86_?static_cast<std::uint32_t>(state.reg(Gpr::Rcx)):method(0);
        const auto argument=ctor?method(x86_?0U:1U):0;
        if(symbol.starts_with("?_type_info_dtor_internal_method@")){
            const auto cached=memory.load(GuestAddress{object+unit},bits);
            if(cached && allocations_.contains(cached))(void)free_heap(memory,cached,state);
            memory.store(GuestAddress{object+unit},bits,0);
        }else{
            ensure_exception_types();const auto& data=runtime_.cxx_exceptions;
            const auto kind=symbol.find("bad_cast@")!=std::string_view::npos?BadCast:symbol.find("bad_typeid@")!=std::string_view::npos?BadTypeid:Exception;
            if(ctor){
                memory.check(GuestAddress{object},3*unit,Access::Write);
                if(copy)crt_exception_data(state,memory,argument+unit,object+unit,false);
                else{
                    const bool indirect=symbol.find("AEBQEBD")!=std::string_view::npos || symbol.find("ABQBD")!=std::string_view::npos;
                    const auto source=indirect?memory.load(GuestAddress{argument},bits):argument;
                    // Microsoft 的 int overload 借用訊息；int 數值不影響行為，析構不釋放借用資料。
                    const auto text=borrowed?source:source?crt_duplicate_string(memory,source):0;
                    memory.store(GuestAddress{object+unit},bits,text);memory.store(GuestAddress{object+2*unit},bits,text && !borrowed?1:0);
                }
                memory.store(GuestAddress{object},bits,data.vtable[kind]);
            }else if(symbol.starts_with("?what@")){
                const auto text=memory.load(GuestAddress{object+unit},bits);method.return_to(state,text?text:data.message[kind]);return std::nullopt;
            }else crt_exception_data(state,memory,object+unit,object+unit,true);
        }
        method.return_to(state,object);return std::nullopt;
    }
    if(symbol=="?_Random_device@std@@YAIXZ"){
        std::array<std::uint8_t,4> bytes{};
        if(!random_bytes(bytes))throw_exception(OutOfRange,0,0,true);
        else {std::uint32_t value=0;for(unsigned i=0;i<bytes.size();++i)value|=static_cast<std::uint32_t>(bytes[i])<<(8*i);call.return_to(state,value);}
        return std::nullopt;
    }
    if(symbol.starts_with("?_X")){
        if(symbol.starts_with("?_Xbad_alloc@"))throw_exception(BadAlloc);
        else if(symbol.starts_with("?_Xbad_function_call@"))throw_exception(BadFunction);
        else if(symbol.starts_with("?_Xregex_error@"))throw_exception(RegexError,0,static_cast<std::uint32_t>(call(0)));
        else {
            unsigned kind=RuntimeError;
            if(symbol.starts_with("?_Xlength_error@"))kind=LengthError;
            else if(symbol.starts_with("?_Xout_of_range@"))kind=OutOfRange;
            else if(symbol.starts_with("?_Xinvalid_argument@"))kind=InvalidArgument;
            else if(symbol.starts_with("?_Xoverflow_error@"))kind=OverflowError;
            else if(!symbol.starts_with("?_Xruntime_error@"))throw HostCallFault("未知 STL 拋出入口");
            throw_exception(kind,call(0));
        }
        return std::nullopt;
    }
    const auto object=call(0);
    if(!object){if(symbol=="__RTtypeid")throw_exception(BadTypeid);else call.return_to(state,0);return std::nullopt;}
    std::uint64_t result=0;
    try {
        RttiReader reader{memory,x86_};const auto vtable=reader.pointer(object),locator=reader.pointer(offset(vtable,-static_cast<std::int64_t>(unit)));
        if(!locator)throw InvalidRtti{};
        memory.check(GuestAddress{locator},x86_?20U:24U,Access::Read);const auto revision=reader.word(locator);
        if(revision>(x86_?0U:1U))throw InvalidRtti{};
        if(!x86_){
            if(revision==1)reader.image=offset(locator,-static_cast<std::int64_t>(reader.word(locator+20)));
            else {const auto* image=module_image(locator,true);if(!image)throw InvalidRtti{};reader.image=image->base;}
        }
        if(symbol=="__RTtypeid"){
            result=reader.reference(locator+12);(void)reader.name(result);
        } else {
            auto complete=offset(object,-static_cast<std::int64_t>(reader.word(locator+4)));
            if(const auto displacement=reader.word(locator+8))complete=offset(complete,-static_cast<std::int64_t>(signed_word(reader.word(offset(object,-static_cast<std::int64_t>(displacement))))));
            if(symbol=="__RTCastToVoid")result=complete;
            else if(symbol=="__RTDynamicCast")result=dynamic_cast_pointer(reader,reader.reference(locator+16),complete,offset(object,-static_cast<std::int64_t>(signed_word(call(1)))),call(2),call(3));
            else throw HostCallFault("未知 MSVC RTTI 入口");
        }
    } catch(const MemoryFault&){throw_exception(NonRtti);return std::nullopt;}
      catch(const InvalidRtti&){throw_exception(NonRtti);return std::nullopt;}
    if(!result && symbol=="__RTDynamicCast" && static_cast<std::uint32_t>(call(4)))throw_exception(BadCast);
    else call.return_to(state,result);
    return std::nullopt;
}
}

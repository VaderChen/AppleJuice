#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <array>

// oleaut32 IDispatch 輔助家族：DispGetParam／DispGetIDsOfNames／DispInvoke 轉到參數 VARIANT 與 ITypeInfo 的既有入口；
// DispCallFunc 依 VARTYPE 在宿主端組出完整呼叫規格（x86 八位元組參數占兩槽、VARIANT／DECIMAL 以值展開，x64 浮點走 XMM），
// CreateStdDispatch 的 IDispatch 物件以宿主服務實作 vtable，Invoke 交給 ITypeInfo::Invoke。
// 另含 OleTranslateColor、ClearCustData、BstrFromVector／VectorFromBstr、VarMonthName／VarWeekdayName、OaBuildVersion、SetOaNoCache。
namespace x64arm {
namespace {
constexpr std::uint32_t invalid=0x80070057U,nomemory=0x8007000eU,nointerface=0x80004002U,pointer_error=0x80004003U,
    badvartype=0x80020008U,paramnotfound=0x80020004U,unknowninterface=0x80020001U,badindex=0x8002000bU;
constexpr std::array<std::uint8_t,16> iid_unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr std::array<std::uint8_t,16> iid_dispatch{0,4,2,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr std::array<std::string_view,3> unknown_methods{"QueryInterface","AddRef","Release"};
constexpr std::array<std::string_view,7> dispatch_methods{"QueryInterface","AddRef","Release","GetTypeInfoCount","GetTypeInfo","GetIDsOfNames","Invoke"};
}
std::span<const WindowsServiceDescriptor> oleaut32_dispatch_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"oleaut32.dll","DispGetParam",5},{"oleaut32.dll","DispGetIDsOfNames",4},{"oleaut32.dll","DispInvoke",8},
        {"oleaut32.dll","DispCallFunc",8},{"oleaut32.dll","CreateStdDispatch",4},
        {"oleaut32.dll","OleTranslateColor",3},{"oleaut32.dll","ClearCustData",1},
        {"oleaut32.dll","BstrFromVector",2},{"oleaut32.dll","VectorFromBstr",2},
        {"oleaut32.dll","VarMonthName",4},{"oleaut32.dll","VarWeekdayName",5},
        {"oleaut32.dll","OaBuildVersion",0},{"oleaut32.dll","SetOaNoCache",0},
        // CreateStdDispatch 物件：內部 IUnknown（聚合時的非委派介面）與 IDispatch。
        {"internal-std-dispatch-unknown","QueryInterface",3},{"internal-std-dispatch-unknown","AddRef",1},{"internal-std-dispatch-unknown","Release",1},
        {"internal-std-dispatch","QueryInterface",3},{"internal-std-dispatch","AddRef",1},{"internal-std-dispatch","Release",1},
        {"internal-std-dispatch","GetTypeInfoCount",2},{"internal-std-dispatch","GetTypeInfo",4},
        {"internal-std-dispatch","GetIDsOfNames",6},{"internal-std-dispatch","Invoke",9},
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_oleaut32_dispatch(GuestState& entry_state,GuestMemory& entry_memory,const WindowsServiceDescriptor& service){
    const unsigned unit=x86_?4U:8U,variant_size=x86_?16U:24U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto name=service.symbol;
    // 純宿主端：不需要客體回呼。
    if(name=="OaBuildVersion" || name=="SetOaNoCache" || name=="OleTranslateColor" || name=="BstrFromVector" || name=="VectorFromBstr" ||
       name=="VarMonthName" || name=="VarWeekdayName" || (service.family=="internal-std-dispatch-unknown" && name=="AddRef")){
        auto& state=entry_state;auto& memory=entry_memory;const WindowsCall arg(state,memory,service);
        const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
        const auto host=[&](std::string_view family,std::string_view symbol,std::initializer_list<std::uint64_t> args){return nested_service(state,memory,family,symbol,args);};
        try{
            if(name=="OaBuildVersion")return done(0x000a0000U); // HIWORD 主版本 10（Windows 10 系列的 OLE Automation），LOWORD 次版本 0。
            if(name=="SetOaNoCache")return done(0); // 引擎的 BSTR 直接由 heap 配置，沒有可停用的快取。
            if(service.family=="internal-std-dispatch-unknown"){
                const auto count=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+2*unit},32))+1;memory.store(GuestAddress{arg(0)+2*unit},32,count);return done(count);
            }
            if(name=="OleTranslateColor"){
                // OLE_COLOR 最高位元組：0x00 RGB、0x01 PALETTEINDEX、0x02 PALETTERGB、0x80 系統色彩索引。
                const auto color=static_cast<std::uint32_t>(arg(0));const auto palette=arg(1),output=arg(2);const auto kind=color>>24;std::uint32_t result=0;
                if(kind==0x80){if((color&0xffffffU)>30)return done(invalid);result=static_cast<std::uint32_t>(host("user32.dll","GetSysColor",{color&0xffU}));}
                else if(kind==0x00)result=palette?(0x02000000U|color):color;
                else if(kind==0x02)result=palette?color:(color&0xffffffU);
                else if(kind==0x01){
                    if(!palette)return done(invalid);
                    const auto entries=static_cast<std::uint32_t>(host("gdi32.dll","GetPaletteEntries",{palette,0,0,0}));
                    if((color&0xffffU)>=entries || (color&0x00ff0000U))return done(invalid);result=color;
                }else return done(invalid);
                if(output)memory.store(GuestAddress{output},32,result);return done(0);
            }
            if(name=="BstrFromVector"){
                // SAFEARRAY {USHORT cDims, fFeatures; ULONG cbElements, cLocks; PVOID pvData; SAFEARRAYBOUND{cElements, lLbound}}：只接受一維位元組向量。
                const auto array=arg(0),output=arg(1);if(!output)return done(invalid);memory.store(GuestAddress{output},bits,0);if(!array)return done(invalid);
                const auto dims=memory.load(GuestAddress{array},16),element=memory.load(GuestAddress{array+4},32);
                if(dims!=1 || element!=1)return done(invalid);
                const auto data=memory.load(GuestAddress{array+(x86_?12U:16U)},bits),count=memory.load(GuestAddress{array+(x86_?16U:24U)},32);
                const auto text=host("oleaut32.dll","SysAllocStringByteLen",{data,count});if(!text)return done(nomemory);
                memory.store(GuestAddress{output},bits,text);return done(0);
            }
            if(name=="VectorFromBstr"){
                const auto text=arg(0),output=arg(1);if(!output)return done(invalid);memory.store(GuestAddress{output},bits,0);
                const auto length=text?memory.load(GuestAddress{text-4},32):0;
                const auto array=host("oleaut32.dll","SafeArrayCreateVector",{17,0,length});if(!array)return done(nomemory);
                if(length){std::vector<std::uint8_t> bytes(length);memory.read(GuestAddress{text},bytes);memory.write(GuestAddress{memory.load(GuestAddress{array+(x86_?12U:16U)},bits)},bytes);}
                memory.store(GuestAddress{output},bits,array);return done(0);
            }
            // VarMonthName(iMonth, fAbbrev, dwFlags, BSTR*)／VarWeekdayName(iWeekday, fAbbrev, iFirstDay, dwFlags, BSTR*)：取目前使用者地區設定的名稱。
            const bool month=name=="VarMonthName";const auto output=arg(month?3:4);
            if(!output)return done(invalid);memory.store(GuestAddress{output},bits,0);
            const auto index=static_cast<std::int32_t>(static_cast<std::uint32_t>(arg(0)));const bool abbreviated=static_cast<std::uint32_t>(arg(1))!=0;
            const auto flags=static_cast<std::uint32_t>(arg(month?2:3));if(flags&~0x80000008U)return done(invalid);
            GuestScratch temporary(*this,memory,state);const auto buffer=temporary.allocate(160);if(!buffer)return done(nomemory);
            std::uint32_t length=0;
            if(month){
                if(index<1 || index>12)return done(invalid);
                if(flags&8U)length=static_cast<std::uint32_t>(host("kernel32.dll","GetCalendarInfoW",{0x400,6,static_cast<std::uint32_t>((abbreviated?0x22:0x15)+index-1)|(flags&0x80000000U),buffer,80,0}));
                else length=static_cast<std::uint32_t>(host("kernel32.dll","GetLocaleInfoW",{0x400,static_cast<std::uint32_t>((abbreviated?0x44:0x38)+index-1)|(flags&0x80000000U),buffer,80}));
            }else{
                // iFirstDay：0＝地區設定的第一天、1＝星期日…7＝星期六；LOCALE_SDAYNAME1 為星期一。
                auto first=static_cast<std::int32_t>(static_cast<std::uint32_t>(arg(2)));
                if(index<1 || index>7 || first<0 || first>7)return done(invalid);
                if(!first){
                    if(!host("kernel32.dll","GetLocaleInfoW",{0x400,0x100cU,buffer,80}))return done(invalid);
                    const auto digit=static_cast<std::int32_t>(memory.load(GuestAddress{buffer},16))-u'0';if(digit<0 || digit>6)return done(invalid);
                    first=(digit+1)%7+1;
                }
                const auto sunday_based=(first-1+index-1)%7;const auto lctype=static_cast<std::uint32_t>((abbreviated?0x31:0x2a)+(sunday_based+6)%7);
                length=static_cast<std::uint32_t>(host("kernel32.dll","GetLocaleInfoW",{0x400,lctype|(flags&0x80000000U),buffer,80}));
            }
            if(!length)return done(invalid);
            std::vector<std::uint8_t> bytes((length-1)*2);if(!bytes.empty())memory.read(GuestAddress{buffer},bytes);
            const auto text=allocate_bstr(memory,bytes.size(),bytes);if(!text)return done(nomemory);
            memory.store(GuestAddress{output},bits,text);return done(0);
        }catch(const MemoryFault&){return done(invalid);}catch(const std::bad_alloc&){return done(nomemory);}
    }
    return nested_continuation(entry_state,entry_memory,"OleAutomationDispatch",[this,service,unit,variant_size,bits](GuestState& state,GuestMemory& memory)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall arg(state,memory,service);const auto name=service.symbol;
        const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
        const auto method=[&](std::uint64_t object,unsigned slot){return memory.load(GuestAddress{memory.load(GuestAddress{object},bits)+slot*unit},bits);};
        const auto guid_at=[&](std::uint64_t address){std::array<std::uint8_t,16> value{};memory.read(GuestAddress{address},value);return value;};
        const auto entry=[this](std::string_view symbol){return resolve("oleaut32.dll",symbol).value;};
        GuestScratch temporary(*this,memory,state);
        if(name=="DispGetParam"){
            // DISPPARAMS {VARIANTARG* rgvarg; DISPID* rgdispidNamedArgs; UINT cArgs, cNamedArgs}：具名參數先依 DISPID 比對，位置參數在 rgvarg 中反序。
            const auto params=arg(0);const auto position=static_cast<std::uint32_t>(arg(1));const auto target=static_cast<std::uint16_t>(arg(2));const auto result=arg(3),error=arg(4);
            if(!params || !result)co_return done(invalid);
            const auto values=memory.load(GuestAddress{params},bits),named=memory.load(GuestAddress{params+unit},bits);
            const auto count=static_cast<std::uint32_t>(memory.load(GuestAddress{params+2*unit},32)),named_count=static_cast<std::uint32_t>(memory.load(GuestAddress{params+2*unit+4},32));
            if(named_count>count)co_return done(invalid);
            std::optional<std::uint32_t> index;
            for(std::uint32_t i=0;i<named_count;++i)if(static_cast<std::uint32_t>(memory.load(GuestAddress{named+i*4},32))==position){index=i;break;}
            if(!index){if(position>=count-named_count)co_return done(paramnotfound);index=count-1-position;}
            memory.write(GuestAddress{result},std::vector<std::uint8_t>(variant_size,0));
            const auto status=co_await service_callback::call(entry("VariantChangeType"),{result,values+*index*variant_size,0,target});
            if(status && error)memory.store(GuestAddress{error},32,*index);
            co_return done(status);
        }
        if(name=="DispGetIDsOfNames"){
            // ITypeInfo::GetIDsOfNames（vtable 第 10 項）。
            if(!arg(0))co_return done(invalid);
            co_return done(co_await service_callback::call(method(arg(0),10),{arg(0),arg(1),arg(2),arg(3)}));
        }
        if(name=="DispInvoke"){
            // ITypeInfo::Invoke(pvInstance, memid, wFlags, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*)（vtable 第 11 項）。
            if(!arg(0) || !arg(1))co_return done(invalid);
            co_return done(co_await service_callback::call(method(arg(1),11),{arg(1),arg(0),arg(2),arg(3)&0xffffU,arg(4),arg(5),arg(6),arg(7)}));
        }
        if(name=="DispCallFunc"){
            // (pvInstance, oVft, CALLCONV, vtReturn, cActuals, VARTYPE*, VARIANTARG**, VARIANT* result)：
            // 有實例時 oVft 是 vtable 位元組位移、實例為第一個參數；沒有實例時 oVft 即函式位址。
            const auto instance=arg(0),offset=arg(1);const auto convention=static_cast<std::uint32_t>(arg(2));const auto returned=static_cast<std::uint16_t>(arg(3));
            const auto count=static_cast<std::uint32_t>(arg(4));const auto types=arg(5),values=arg(6),result=arg(7);
            if(convention>=9 || count>64 || (count && (!types || !values)))co_return done(invalid);
            // Win32 支援 FASTCALL、CDECL、STDCALL、SYSCALL；PASCAL 類慣例參數順序相反，不在 Windows x86 的 COM 範圍。
            if(x86_ && convention!=0 && convention!=1 && convention!=4 && convention!=6)co_return done(invalid);
            GuestCallback call{};call.target=instance?memory.load(GuestAddress{memory.load(GuestAddress{instance},bits)+offset},bits):offset;
            if(!call.target)co_return done(invalid);
            std::uint32_t expanded=0;
            const auto append=[&](std::uint64_t value,bool wide=false,bool floating=false,bool slot=false){
                const auto at=static_cast<unsigned>(call.arguments.size());if(at>=32)throw std::invalid_argument("參數過多");
                call.arguments.push_back(value);if(wide)call.wide_arguments|=1U<<at;if(floating)call.floating_arguments|=1U<<at;if(slot)expanded|=1U<<at;};
            if(instance)append(instance);
            const bool aggregate_result=returned==12 || returned==14;std::uint64_t returned_value=0;
            if(aggregate_result){returned_value=temporary.allocate(variant_size);if(!returned_value)co_return done(nomemory);append(returned_value);}
            const auto size_of=[&](std::uint16_t vt)->unsigned{
                if(vt&0x4000U)return 0;if(vt&0x2000U)return unit;
                switch(vt){case 16:case 17:return 1;case 2:case 11:case 18:return 2;case 3:case 4:case 10:case 19:case 22:case 23:case 25:return 4;
                    case 5:case 6:case 7:case 20:case 21:return 8;case 8:case 9:case 13:case 26:case 30:case 31:case 37:case 38:return unit;case 12:case 14:return 16;default:return UINT32_MAX;}};
            try{
                for(std::uint32_t i=0;i<count;++i){
                    const auto vt=static_cast<std::uint16_t>(memory.load(GuestAddress{types+i*2},16));const auto source=memory.load(GuestAddress{values+i*unit},bits);
                    const auto size=size_of(vt);if(size==UINT32_MAX || !source)co_return done(badvartype);
                    if(!size){append(memory.load(GuestAddress{source+8},bits));continue;}
                    if(vt==12 || vt==14){
                        // VARIANT／DECIMAL 以值傳遞：Win32 展開成四個堆疊槽，Win64 傳呼叫者配置的副本指標。
                        std::array<std::uint8_t,24> copy{};memory.read(GuestAddress{source},std::span{copy}.first(variant_size));
                        if(vt==14){copy[0]=copy[1]=0;}
                        if(x86_){for(unsigned word=0;word<4;++word){std::uint32_t value=0;for(unsigned b=0;b<4;++b)value|=std::uint32_t(copy[word*4+b])<<(b*8);append(value,false,false,true);}}
                        else {const auto block=temporary.allocate(variant_size);if(!block)co_return done(nomemory);memory.write(GuestAddress{block},std::span{copy}.first(variant_size));append(block);}
                        continue;
                    }
                    const auto value=memory.load(GuestAddress{source+8},static_cast<std::uint8_t>(size*8));
                    append(value,size==8,vt==4 || vt==5 || vt==7);
                }
            }catch(const std::invalid_argument&){co_return done(invalid);}
            if(x86_ && convention==0){
                // FASTCALL：最左邊兩個非 64 位元、非浮點、非展開的 DWORD 參數改由 ECX／EDX 傳遞。
                std::vector<unsigned> order;for(unsigned i=0;i<call.arguments.size() && order.size()<2;++i)if(!((call.wide_arguments|call.floating_arguments|expanded)&(1U<<i)))order.push_back(i);
                call.x86_register_arguments=static_cast<unsigned>(order.size());
                for(unsigned i=0;i<call.arguments.size();++i)if(std::find(order.begin(),order.end(),i)==order.end())order.push_back(i);
                auto args=call.arguments;const auto wide=call.wide_arguments,floating=call.floating_arguments;call.wide_arguments=call.floating_arguments=0;
                for(unsigned i=0;i<order.size();++i){call.arguments[i]=args[order[i]];if(wide&(1U<<order[i]))call.wide_arguments|=1U<<i;if(floating&(1U<<order[i]))call.floating_arguments|=1U<<i;}
            }
            if(returned==4)call.floating_result=32;else if(returned==5 || returned==7)call.floating_result=64;
            else if(returned==6 || returned==20 || returned==21)call.wide_result=true;
            const auto value=co_await service_callback::call_native(std::move(call));
            if(result){
                std::vector<std::uint8_t> out(variant_size,0);
                if(aggregate_result){memory.read(GuestAddress{returned_value},std::span{out});if(returned==14){out[0]=14;out[1]=0;}}
                else if(returned!=0 && returned!=24){
                    out[0]=static_cast<std::uint8_t>(returned);out[1]=static_cast<std::uint8_t>(returned>>8);
                    const auto size=returned==25?4U:size_of(returned);const auto bytes=size==0 || size==UINT32_MAX?unit:size;
                    for(unsigned b=0;b<bytes && b<8;++b)out[8+b]=static_cast<std::uint8_t>(value>>(b*8));
                }
                memory.write(GuestAddress{result},out);
            }
            co_return done(0);
        }
        if(name=="ClearCustData"){
            // CUSTDATA {DWORD cCustData; CUSTDATAITEM* prgCustData}，項目為 {GUID; VARIANTARG}；逐項 VariantClear 後以 CoTaskMemFree 釋放陣列。
            const auto data=arg(0);
            if(data){
                const auto count=static_cast<std::uint32_t>(memory.load(GuestAddress{data},32));const auto items=memory.load(GuestAddress{data+unit},bits);
                for(std::uint32_t i=0;items && i<count;++i)(void)co_await service_callback::call(entry("VariantClear"),{items+i*(16+variant_size)+16});
                if(items)(void)nested_service(state,memory,"ole32.dll","CoTaskMemFree",{items});
                memory.store(GuestAddress{data},32,0);memory.store(GuestAddress{data+unit},bits,0);
            }
            co_return done(0);
        }
        if(name=="CreateStdDispatch"){
            // 物件：{內部 IUnknown vtable, IDispatch vtable, 參考計數, 控制 IUnknown, pvThis, ITypeInfo*}；回傳內部 IUnknown。
            const auto outer=arg(0),instance=arg(1),info=arg(2),output=arg(3);
            if(!output)co_return done(invalid);memory.store(GuestAddress{output},bits,0);if(!instance || !info)co_return done(invalid);
            if(!std_dispatch_vtables_[0]){
                for(unsigned table=0;table<2;++table){
                    const auto family=table?"internal-std-dispatch":"internal-std-dispatch-unknown";const auto count=table?dispatch_methods.size():unknown_methods.size();
                    const auto address=runtime_allocate(memory,{},count*unit);
                    for(std::size_t i=0;i<count;++i)memory.store(GuestAddress{address+i*unit},bits,resolve(family,table?dispatch_methods[i]:unknown_methods[i]).value);
                    std_dispatch_vtables_[table]=address;
                }
            }
            const auto object=allocate_heap(memory,6*unit);if(!object)co_return done(nomemory);
            memory.store(GuestAddress{object},bits,std_dispatch_vtables_[0]);memory.store(GuestAddress{object+unit},bits,std_dispatch_vtables_[1]);
            memory.store(GuestAddress{object+2*unit},bits,1);memory.store(GuestAddress{object+3*unit},bits,outer?outer:object);
            memory.store(GuestAddress{object+4*unit},bits,instance);memory.store(GuestAddress{object+5*unit},bits,info);
            (void)co_await service_callback::call(method(info,1),{info});
            memory.store(GuestAddress{output},bits,object);co_return done(0);
        }
        // CreateStdDispatch 物件的方法。
        const bool inner=service.family=="internal-std-dispatch-unknown";const auto object=inner?arg(0):arg(0)-unit;
        const auto controlling=memory.load(GuestAddress{object+3*unit},bits);const bool aggregated=controlling!=object;
        const auto count_at=object+2*unit;
        if(inner && name=="QueryInterface"){
            if(!arg(2))co_return done(pointer_error);memory.store(GuestAddress{arg(2)},bits,0);
            const auto iid=guid_at(arg(1));std::uint64_t pointer=0;
            if(iid==iid_unknown)pointer=object;else if(iid==iid_dispatch)pointer=object+unit;else co_return done(nointerface);
            // 傳回的介面以該介面的 AddRef 計數：聚合時 IDispatch 的 AddRef 委派給外部物件。
            if(pointer==object+unit && aggregated)(void)co_await service_callback::call(method(controlling,1),{controlling});
            else memory.store(GuestAddress{count_at},32,memory.load(GuestAddress{count_at},32)+1);
            memory.store(GuestAddress{arg(2)},bits,pointer);co_return done(0);
        }
        if(inner && name=="Release"){
            const auto count=static_cast<std::uint32_t>(memory.load(GuestAddress{count_at},32));if(!count)co_return done(0);
            memory.store(GuestAddress{count_at},32,count-1);
            if(count==1){const auto info=memory.load(GuestAddress{object+5*unit},bits);(void)co_await service_callback::call(method(info,2),{info});(void)free_heap(memory,object,state);}
            co_return done(count-1);
        }
        if(name=="QueryInterface" || name=="AddRef" || name=="Release"){
            // IDispatch 的 IUnknown 方法一律委派給控制 IUnknown（未聚合時就是內部 IUnknown）。
            const unsigned slot=name=="QueryInterface"?0:name=="AddRef"?1:2;
            std::vector<std::uint64_t> args{controlling};if(slot==0){args.push_back(arg(1));args.push_back(arg(2));}
            co_return done(co_await service_callback::call(method(controlling,slot),std::move(args)));
        }
        const auto info=memory.load(GuestAddress{object+5*unit},bits),instance=memory.load(GuestAddress{object+4*unit},bits);
        if(name=="GetTypeInfoCount"){if(!arg(1))co_return done(invalid);memory.store(GuestAddress{arg(1)},32,1);co_return done(0);}
        if(name=="GetTypeInfo"){
            if(!arg(3))co_return done(invalid);memory.store(GuestAddress{arg(3)},bits,0);if(static_cast<std::uint32_t>(arg(1)))co_return done(badindex);
            (void)co_await service_callback::call(method(info,1),{info});memory.store(GuestAddress{arg(3)},bits,info);co_return done(0);
        }
        constexpr std::array<std::uint8_t,16> iid_null{};
        if(name=="GetIDsOfNames"){
            if(!arg(1) || guid_at(arg(1))!=iid_null)co_return done(unknowninterface);
            co_return done(co_await service_callback::call(method(info,10),{info,arg(2),arg(3),arg(5)}));
        }
        if(name=="Invoke"){
            if(!arg(2) || guid_at(arg(2))!=iid_null)co_return done(unknowninterface);
            co_return done(co_await service_callback::call(method(info,11),{info,instance,arg(1),arg(4)&0xffffU,arg(5),arg(6),arg(7),arg(8)}));
        }
        throw HostCallFault("未知的 oleaut32 IDispatch 服務");
    });
}
}

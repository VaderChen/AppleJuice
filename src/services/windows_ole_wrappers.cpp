// ole32 包裝函式（以既有 COM 啟動、持續化與資料物件服務轉接；combase 同名入口在 builtin_export_aliases.hpp 轉到這裡）：
// - CoCreateInstanceFromApp：與 CoCreateInstanceEx 相同（保留參數取代 COSERVERINFO，只做本機啟動）。
// - CoGetInstanceFromFile／CoGetInstanceFromIStorage：CLSID 取自參數，否則由 GetClassFile／ReadClassStg 判定；以 IPersistFile::Load（grfMode）或
//   IPersistStorage::Load 載入後依 MULTI_QI 逐一 QueryInterface；全部成功 S_OK、部分 CO_S_NOTALLINTERFACES、全部失敗 E_NOINTERFACE。
// - FreePropVariantArray：逐一 PropVariantClear，回傳第一個失敗。
// - BindMoniker：CreateBindCtx 後 IMoniker::BindToObject（grfOpt 必須為 0）。
// - OleNoteObjectVisible：CoLockObjectExternal(pUnknown, fVisible, FALSE)。
// - OleQueryCreateFromData：資料物件列出 Embedded Object／Embed Source 為 S_OK；只有 CF_METAFILEPICT／CF_BITMAP／CF_DIB／CF_ENHMETAFILE 為 OLE_S_STATIC；否則 S_FALSE。
//   OleQueryLinkFromData：列出 Link Source 為 S_OK，否則 S_FALSE。格式以 IDataObject::EnumFormatEtc(DATADIR_GET) 列舉。
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <array>

namespace x64arm {
namespace {
constexpr std::uint32_t e_invalidarg=0x80070057U,e_pointer=0x80004003U,e_outofmemory=0x8007000eU,e_nointerface=0x80004002U,co_s_notallinterfaces=0x00080012U;
constexpr std::array<std::uint8_t,16> iid_persist_file{0x0b,0x01,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr std::array<std::uint8_t,16> iid_persist_storage{0x0a,0x01,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
}
std::span<const WindowsServiceDescriptor> ole_wrapper_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"ole32.dll","WdtpInterfacePointer_UserSize",5},
        {"ole32.dll","WdtpInterfacePointer_UserMarshal",5},
        {"ole32.dll","WdtpInterfacePointer_UserUnmarshal",4},
        {"ole32.dll","WdtpInterfacePointer_UserFree",1},
        {"combase.dll","WdtpInterfacePointer_UserSize",5},
        {"combase.dll","WdtpInterfacePointer_UserMarshal",5},
        {"combase.dll","WdtpInterfacePointer_UserUnmarshal",4},
        {"combase.dll","WdtpInterfacePointer_UserFree",1},
        {"combase.dll","WdtpInterfacePointer_UserSize64",5},
        {"combase.dll","WdtpInterfacePointer_UserMarshal64",5},
        {"combase.dll","WdtpInterfacePointer_UserUnmarshal64",4},
        {"combase.dll","WdtpInterfacePointer_UserFree64",1},
        {"ole32.dll","CoCreateInstanceFromApp",6,0,0,0x14},{"ole32.dll","CoGetInstanceFromFile",8,0,0,0x58},{"ole32.dll","CoGetInstanceFromIStorage",7,0,0,0x28},
        {"ole32.dll","ReadOleStg",6},{"ole32.dll","WriteOleStg",4,0,0,4},
        {"ole32.dll","OpenOrCreateStream",3},{"ole32.dll","ReadStringStream",2},{"ole32.dll","WriteStringStream",2},
        {"ole32.dll","FreePropVariantArray",2,0,0,0x1},{"ole32.dll","BindMoniker",4,0,0,0x2},{"ole32.dll","OleNoteObjectVisible",2,0,0,0x2},
        {"ole32.dll","OleQueryCreateFromData",1},{"ole32.dll","OleQueryLinkFromData",1}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_ole_wrappers(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    return nested_continuation(state,memory,"ole32 包裝函式",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
        const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);const auto name=service.symbol;
        std::vector<std::uint64_t> blocks;
        struct Release {WindowsServices& self;GuestMemory& m;GuestState& s;std::vector<std::uint64_t>& blocks;~Release(){for(const auto block:blocks)(void)self.free_heap(m,block,s);}} release{*this,m,s,blocks};
        const auto scratch=[&](std::size_t size)->std::uint64_t{const auto block=allocate_heap(m,std::max<std::size_t>(size,1));if(!block)throw std::bad_alloc{};m.fill(GuestAddress{block},0,std::max<std::size_t>(size,1));blocks.push_back(block);return block;};
        const auto bytes=[&](std::span<const std::uint8_t> value){const auto block=scratch(value.size());m.write(GuestAddress{block},value);return block;};
        const auto api=[&](std::string_view dll,std::string_view symbol,std::vector<std::uint64_t> arguments)->service_callback::Task<std::uint64_t>{
            co_return co_await service_callback::call_value(resolve(dll,symbol).value,std::move(arguments));
        };
        const auto method=[&](std::uint64_t object,unsigned index){return m.load(GuestAddress{m.load(GuestAddress{object},bits)+std::uint64_t{index}*unit},bits);};
        const auto com=[&](std::uint64_t object,unsigned index,std::vector<std::uint64_t> arguments)->service_callback::Task<std::uint64_t>{
            arguments.insert(arguments.begin(),object);co_return co_await service_callback::call_value(method(object,index),std::move(arguments));
        };
        try{
            if(name=="CoCreateInstanceFromApp")
                co_return finish(static_cast<std::uint32_t>(co_await api("ole32.dll","CoCreateInstanceEx",{call(0),call(1),call(2),0,call(4),call(5)})));
#include "windows_propvariant_aggregate.inc"
#include "windows_ole_record.inc"
#include "windows_ole_interface_marshal.inc"
            if(name=="OpenOrCreateStream"){
                const auto storage=call(0),stream_name=call(1),out=call(2);
                if(!out)co_return finish(e_pointer);
                m.store(GuestAddress{out},bits,0);
                if(!storage || !stream_name)co_return finish(e_invalidarg);
                // STGM_READWRITE | STGM_SHARE_EXCLUSIVE；建立時保留已存在的內容。
                constexpr unsigned mode=0x12;
                auto hr=static_cast<std::uint32_t>(co_await com(storage,3,{stream_name,mode,0,0,out}));
                if(hr==0x80030050U)
                    hr=static_cast<std::uint32_t>(co_await com(storage,4,{stream_name,0,mode,0,out}));
                co_return finish(hr);
            }
            if(name=="WriteStringStream" || name=="ReadStringStream"){
                const bool writing=name=="WriteStringStream";
                const auto stream=call(0),text=call(1);
                if(!writing){
                    if(!text)co_return finish(e_pointer);
                    m.store(GuestAddress{text},bits,0);
                }
                if(!stream)co_return finish(e_invalidarg);
                const auto header=scratch(8);
                if(writing){
                    std::uint32_t size=0;
                    if(text){
                        do{
                            if(size>UINT32_MAX-2 || text>UINT64_MAX-size)co_return finish(e_invalidarg);
                            size+=2;
                        }while(m.load(GuestAddress{text+(size-2)},16));
                    }
                    m.store(GuestAddress{header},32,size);
                    auto hr=static_cast<std::uint32_t>(co_await com(stream,4,{header,4,0}));
                    if(!hr && size)hr=static_cast<std::uint32_t>(co_await com(stream,4,{text,size,0}));
                    co_return finish(hr);
                }
                const auto read=[&](std::uint64_t dest,std::uint32_t count)->service_callback::Task<std::uint32_t>{
                    m.store(GuestAddress{header+4},32,0);
                    const auto hr=static_cast<std::uint32_t>(co_await com(stream,3,{dest,count,header+4}));
                    co_return hr?hr:static_cast<std::uint32_t>(m.load(GuestAddress{header+4},32)!=count);
                };
                if(const auto hr=co_await read(header,4))co_return finish(hr);
                const auto size=static_cast<std::uint32_t>(m.load(GuestAddress{header},32));
                if(!size)co_return finish(0);
                if(size>INT32_MAX)co_return finish(0x80004005U);
                const auto data=co_await api("ole32.dll","CoTaskMemAlloc",{size});
                if(!data)co_return finish(e_outofmemory);
                std::uint32_t hr=0;
                std::exception_ptr failure;
                try{
                    hr=co_await read(data,size);
                    if(!hr)m.store(GuestAddress{text},bits,data);
                }catch(...){failure=std::current_exception();}
                if(hr || failure)(void)co_await api("ole32.dll","CoTaskMemFree",{data});
                if(failure)std::rethrow_exception(failure);
                co_return finish(hr);
            }
            if(name=="FreePropVariantArray"){
                const auto count=static_cast<std::uint32_t>(call(0));const auto items=call(1);if(count && !items)co_return finish(e_invalidarg);
                const unsigned stride=x86_?16U:24U;std::uint32_t first=0;
                for(std::uint32_t i=0;i<count;++i){const auto result=static_cast<std::uint32_t>(co_await api("ole32.dll","PropVariantClear",{items+std::uint64_t{i}*stride}));if(result && !first)first=result;}
                co_return finish(first);
            }
            if(name=="BindMoniker"){
                // (pmk, grfOpt, riidResult, ppvResult)
                if(!call(3))co_return finish(e_pointer);m.store(GuestAddress{call(3)},bits,0);
                if(!call(0) || static_cast<std::uint32_t>(call(1)) || !call(2))co_return finish(e_invalidarg);
                const auto context_at=scratch(unit);
                if(const auto status=static_cast<std::uint32_t>(co_await api("ole32.dll","CreateBindCtx",{0,context_at})))co_return finish(status);
                const auto context=m.load(GuestAddress{context_at},bits);
                const auto result=static_cast<std::uint32_t>(co_await com(call(0),8,{context,0,call(2),call(3)})); // IMoniker::BindToObject
                (void)co_await com(context,2,{});
                co_return finish(result);
            }
            if(name=="OleNoteObjectVisible"){
                if(!call(0))co_return finish(e_invalidarg);
                co_return finish(static_cast<std::uint32_t>(co_await api("ole32.dll","CoLockObjectExternal",{call(0),static_cast<std::uint32_t>(call(1))?1U:0U,0})));
            }
            if(name=="OleQueryCreateFromData" || name=="OleQueryLinkFromData"){
                if(!call(0))co_return finish(e_invalidarg);
                const auto enum_at=scratch(unit);
                if(static_cast<std::uint32_t>(co_await com(call(0),8,{1,enum_at})))co_return finish(1); // IDataObject::EnumFormatEtc(DATADIR_GET)；無法列舉時視為沒有可用格式
                const auto enumerator=m.load(GuestAddress{enum_at},bits);if(!enumerator)co_return finish(1);
                const auto registered=[&](std::u16string_view format)->service_callback::Task<std::uint32_t>{
                    co_return static_cast<std::uint32_t>(co_await api("user32.dll","RegisterClipboardFormatW",{bytes(utf16_bytes(format))}))&0xffffU;};
                const auto embedded=co_await registered(u"Embedded Object"),source=co_await registered(u"Embed Source"),link=co_await registered(u"Link Source");
                const unsigned stride=x86_?20U:32U;const auto items=scratch(16*stride),fetched=scratch(4);
                bool embed=false,linkable=false,picture=false;
                for(unsigned round=0;round<4096;++round){
                    m.store(GuestAddress{fetched},32,0);const auto status=static_cast<std::uint32_t>(co_await com(enumerator,3,{16,items,fetched})); // IEnumFORMATETC::Next
                    const auto count=std::min<std::uint32_t>(static_cast<std::uint32_t>(m.load(GuestAddress{fetched},32)),16);
                    for(std::uint32_t i=0;i<count;++i){
                        const auto at=items+std::uint64_t{i}*stride;const auto format=static_cast<std::uint32_t>(m.load(GuestAddress{at},16));
                        if(format==embedded || format==source)embed=true;else if(format==link)linkable=true;else if(format==3 || format==2 || format==8 || format==14)picture=true;
                        if(const auto device=m.load(GuestAddress{at+unit},bits))(void)co_await api("ole32.dll","CoTaskMemFree",{device});
                    }
                    if(status || count<16)break;
                }
                (void)co_await com(enumerator,2,{});
                if(name=="OleQueryLinkFromData")co_return finish(linkable?0U:1U);
                co_return finish(embed?0U:picture?0x00040001U:1U); // OLE_S_STATIC
            }
            if(name=="CoGetInstanceFromFile" || name=="CoGetInstanceFromIStorage"){
                // 檔案：(pServerInfo, pClsid, punkOuter, dwClsCtx, grfMode, pwszName, dwCount, pResults)；儲存體：(pServerInfo, pClsid, punkOuter, dwClsCtx, pstg, dwCount, pResults)
                const bool file=name=="CoGetInstanceFromFile";
                const auto source=call(file?5:4);const auto count=static_cast<std::uint32_t>(call(file?6:5));const auto results=call(file?7:6);
                if(!source || !count || !results || count>4096)co_return finish(e_invalidarg);
                const unsigned stride=x86_?12U:24U;
                for(std::uint32_t i=0;i<count;++i){const auto at=results+std::uint64_t{i}*stride;m.store(GuestAddress{at+unit},bits,0);m.store(GuestAddress{at+2*unit},32,e_nointerface);}
                auto clsid=call(1);
                if(!clsid){clsid=scratch(16);
                    const auto status=static_cast<std::uint32_t>(co_await api("ole32.dll",file?"GetClassFile":"ReadClassStg",{source,clsid}));if(status)co_return finish(status);}
                const auto object_at=scratch(unit);
                const auto interface_id=bytes(file?iid_persist_file:iid_persist_storage);
                if(const auto status=static_cast<std::uint32_t>(co_await api("ole32.dll","CoCreateInstance",{clsid,call(2),call(3),interface_id,object_at})))co_return finish(status);
                const auto object=m.load(GuestAddress{object_at},bits);
                // IPersistFile::Load 為第 5 個方法（IUnknown 3、GetClassID、IsDirty 之後）；IPersistStorage::Load 為第 7 個（InitNew 之後）。
                const auto loaded=static_cast<std::uint32_t>(file?co_await com(object,5,{source,static_cast<std::uint32_t>(call(4))}):co_await com(object,6,{source}));
                if(loaded&0x80000000U){(void)co_await com(object,2,{});co_return finish(loaded);}
                std::uint32_t succeeded=0;
                for(std::uint32_t i=0;i<count;++i){
                    const auto at=results+std::uint64_t{i}*stride;const auto iid=m.load(GuestAddress{at},bits);
                    const auto status=iid?static_cast<std::uint32_t>(co_await com(object,0,{iid,at+unit})):e_pointer;m.store(GuestAddress{at+2*unit},32,status);if(!status)++succeeded;
                }
                (void)co_await com(object,2,{});
                co_return finish(succeeded==count?0U:succeeded?co_s_notallinterfaces:e_nointerface);
            }
        }catch(const MemoryFault&){co_return finish(e_invalidarg);}catch(const std::bad_alloc&){co_return finish(e_outofmemory);}
        throw HostCallFault("未知的 ole32 包裝函式");
    });
}
}

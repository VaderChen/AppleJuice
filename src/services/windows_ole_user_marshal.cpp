// OLE 線路型別的 user marshal 常式（*_UserSize／_UserMarshal／_UserUnmarshal／_UserFree），依 Windows SDK wtypes.idl／objidl.idl
// 的線路型別以 NDR 表示（小端、依絕對位置對齊；unique 指標寫非零 referent ID，pointee 延後寫在結構之後；conformant 結構
// 先寫 MaxCount）。pFlags 低 16 位元為 MSHCTX：INPROC（3）與 CROSSCTX（4）在同一程序內只傳 handle，其餘依型別傳內容。
// - 一般 handle（HWND、HMENU、HACCEL、HBRUSH、HDC、HICON）：RemotableHandle {fContext, long}；DIFFERENTMACHINE 無法傳送。
// - CLIPFORMAT：userCLIPFORMAT；跨機器的已登錄格式（≥0xC000）改傳格式名稱，接收端重新 RegisterClipboardFormat。
// - HGLOBAL、HBITMAP、HPALETTE、HENHMETAFILE、HMETAFILE、HMETAFILEPICT：同程序傳 handle（x64 為 WDT_INPROC64_CALL＋__int64），
//   跨程序傳 FLAGGED_BYTE_BLOB／userBITMAP／LOGPALETTE／BYTE_BLOB／remoteMETAFILEPICT 內容並在接收端重建；UserFree 只釋放跨程序重建的物件。
// - SNB：RemSNB {ulCntStr, ulCntChar, OLECHAR[]}；接收端以 CoTaskMemAlloc 配置單一區塊（指標表＋字串），UserFree 以 CoTaskMemFree 釋放。
// - BSTR：FLAGGED_WORD_BLOB，fFlags 為位元組長度（NULL BSTR 為 0xFFFFFFFF）。
// - HSTRING：8 位元組對齊；同程序封送持有一次引用，其餘內容封送保留完整 UTF-16。
// 錯誤依 RPC 契約以不可繼續例外丟出（RPC_X_BAD_STUB_DATA、RPC_S_INVALID_TAG、E_OUTOFMEMORY）。
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>

namespace x64arm {
namespace {
constexpr std::uint32_t inproc_call=0x48746457,remote_call=0x52746457,inproc64_call=0x50746457;
constexpr std::uint32_t bad_stub_data=1783,invalid_tag=1733,out_of_memory=0x8007000eU;
struct MarshalError {std::uint32_t code;};
// Size 從 StartingSize、Marshal 從 pBuffer 起算絕對位置，兩者共用同一份編碼。
struct NdrWriter {
    std::uint64_t base{};std::vector<std::uint8_t> bytes;
    void align(unsigned n){while((base+bytes.size())%n)bytes.push_back(0);}
    void put(std::uint64_t value,unsigned size){align(size);for(unsigned i=0;i<size;++i)bytes.push_back(static_cast<std::uint8_t>(value>>(i*8)));}
    void raw(std::span<const std::uint8_t> data){bytes.insert(bytes.end(),data.begin(),data.end());}
};
struct NdrReader {
    GuestMemory& memory;std::uint64_t at;
    std::uint64_t get(unsigned size){at=(at+size-1)/size*size;const auto value=memory.load(GuestAddress{at},static_cast<std::uint8_t>(size*8));at+=size;return value;}
    std::vector<std::uint8_t> raw(std::uint64_t size){
        if(size>256U*1024U*1024U)throw MarshalError{bad_stub_data};
        std::vector<std::uint8_t> data(static_cast<std::size_t>(size));if(!data.empty())memory.read(GuestAddress{at},data);at+=size;return data;
    }
};
enum class Wire {Handle,ClipFormat,Global,Bitmap,Palette,EnhMetafile,Metafile,MetafilePict,Snb,Bstr};
Wire wire_type(std::string_view type){
    if(type=="CLIPFORMAT")return Wire::ClipFormat;if(type=="HGLOBAL")return Wire::Global;if(type=="HBITMAP")return Wire::Bitmap;
    if(type=="HPALETTE")return Wire::Palette;if(type=="HENHMETAFILE")return Wire::EnhMetafile;if(type=="HMETAFILE")return Wire::Metafile;
    if(type=="HMETAFILEPICT")return Wire::MetafilePict;if(type=="SNB")return Wire::Snb;if(type=="BSTR")return Wire::Bstr;
    return Wire::Handle;
}
}
#define X64ARM_WIRE(dll,type) {dll,#type "_UserSize",3,0,0,0x2},{dll,#type "_UserMarshal",3},{dll,#type "_UserUnmarshal",3},{dll,#type "_UserFree",2}
std::span<const WindowsServiceDescriptor> ole_user_marshal_service_descriptors(){
    // combase 為實作所在；ole32 的同名匯出以別名指向 combase。ole32 另有 metafile、HMETAFILEPICT 與 SNB，oleaut32 有 BSTR。
    static constexpr WindowsServiceDescriptor table[]{
        X64ARM_WIRE("combase.dll",CLIPFORMAT),X64ARM_WIRE("combase.dll",HACCEL),X64ARM_WIRE("combase.dll",HBITMAP),X64ARM_WIRE("combase.dll",HBRUSH),
        X64ARM_WIRE("combase.dll",HDC),X64ARM_WIRE("combase.dll",HGLOBAL),X64ARM_WIRE("combase.dll",HICON),X64ARM_WIRE("combase.dll",HMENU),
        X64ARM_WIRE("combase.dll",HPALETTE),X64ARM_WIRE("combase.dll",HWND),X64ARM_WIRE("combase.dll",HSTRING),
        {"combase.dll","HSTRING_UserSize64",3,0,0,2},{"combase.dll","HSTRING_UserMarshal64",3},
        {"combase.dll","HSTRING_UserUnmarshal64",3},{"combase.dll","HSTRING_UserFree64",2},
        X64ARM_WIRE("ole32.dll",HENHMETAFILE),X64ARM_WIRE("ole32.dll",HMETAFILE),X64ARM_WIRE("ole32.dll",HMETAFILEPICT),X64ARM_WIRE("ole32.dll",SNB),
        X64ARM_WIRE("oleaut32.dll",BSTR)};
    return table;
}
#undef X64ARM_WIRE
std::optional<std::uint32_t> WindowsServices::invoke_ole_user_marshal(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const auto split=name.find("_User");
    const auto type=name.substr(0,split);auto operation=name.substr(split+1);
    if(operation.ends_with("64"))operation.remove_suffix(2);
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    std::vector<std::uint64_t> blocks;
    struct Release {WindowsServices& self;GuestMemory& memory;GuestState& state;std::vector<std::uint64_t>& blocks;~Release(){for(const auto block:blocks)(void)self.free_heap(memory,block,state);}} release{*this,memory,state,blocks};
    const auto scratch=[&](std::size_t size)->std::uint64_t{const auto block=allocate_heap(memory,std::max<std::size_t>(size,1));if(!block)throw MarshalError{out_of_memory};blocks.push_back(block);return block;};
    const auto api=[&](std::string_view dll,std::string_view symbol,std::initializer_list<std::uint64_t> arguments){return nested_service(state,memory,dll,symbol,arguments);};
    const auto raise=[&](std::uint32_t code){
        start_callbacks(state,memory,{{resolve("kernel32.dll","RaiseException").value,{code,1,0,0}}},Continuation::Return,0);return std::optional<std::uint32_t>{};};
    try {
        const auto context=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)},32))&0xffffU;
        const bool inproc=context==3 || context==4;
        const auto pointer=arg(operation=="UserFree"?1:2);
        if(type=="HSTRING"){
            const auto finish=[&](std::uint64_t result){arg.return_to(state,result);return std::optional<std::uint32_t>{};};
            const bool by_handle=context==3;
            if(operation=="UserFree"){
                (void)api("combase.dll","WindowsDeleteString",{memory.load(GuestAddress{pointer},bits)});
                return finish(0);
            }
            const bool sizing=operation=="UserSize";
            const auto start=sizing?static_cast<std::uint32_t>(arg(1)):arg(1);
            const auto aligned=(start+7)&~std::uint64_t{7};
            if(aligned<start)throw MarshalError{bad_stub_data};
            const auto header=by_handle?2*unit:8U;
            if(operation=="UserUnmarshal"){
                memory.check(GuestAddress{aligned},header,Access::Read);
                if(by_handle){
                    memory.store(GuestAddress{pointer},bits,memory.load(GuestAddress{aligned+unit},bits));
                    return finish(aligned+header);
                }
                const auto bytes=memory.load(GuestAddress{aligned+4},32);
                if(bytes&1)throw MarshalError{bad_stub_data};
                if(bytes)memory.check(GuestAddress{aligned+header},bytes,Access::Read);
                if(api("combase.dll","WindowsCreateString",{aligned+header,bytes/2,pointer}))throw MarshalError{14};
                return finish(aligned+header+bytes);
            }
            const auto value=memory.load(GuestAddress{pointer},bits);
            const auto text=by_handle?WinrtString{}:winrt_string(memory,value);
            const auto bytes=std::uint64_t{text.length}*2;
            if(bytes>UINT32_MAX)throw MarshalError{bad_stub_data};
            const auto end=aligned+header+bytes;
            if(sizing)return finish(static_cast<std::uint32_t>(end));
            memory.check(GuestAddress{aligned},header+bytes,Access::Write);
            if(by_handle){
                if(api("combase.dll","WindowsDuplicateString",{value,aligned+unit}))throw MarshalError{14};
            }else{
                if(bytes)memory.move(GuestAddress{aligned+header},GuestAddress{text.data},bytes);
                memory.store(GuestAddress{aligned+4},32,bytes);
            }
            memory.store(GuestAddress{aligned},32,x86_?inproc_call:inproc64_call);
            return finish(end);
        }
        const auto wire=wire_type(type);
        const auto read_guest=[&](std::uint64_t address,std::size_t size){std::vector<std::uint8_t> data(size);if(size)memory.read(GuestAddress{address},data);return data;};
        const auto global_bytes=[&](std::uint64_t handle){
            const auto size=api("kernel32.dll","GlobalSize",{handle});const auto locked=size?api("kernel32.dll","GlobalLock",{handle}):0;
            if(size && !locked)throw MarshalError{bad_stub_data};
            auto data=read_guest(locked,static_cast<std::size_t>(size));if(locked)(void)api("kernel32.dll","GlobalUnlock",{handle});return data;
        };
        const auto new_global=[&](std::span<const std::uint8_t> data,std::uint64_t existing){
            const auto handle=existing?api("kernel32.dll","GlobalReAlloc",{existing,data.size(),2}):api("kernel32.dll","GlobalAlloc",{2,data.size()});
            if(!handle)throw MarshalError{out_of_memory};
            if(!data.empty()){const auto locked=api("kernel32.dll","GlobalLock",{handle});if(!locked)throw MarshalError{out_of_memory};
                memory.write(GuestAddress{locked},data);(void)api("kernel32.dll","GlobalUnlock",{handle});}
            return handle;
        };
        const auto in_guest=[&](std::span<const std::uint8_t> data){const auto block=scratch(data.size());if(!data.empty())memory.write(GuestAddress{block},data);return block;};
        // 同程序：handle 本身（x86 為 long，x64 為 __int64）；回傳 false 表示需要傳內容。
        const auto put_inproc=[&](NdrWriter& w,std::uint64_t handle){
            if(!inproc)return false;if(x86_){w.put(inproc_call,4);w.put(handle,4);}else {w.put(inproc64_call,4);w.put(handle,8);}return true;};
        const auto metafile_bits=[&](std::uint64_t handle,bool enhanced){
            const auto symbol=enhanced?"GetEnhMetaFileBits":"GetMetaFileBitsEx";const auto size=api("gdi32.dll",symbol,{handle,0,0});if(!size)throw MarshalError{bad_stub_data};
            const auto buffer=scratch(static_cast<std::size_t>(size));if(api("gdi32.dll",symbol,{handle,size,buffer})!=size)throw MarshalError{bad_stub_data};
            return read_guest(buffer,static_cast<std::size_t>(size));
        };
        // userHMETAFILE／userHENHMETAFILE 的遠端表示：referent、BYTE_BLOB（MaxCount、clSize、資料）。
        const auto put_metafile_remote=[&](NdrWriter& w,std::uint64_t handle,bool enhanced){
            w.put(remote_call,4);w.put(handle?0x20000U:0U,4);if(!handle)return;
            const auto data=metafile_bits(handle,enhanced);w.put(data.size(),4);w.put(data.size(),4);w.raw(data);
        };
        const auto get_metafile_remote=[&](NdrReader& r,bool enhanced)->std::uint64_t{
            if(r.get(4)!=remote_call)throw MarshalError{invalid_tag};if(!r.get(4))return 0;
            const auto maximum=r.get(4),size=r.get(4);if(size!=maximum)throw MarshalError{bad_stub_data};const auto data=r.raw(size);
            const auto handle=enhanced?api("gdi32.dll","SetEnhMetaFileBits",{data.size(),in_guest(data)}):api("gdi32.dll","SetMetaFileBitsEx",{data.size(),in_guest(data)});
            if(!handle)throw MarshalError{bad_stub_data};return handle;
        };
        const auto encode=[&](NdrWriter& w){
            const auto value=wire==Wire::ClipFormat?memory.load(GuestAddress{pointer},16):memory.load(GuestAddress{pointer},bits);
            switch(wire){
            case Wire::Handle:
                if(context==2)throw MarshalError{bad_stub_data};
                w.put(inproc?inproc_call:remote_call,4);w.put(value,4);break;
            case Wire::ClipFormat:{
                if(context!=2 || value<0xc000){w.put(inproc_call,4);w.put(value,4);break;}
                const auto buffer=scratch(512);const auto length=api("user32.dll","GetClipboardFormatNameW",{value,buffer,256});if(!length)throw MarshalError{bad_stub_data};
                w.put(remote_call,4);w.put(0x20000,4);w.put(length+1,4);w.put(0,4);w.put(length+1,4);
                auto text=read_guest(buffer,static_cast<std::size_t>(length*2));text.push_back(0);text.push_back(0);w.raw(text);break;}
            case Wire::Global:case Wire::Bitmap:case Wire::Palette:case Wire::EnhMetafile:case Wire::Metafile:case Wire::MetafilePict:{
                if(put_inproc(w,value))break;
                if(wire==Wire::EnhMetafile || wire==Wire::Metafile){put_metafile_remote(w,value,wire==Wire::EnhMetafile);break;}
                w.put(remote_call,4);w.put(value?0x20000U:0U,4);if(!value)break;
                if(wire==Wire::Global){const auto data=global_bytes(value);w.put(data.size(),4);w.put(0,4);w.put(data.size(),4);w.raw(data);}
                else if(wire==Wire::Bitmap){
                    // userBITMAP：BITMAP 欄位與裝置相依位元（GetBitmapBits，列以 WORD 對齊）。
                    const auto info=scratch(32);if(!api("gdi32.dll","GetObjectW",{value,x86_?24U:32U,info}))throw MarshalError{bad_stub_data};
                    const auto at=[&](unsigned offset,unsigned size){return memory.load(GuestAddress{info+offset},static_cast<std::uint8_t>(size*8));};
                    const auto size=at(12,4)*at(8,4);if(size>256U*1024U*1024U)throw MarshalError{bad_stub_data};
                    const auto buffer=scratch(static_cast<std::size_t>(size));if(size && api("gdi32.dll","GetBitmapBits",{value,size,buffer})!=size)throw MarshalError{bad_stub_data};
                    w.put(size,4);w.put(at(0,4),4);w.put(at(4,4),4);w.put(at(8,4),4);w.put(at(12,4),4);w.put(at(16,2),2);w.put(at(18,2),2);w.put(size,4);
                    w.raw(read_guest(buffer,static_cast<std::size_t>(size)));
                }else if(wire==Wire::Palette){
                    const auto count=api("gdi32.dll","GetPaletteEntries",{value,0,0,0});if(!count)throw MarshalError{bad_stub_data};
                    const auto entries=scratch(static_cast<std::size_t>(count*4));if(api("gdi32.dll","GetPaletteEntries",{value,0,count,entries})!=count)throw MarshalError{bad_stub_data};
                    w.put(count,4);w.put(0x300,2);w.put(count,2);w.raw(read_guest(entries,static_cast<std::size_t>(count*4)));
                }else {
                    // remoteMETAFILEPICT {mm, xExt, yExt, userHMETAFILE*}；METAFILEPICT 的 hMF 在 x86 位移 12、x64 位移 16。
                    const auto picture=global_bytes(value);if(picture.size()<(x86_?16U:24U))throw MarshalError{bad_stub_data};
                    const auto field=[&](unsigned offset,unsigned size){std::uint64_t result=0;for(unsigned i=0;i<size;++i)result|=std::uint64_t{picture[offset+i]}<<(i*8);return result;};
                    const auto metafile=field(x86_?12:16,unit);
                    w.put(field(0,4),4);w.put(field(4,4),4);w.put(field(8,4),4);w.put(metafile?0x20004U:0U,4);
                    if(metafile)put_metafile_remote(w,metafile,false);
                }
                break;}
            case Wire::Snb:{
                // 字串指標陣列以 NULL 結束；字元數含每個字串的 NUL。
                std::vector<std::uint8_t> text;std::uint32_t strings=0;
                if(value)for(;strings<65536;++strings){const auto item=memory.load(GuestAddress{value+std::uint64_t{strings}*unit},bits);if(!item)break;
                    const auto content=read_utf16(memory,GuestAddress{item},32768);for(const auto c:content){text.push_back(static_cast<std::uint8_t>(c));text.push_back(static_cast<std::uint8_t>(c>>8));}
                    text.push_back(0);text.push_back(0);}
                w.put(text.size()/2,4);w.put(strings,4);w.put(text.size()/2,4);w.raw(text);break;}
            case Wire::Bstr:{
                std::vector<std::uint8_t> data;std::uint32_t flags=UINT32_MAX;
                if(value){
                    const auto length=static_cast<std::uint32_t>(api("oleaut32.dll","SysStringByteLen",{value}));
                    flags=length;data=read_guest(value,length);if(length%2)data.push_back(0);
                }
                w.put(data.size()/2,4);w.put(flags,4);w.put(data.size()/2,4);w.raw(data);break;}
            }
        };
        const auto decode=[&](NdrReader& r){
            const auto existing=wire==Wire::ClipFormat?0:memory.load(GuestAddress{pointer},bits);
            const auto store=[&](std::uint64_t value){if(wire==Wire::ClipFormat)memory.store(GuestAddress{pointer},16,value);else memory.store(GuestAddress{pointer},bits,value);};
            switch(wire){
            case Wire::Handle:{
                const auto tag=r.get(4);if(tag!=inproc_call && tag!=remote_call)throw MarshalError{invalid_tag};
                store(static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(r.get(4)))));break;}
            case Wire::ClipFormat:{
                const auto tag=r.get(4);
                if(tag==inproc_call){store(r.get(4)&0xffffU);break;}
                if(tag!=remote_call)throw MarshalError{invalid_tag};
                if(!r.get(4))throw MarshalError{bad_stub_data};
                const auto maximum=r.get(4),offset=r.get(4),actual=r.get(4);
                if(offset || !actual || actual>maximum || actual>256)throw MarshalError{bad_stub_data};
                const auto text=r.raw(actual*2);if(text[text.size()-1] || text[text.size()-2])throw MarshalError{bad_stub_data};
                const auto format=api("user32.dll","RegisterClipboardFormatW",{in_guest(text)});if(!format)throw MarshalError{bad_stub_data};store(format);break;}
            case Wire::Global:case Wire::Bitmap:case Wire::Palette:case Wire::EnhMetafile:case Wire::Metafile:case Wire::MetafilePict:{
                const auto tag=r.get(4);
                if((x86_ && tag==inproc_call) || (!x86_ && tag==inproc64_call)){store(r.get(x86_?4:8));break;}
                if(tag!=remote_call)throw MarshalError{invalid_tag};
                if(wire==Wire::EnhMetafile || wire==Wire::Metafile){r.at-=4;store(get_metafile_remote(r,wire==Wire::EnhMetafile));break;}
                if(!r.get(4)){store(0);break;}
                if(wire==Wire::Global){
                    const auto maximum=r.get(4);(void)r.get(4);const auto size=r.get(4);if(size!=maximum)throw MarshalError{bad_stub_data};
                    store(new_global(r.raw(size),existing));
                }else if(wire==Wire::Bitmap){
                    const auto maximum=r.get(4);const auto type=r.get(4),width=r.get(4),height=r.get(4),width_bytes=r.get(4),planes=r.get(2),depth=r.get(2),size=r.get(4);(void)type;
                    if(size!=maximum || size!=width_bytes*height)throw MarshalError{bad_stub_data};const auto data=r.raw(size);
                    const auto bitmap=api("gdi32.dll","CreateBitmap",{width,height,planes,depth,data.empty()?0:in_guest(data)});if(!bitmap)throw MarshalError{out_of_memory};store(bitmap);
                }else if(wire==Wire::Palette){
                    const auto maximum=r.get(4);const auto version=r.get(2),count=r.get(2);if(count!=maximum || !count)throw MarshalError{bad_stub_data};
                    const auto entries=r.raw(count*4);std::vector<std::uint8_t> logical{static_cast<std::uint8_t>(version),static_cast<std::uint8_t>(version>>8),static_cast<std::uint8_t>(count),static_cast<std::uint8_t>(count>>8)};
                    logical.insert(logical.end(),entries.begin(),entries.end());
                    const auto palette=api("gdi32.dll","CreatePalette",{in_guest(logical)});if(!palette)throw MarshalError{out_of_memory};store(palette);
                }else {
                    const auto mm=r.get(4),x=r.get(4),y=r.get(4),metafile_ref=r.get(4);
                    const auto metafile=metafile_ref?get_metafile_remote(r,false):0;
                    std::vector<std::uint8_t> picture(x86_?16U:24U);
                    const auto put=[&](unsigned offset,std::uint64_t field,unsigned size){for(unsigned i=0;i<size;++i)picture[offset+i]=static_cast<std::uint8_t>(field>>(i*8));};
                    put(0,mm,4);put(4,x,4);put(8,y,4);put(x86_?12:16,metafile,unit);store(new_global(picture,0));
                }
                break;}
            case Wire::Snb:{
                const auto maximum=r.get(4),strings=r.get(4),chars=r.get(4);if(chars!=maximum || strings>chars)throw MarshalError{bad_stub_data};
                const auto text=r.raw(chars*2);std::uint32_t terminators=0;for(std::size_t i=0;i+1<text.size();i+=2)if(!text[i] && !text[i+1])++terminators;
                if(terminators!=strings || (chars && (text[text.size()-1] || text[text.size()-2])))throw MarshalError{bad_stub_data};
                if(!strings){store(0);break;}
                const auto table=(std::uint64_t{strings}+1)*unit;const auto block=api("ole32.dll","CoTaskMemAlloc",{table+text.size()});if(!block)throw MarshalError{out_of_memory};
                memory.write(GuestAddress{block+table},text);std::uint64_t cursor=block+table;
                for(std::uint32_t i=0;i<strings;++i){memory.store(GuestAddress{block+std::uint64_t{i}*unit},bits,cursor);while(memory.load(GuestAddress{cursor},16))cursor+=2;cursor+=2;}
                memory.store(GuestAddress{block+std::uint64_t{strings}*unit},bits,0);store(block);break;}
            case Wire::Bstr:{
                const auto maximum=r.get(4),flags=r.get(4),size=r.get(4);if(size!=maximum)throw MarshalError{bad_stub_data};const auto data=r.raw(size*2);
                if(flags!=UINT32_MAX && flags>data.size())throw MarshalError{bad_stub_data};
                if(existing)(void)api("oleaut32.dll","SysFreeString",{existing});
                if(flags==UINT32_MAX){store(0);break;}
                const auto text=api("oleaut32.dll","SysAllocStringByteLen",{data.empty()?0:in_guest(data),flags});if(!text)throw MarshalError{out_of_memory};store(text);
                break;}
            }
        };
        if(operation=="UserSize" || operation=="UserMarshal"){
            const bool sizing=operation=="UserSize";NdrWriter w;w.base=sizing?static_cast<std::uint32_t>(arg(1)):arg(1);encode(w);
            if(!sizing && !w.bytes.empty())memory.write(GuestAddress{arg(1)},w.bytes);
            arg.return_to(state,sizing?static_cast<std::uint32_t>(w.base+w.bytes.size()):arg(1)+w.bytes.size());return std::nullopt;
        }
        if(operation=="UserUnmarshal"){NdrReader r{memory,arg(1)};decode(r);arg.return_to(state,r.at);return std::nullopt;}
        // UserFree：只釋放跨程序重建的物件；BSTR、HSTRING 與 SNB 一律釋放接收端配置。
        const auto value=wire==Wire::ClipFormat?0:memory.load(GuestAddress{pointer},bits);
        if(value)switch(wire){
        case Wire::Global:if(!inproc)(void)api("kernel32.dll","GlobalFree",{value});break;
        case Wire::Bitmap:case Wire::Palette:if(!inproc)(void)api("gdi32.dll","DeleteObject",{value});break;
        case Wire::EnhMetafile:if(!inproc)(void)api("gdi32.dll","DeleteEnhMetaFile",{value});break;
        case Wire::Metafile:if(!inproc)(void)api("gdi32.dll","DeleteMetaFile",{value});break;
        case Wire::MetafilePict:
            if(!inproc){const auto picture=global_bytes(value);if(picture.size()>=(x86_?16U:24U)){std::uint64_t metafile=0;for(unsigned i=0;i<unit;++i)metafile|=std::uint64_t{picture[(x86_?12U:16U)+i]}<<(i*8);
                    if(metafile)(void)api("gdi32.dll","DeleteMetaFile",{metafile});}(void)api("kernel32.dll","GlobalFree",{value});}
            break;
        case Wire::Snb:(void)api("ole32.dll","CoTaskMemFree",{value});break;
        case Wire::Bstr:(void)api("oleaut32.dll","SysFreeString",{value});break;
        default:break;
        }
        arg.return_to(state,0);return std::nullopt;
    }catch(const MarshalError& error){return raise(error.code);}
    catch(const MemoryFault&){return raise(0xc0000005U);}
    catch(const std::bad_alloc&){return raise(out_of_memory);}
}
}

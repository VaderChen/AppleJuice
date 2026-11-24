// dbghelp 檔案工具：SymMatchFileName(W)、SymSetHomeDirectory(W)／SymGetHomeDirectory(W)、EnumDirTree(W)、
// SymSrvGetFileIndexInfo(W)、SymSrvGetFileIndexes(W)、SymSrvGetFileIndexString(W)、SymFindFileInPath(W)。
// - 檔案索引與符號伺服器相同：PE 映像與 DBG 為 TimeDateStamp＋SizeOfImage；PDB 7.0（MSF 7.00）為 GUID＋Age（DBI 串流的 Age，
//   沒有 DBI 時取 PDB 串流），PDB 2.0（JG）為簽章＋Age。映像的 PDB／DBG 名稱取自 CodeView（RSDS／NB10）與 MISC 偵錯目錄項目。
// - 符號路徑以 ';' 分隔：一般目錄直接找同名檔；srv*／symsrv*／cache* 元素中的本機目錄依符號存放區版面（檔名\索引\檔名）尋找，
//   網址元素（http／https）不連線。
// - 家目錄預設為 dbghelp.dll 所在的系統目錄；檔案系統唯讀時改用 CSIDL_COMMON_APPDATA。
#include "ansi_codepage.hpp"
#include "x64arm/windows_services.hpp"
#include "x64arm/windows_paths.hpp"
#include "x64arm/windows_system_paths.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace x64arm {
namespace {
struct BadFormat {};
std::uint64_t field(const std::vector<std::uint8_t>& bytes,std::uint64_t at,unsigned size){
    if(at>bytes.size() || size>bytes.size()-at)throw BadFormat{};
    std::uint64_t value=0;for(unsigned i=0;i<size;++i)value|=std::uint64_t{bytes[static_cast<std::size_t>(at)+i]}<<(8*i);return value;
}
std::u16string leaf_name(std::u16string_view path){return std::u16string(path.substr(path.find_last_of(u"\\/")+1));}
// 以 NUL 結尾（或到 limit 為止）的 8 位元字串。
std::u16string narrow_text(const std::vector<std::uint8_t>& bytes,std::size_t at,std::size_t limit){
    std::string text;for(std::size_t i=at;i<bytes.size() && i<at+limit && bytes[i];++i)text.push_back(static_cast<char>(bytes[i]));return utf8_to_utf16_lossy(text);
}
struct FileIndex {
    enum class Kind {Image,Pdb,Dbg} kind{};
    std::u16string dbg,pdb;bool stripped{},guid_present{};
    std::uint32_t timestamp{},size{},sig{},age{};std::array<std::uint8_t,16> guid{};
};
// PE 與 DBG 共用：IMAGE_DEBUG_DIRECTORY 陣列（每項 28 bytes）的 CodeView（2）與 MISC（4）項目。
void read_debug_entries(const std::vector<std::uint8_t>& bytes,std::uint64_t at,std::uint64_t count,
                        const std::function<std::optional<std::uint64_t>(std::uint32_t,std::uint32_t)>& offset,FileIndex& index){
    for(std::uint64_t i=0;i<count && i<1024;++i){
        const auto entry=at+i*28;const auto type=field(bytes,entry+12,4),size=field(bytes,entry+16,4);
        const auto position=offset(static_cast<std::uint32_t>(field(bytes,entry+20,4)),static_cast<std::uint32_t>(field(bytes,entry+24,4)));
        if(!position || *position>bytes.size() || size>bytes.size()-*position)continue;
        const auto data=static_cast<std::size_t>(*position);
        if(type==2 && size>=16){
            const auto magic=field(bytes,data,4);
            if(magic==0x53445352U && size>=24){ // RSDS：GUID、Age、PDB 路徑（UTF-8）
                std::copy_n(bytes.begin()+static_cast<std::ptrdiff_t>(data+4),16,index.guid.begin());index.guid_present=true;
                index.age=static_cast<std::uint32_t>(field(bytes,data+20,4));index.pdb=leaf_name(narrow_text(bytes,data+24,static_cast<std::size_t>(size-24)));
            }else if(magic==0x3031424eU){ // NB10：位移、簽章、Age、PDB 路徑
                index.sig=static_cast<std::uint32_t>(field(bytes,data+8,4));index.age=static_cast<std::uint32_t>(field(bytes,data+12,4));
                index.pdb=leaf_name(narrow_text(bytes,data+16,static_cast<std::size_t>(size-16)));
            }
        }else if(type==4 && size>=12 && field(bytes,data,4)==1){ // IMAGE_DEBUG_MISC_EXENAME：DBG 檔名
            const auto length=std::min<std::uint64_t>(field(bytes,data+4,4),size);
            if(length>12){
                std::u16string name;
                if(bytes[data+8]){for(std::size_t at_char=data+12;at_char+1<data+length;at_char+=2){const auto c=static_cast<char16_t>(field(bytes,at_char,2));if(!c)break;name.push_back(c);}}
                else name=narrow_text(bytes,data+12,static_cast<std::size_t>(length-12));
                index.dbg=leaf_name(name);
            }
        }
    }
}
FileIndex image_index(const std::vector<std::uint8_t>& bytes){
    FileIndex index;index.kind=FileIndex::Kind::Image;
    const auto pe=field(bytes,60,4);if(field(bytes,pe,4)!=0x4550U)throw BadFormat{};
    const auto sections=field(bytes,pe+6,2),optional_size=field(bytes,pe+20,2);
    index.timestamp=static_cast<std::uint32_t>(field(bytes,pe+8,4));index.stripped=(field(bytes,pe+22,2)&0x200U)!=0;
    const auto optional=pe+24,magic=field(bytes,optional,2);if(magic!=0x10bU && magic!=0x20bU)throw BadFormat{};
    index.size=static_cast<std::uint32_t>(field(bytes,optional+56,4));
    const bool plus=magic==0x20bU;const auto directories=optional+(plus?112U:96U),count=field(bytes,optional+(plus?108U:92U),4);
    if(count<=6 || directories+56>optional+optional_size)return index;
    const auto table=optional+optional_size;
    const auto to_offset=[&](std::uint32_t address)->std::optional<std::uint64_t>{
        for(std::uint64_t s=0;s<sections && s<96;++s){
            const auto header=table+s*40;const auto virtual_size=field(bytes,header+8,4),virtual_address=field(bytes,header+12,4),raw_size=field(bytes,header+16,4),raw=field(bytes,header+20,4);
            if(address>=virtual_address && address-virtual_address<std::max(virtual_size,raw_size) && address-virtual_address<raw_size)return raw+(address-virtual_address);
        }
        return std::nullopt;
    };
    const auto rva=static_cast<std::uint32_t>(field(bytes,directories+48,4));const auto size=field(bytes,directories+52,4);
    if(rva && size)if(const auto at=to_offset(rva))
        read_debug_entries(bytes,*at,size/28,[&](std::uint32_t data_rva,std::uint32_t pointer){return pointer?std::optional<std::uint64_t>{pointer}:to_offset(data_rva);},index);
    return index;
}
FileIndex dbg_index(const std::vector<std::uint8_t>& bytes){
    // IMAGE_SEPARATE_DEBUG_HEADER（48 bytes）後接節區標頭、匯出名稱與偵錯目錄。
    FileIndex index;index.kind=FileIndex::Kind::Dbg;
    index.stripped=(field(bytes,6,2)&0x200U)!=0;index.timestamp=static_cast<std::uint32_t>(field(bytes,8,4));index.size=static_cast<std::uint32_t>(field(bytes,20,4));
    const auto sections=field(bytes,24,4),exported=field(bytes,28,4),debug_size=field(bytes,32,4);
    if(sections>96)throw BadFormat{};
    read_debug_entries(bytes,48+sections*40+exported,debug_size/28,[](std::uint32_t,std::uint32_t pointer){return pointer?std::optional<std::uint64_t>{pointer}:std::nullopt;},index);
    return index;
}
// MSF 7.00：區塊大小、目錄大小與目錄區塊表位址；目錄＝串流數、各串流大小、各串流的區塊編號。
std::vector<std::vector<std::uint8_t>> msf_streams(const std::vector<std::uint8_t>& bytes,std::size_t wanted){
    const auto block=field(bytes,32,4),directory_size=field(bytes,44,4),map=field(bytes,52,4);
    if(block<512 || block>65536 || (block&(block-1)) || directory_size>16U*1024U*1024U)throw BadFormat{};
    const auto read_blocks=[&](std::vector<std::uint64_t> numbers,std::uint64_t size){
        std::vector<std::uint8_t> out;out.reserve(static_cast<std::size_t>(size));
        for(const auto number:numbers){const auto at=number*block;if(at>bytes.size() || block>bytes.size()-at)throw BadFormat{};
            out.insert(out.end(),bytes.begin()+static_cast<std::ptrdiff_t>(at),bytes.begin()+static_cast<std::ptrdiff_t>(at+block));}
        out.resize(static_cast<std::size_t>(size));return out;
    };
    const auto directory_blocks=(directory_size+block-1)/block;std::vector<std::uint64_t> numbers;
    for(std::uint64_t i=0;i<directory_blocks;++i)numbers.push_back(field(bytes,map*block+i*4,4));
    const auto directory=read_blocks(numbers,directory_size);
    const auto count=field(directory,0,4);if(count>65536)throw BadFormat{};
    std::vector<std::vector<std::uint8_t>> streams;std::uint64_t cursor=4+count*4;
    for(std::uint64_t s=0;s<count && s<=wanted;++s){
        auto size=field(directory,4+s*4,4);if(size==0xffffffffU)size=0;
        const auto blocks=(size+block-1)/block;std::vector<std::uint64_t> list;
        for(std::uint64_t i=0;i<blocks;++i)list.push_back(field(directory,cursor+i*4,4));cursor+=blocks*4;
        streams.push_back(read_blocks(std::move(list),size));
    }
    return streams;
}
// PDB 2.00（JG）：頁面大小與根串流的頁面表（16 位元頁碼）；根串流＝串流數、各串流 {大小, 保留}、各串流頁碼。
std::vector<std::vector<std::uint8_t>> jg_streams(const std::vector<std::uint8_t>& bytes,std::size_t wanted){
    const auto page=field(bytes,44,4),root_size=field(bytes,52,4);
    if(page<512 || page>65536 || root_size>16U*1024U*1024U)throw BadFormat{};
    const auto read_pages=[&](std::uint64_t table,std::uint64_t size,const std::vector<std::uint8_t>& source){
        std::vector<std::uint8_t> out;const auto pages=(size+page-1)/page;
        for(std::uint64_t i=0;i<pages;++i){const auto at=field(source,table+i*2,2)*page;if(at>bytes.size() || page>bytes.size()-at)throw BadFormat{};
            out.insert(out.end(),bytes.begin()+static_cast<std::ptrdiff_t>(at),bytes.begin()+static_cast<std::ptrdiff_t>(at+page));}
        out.resize(static_cast<std::size_t>(size));return out;
    };
    const auto root=read_pages(60,root_size,bytes);
    const auto count=field(root,0,2);std::vector<std::vector<std::uint8_t>> streams;std::uint64_t cursor=4+count*8;
    for(std::uint64_t s=0;s<count && s<=wanted;++s){
        auto size=field(root,4+s*8,4);if(size==0xffffffffU)size=0;
        streams.push_back(read_pages(cursor,size,root));cursor+=((size+page-1)/page)*2;
    }
    return streams;
}
FileIndex pdb_index(const std::vector<std::uint8_t>& bytes,bool msf){
    FileIndex index;index.kind=FileIndex::Kind::Pdb;
    const auto streams=msf?msf_streams(bytes,3):jg_streams(bytes,1);
    if(streams.size()<2 || streams[1].size()<12)throw BadFormat{};
    // PDB 串流：Version、Signature、Age，7.0 之後接 GUID。
    const auto& info=streams[1];index.sig=static_cast<std::uint32_t>(field(info,4,4));index.age=static_cast<std::uint32_t>(field(info,8,4));
    if(msf){
        if(info.size()<28)throw BadFormat{};std::copy_n(info.begin()+12,16,index.guid.begin());index.guid_present=true;
        // DBI 串流（3）：VersionSignature、VersionHeader、Age；與映像 CodeView 記錄的 Age 一致。
        if(streams.size()>3 && streams[3].size()>=12 && field(streams[3],0,4)==0xffffffffU)index.age=static_cast<std::uint32_t>(field(streams[3],8,4));
    }
    return index;
}
FileIndex file_index(const std::vector<std::uint8_t>& bytes){
    static constexpr char msf7[]="Microsoft C/C++ MSF 7.00\r\n\x1a""DS\0\0";
    static constexpr char jg[]="Microsoft C/C++ program database 2.00\r\n\x1a""JG\0";
    const auto starts=[&](const char* magic,std::size_t length){return bytes.size()>=length && std::equal(magic,magic+length,bytes.begin(),[](char a,std::uint8_t b){return static_cast<std::uint8_t>(a)==b;});};
    if(starts(msf7,sizeof msf7))return pdb_index(bytes,true);
    if(starts(jg,sizeof jg))return pdb_index(bytes,false);
    if(bytes.size()>=64 && bytes[0]=='M' && bytes[1]=='Z')return image_index(bytes);
    if(bytes.size()>=48 && bytes[0]=='D' && bytes[1]=='I')return dbg_index(bytes);
    throw BadFormat{};
}
// 符號存放區的索引字串：映像與 DBG 為 %08X%x（時間戳記、映像大小）；PDB 為 GUID 十六進位（無分隔）＋Age，舊式 PDB 為簽章＋Age。
std::u16string index_string(const FileIndex& index){
    char text[64];
    if(index.kind!=FileIndex::Kind::Pdb)std::snprintf(text,sizeof text,"%08X%x",index.timestamp,index.size);
    else if(index.guid_present){
        const auto& g=index.guid;const auto d1=static_cast<unsigned>(g[0])|(static_cast<unsigned>(g[1])<<8)|(static_cast<unsigned>(g[2])<<16)|(static_cast<unsigned>(g[3])<<24);
        std::snprintf(text,sizeof text,"%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%x",d1,static_cast<unsigned>(g[4])|(static_cast<unsigned>(g[5])<<8),static_cast<unsigned>(g[6])|(static_cast<unsigned>(g[7])<<8),
                      g[8],g[9],g[10],g[11],g[12],g[13],g[14],g[15],index.age);
    }else std::snprintf(text,sizeof text,"%08X%x",index.sig,index.age);
    return utf8_to_utf16(text);
}
bool same_fold(std::u16string a,std::u16string b){
    for(auto& c:a)c=unicode_case_unit(c,true);for(auto& c:b)c=unicode_case_unit(c,true);return a==b;
}
bool starts_fold(std::u16string_view text,std::u16string_view prefix){return text.size()>=prefix.size() && same_fold(std::u16string(text.substr(0,prefix.size())),std::u16string(prefix));}
}

std::optional<std::uint32_t> WindowsServices::invoke_dbghelp_files(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const auto name=service.symbol;const bool wide=name.ends_with('W');
    const unsigned unit=wide?2U:1U;
    const auto read_text=[wide](const GuestMemory& m,std::uint64_t pointer){return wide?read_utf16(m,GuestAddress{pointer},32768):ansi_to_utf16(read_ansi(m,pointer,32768));};
    const auto encode=[wide](const std::u16string& text){
        if(wide)return utf16_bytes(text);auto narrow=utf16_to_ansi(text);std::vector<std::uint8_t> bytes(narrow.begin(),narrow.end());bytes.push_back(0);return bytes;};
    // 開檔並取得索引：找不到檔案等開檔錯誤原樣回傳，格式不符為 ERROR_BAD_FORMAT。
    const auto index_of=[this](const std::u16string& path,std::uint32_t& error)->std::optional<FileIndex>{
        const auto opened=filesystem_.open(utf16_to_utf8(normalized_windows_path(path,runtime_.current_directory)));
        if(opened.error || !opened.bytes){error=opened.error?opened.error:2U;return std::nullopt;}
        try{return file_index(*opened.bytes);}catch(const BadFormat&){error=11;return std::nullopt;}
    };
    const auto exists=[this](const std::u16string& path){
        const auto info=filesystem_.information(utf16_to_utf8(normalized_windows_path(path,runtime_.current_directory)));return !info.error && !(info.metadata.attributes&16U);};
    if(name=="EnumDirTree" || name=="EnumDirTreeW" || name=="SymFindFileInPath" || name=="SymFindFileInPathW")
        return nested_continuation(state,memory,"dbghelp 檔案搜尋",[this,service,unit,read_text,encode,index_of,exists](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);const auto symbol=service.symbol;
            const auto finish=[&](bool ok,std::uint32_t error){if(!ok)last_error_=error;call.return_to(s,ok?1U:0U);return std::optional<std::uint32_t>{};};
            std::uint64_t buffer=0;
            struct Release {WindowsServices& self;GuestMemory& m;GuestState& s;std::uint64_t& block;~Release(){if(block)(void)self.free_heap(m,block,s);}} cleanup{*this,m,s,buffer};
            try{
                if(symbol.starts_with("EnumDirTree")){
                    // (hProcess, RootPath, InputPathName, OutputPathBuffer[MAX_PATH], Callback, CallbackData)：深度優先列出同名檔案，
                    // 每個符合者寫入輸出緩衝並呼叫回呼；回呼傳回 TRUE 即停止並成功。沒有回呼時找到第一個即成功。
                    if(!call(1) || !call(2))co_return finish(false,87);
                    const auto callback=call(4),data=call(5),output=call(3);
                    const auto matches=find_tree_files(read_text(m,call(1)),read_text(m,call(2)),callback?4096U:1U);
                    if(callback && !output){buffer=allocate_heap(m,261*unit);if(!buffer)co_return finish(false,8);}
                    for(const auto& path:matches){
                        if(path.size()>=260)continue;const auto bytes=encode(path);
                        if(output)m.write(GuestAddress{output},bytes);if(buffer)m.write(GuestAddress{buffer},bytes);
                        if(!callback)co_return finish(true,0);
                        if(co_await service_callback::call_value(callback,{output?output:buffer,data})&UINT32_MAX)co_return finish(true,0);
                    }
                    co_return finish(false,2);
                }
                // SymFindFileInPath(hProcess, SearchPath, FileName, id, two, three, flags, FoundFile[MAX_PATH], callback, context)：
                // 逐一搜尋路徑元素；指定 id 時以檔案索引核對（SSRVOPT_GUIDPTR 8：id 指向 GUID；SSRVOPT_DWORDPTR 4：指向 DWORD；
                // 其餘為 DWORD 值），two 為映像大小或 PDB Age。回呼傳回 FALSE 表示接受該檔案，TRUE 繼續找下一個。
                if(!call(2) || !call(7))co_return finish(false,87);
                const auto flags=static_cast<std::uint32_t>(call(6)),two=static_cast<std::uint32_t>(call(4));
                std::array<std::uint8_t,16> guid{};std::uint32_t id=0;bool check=false;
                if(flags&8U){if(!call(3))co_return finish(false,87);m.read(GuestAddress{call(3)},guid);check=true;}
                else if(flags&4U){if(!call(3))co_return finish(false,87);id=static_cast<std::uint32_t>(m.load(GuestAddress{call(3)},32));check=true;}
                else {id=static_cast<std::uint32_t>(call(3));check=id!=0 || two!=0;}
                std::u16string path;
                if(call(1))path=read_text(m,call(1));
                else if(const auto session=runtime_.symbol_sessions.find(call(0));session!=runtime_.symbol_sessions.end())path=session->second.search_path;
                else co_return finish(false,6);
                const auto file=leaf_name(read_text(m,call(2)));if(file.empty())co_return finish(false,87);
                // 要求的索引字串：GUID 為 PDB 版面；DWORD 依副檔名決定（.pdb 為簽章＋Age，其餘為映像時間戳記＋大小）。
                FileIndex wanted;wanted.kind=flags&8U?FileIndex::Kind::Pdb:file.size()>4 && same_fold(file.substr(file.size()-4),u".pdb")?FileIndex::Kind::Pdb:FileIndex::Kind::Image;
                if(flags&8U){wanted.guid=guid;wanted.guid_present=true;wanted.age=two;}
                else if(wanted.kind==FileIndex::Kind::Pdb){wanted.sig=id;wanted.age=two;}else {wanted.timestamp=id;wanted.size=two;}
                const auto matches=[&](const FileIndex& candidate){
                    if(!check)return true;
                    if(flags&8U)return candidate.kind==FileIndex::Kind::Pdb && candidate.guid_present && candidate.guid==guid && candidate.age==two;
                    if(candidate.kind==FileIndex::Kind::Pdb)return !candidate.guid_present?candidate.sig==id && candidate.age==two:false;
                    return candidate.timestamp==id && candidate.size==two;
                };
                std::vector<std::u16string> candidates;
                const auto join=[](std::u16string directory,const std::u16string& rest){if(!directory.empty() && directory.back()!=u'\\' && directory.back()!=u'/')directory+=u'\\';return directory+rest;};
                for(std::size_t at=0;at<=path.size();){
                    const auto end=path.find(u';',at);auto element=path.substr(at,end==path.npos?path.npos:end-at);at=end==path.npos?path.size()+1:end+1;
                    while(!element.empty() && element.front()==u' ')element.erase(0,1);while(!element.empty() && element.back()==u' ')element.pop_back();
                    if(element.empty())continue;
                    if(starts_fold(element,u"srv*") || starts_fold(element,u"symsrv*") || starts_fold(element,u"cache*")){
                        // 符號存放區：本機目錄依 檔名\索引\檔名 版面；symsrv* 的第二段是伺服器 DLL 名稱。
                        if(!check)continue;std::vector<std::u16string> parts;
                        for(std::size_t p=element.find(u'*')+1;p<=element.size();){const auto q=element.find(u'*',p);parts.push_back(element.substr(p,q==element.npos?element.npos:q-p));p=q==element.npos?element.size()+1:q+1;}
                        if(starts_fold(element,u"symsrv*") && !parts.empty())parts.erase(parts.begin());
                        for(const auto& store:parts)if(!store.empty() && !starts_fold(store,u"http://") && !starts_fold(store,u"https://"))
                            candidates.push_back(join(join(join(store,file),index_string(wanted)),file));
                    }else candidates.push_back(join(element,file));
                }
                for(const auto& candidate:candidates){
                    if(candidate.size()>=260 || !exists(candidate))continue;
                    std::uint32_t error=0;const auto index=check?index_of(candidate,error):std::optional<FileIndex>{FileIndex{}};
                    if(!index || !matches(*index))continue;
                    const auto bytes=encode(candidate);
                    if(call(8)){
                        if(!buffer){buffer=allocate_heap(m,261*unit);if(!buffer)co_return finish(false,8);}
                        m.write(GuestAddress{buffer},bytes);
                        if(co_await service_callback::call_value(call(8),{buffer,call(9)})&UINT32_MAX)continue;
                    }
                    m.write(GuestAddress{call(7)},bytes);co_return finish(true,0);
                }
                co_return finish(false,2);
            }catch(const MemoryFault&){co_return finish(false,998);}
            catch(const std::bad_alloc&){co_return finish(false,8);}
        });
    const WindowsCall arg(state,memory,service);
    const auto fail=[&](std::uint32_t error,std::uint64_t value=0){last_error_=error;arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    try{
        if(name=="SymMatchFileName" || name=="SymMatchFileNameW"){
            // (FileName, Match, FileNameStop*, MatchStop*)：自尾端往前比對（不分大小寫，'\\' 與 '/' 視為相同），Match 全部吻合即成功；
            // 兩個停止位置指向第一個不吻合的字元（全部吻合時為字串開頭的前一個位置）。
            if(!arg(0) || !arg(1))return fail(87);
            const auto file=read_text(memory,arg(0)),match=read_text(memory,arg(1));
            std::int64_t i=static_cast<std::int64_t>(file.size())-1,j=static_cast<std::int64_t>(match.size())-1;
            const auto separator=[](char16_t c){return c==u'\\' || c==u'/';};
            while(i>=0 && j>=0){
                const auto a=file[static_cast<std::size_t>(i)],b=match[static_cast<std::size_t>(j)];
                if(!(separator(a) && separator(b)) && unicode_case_unit(a,true)!=unicode_case_unit(b,true))break;--i;--j;
            }
            // ANSI 版的停止位置以位元組計算。
            const auto units=[&](const std::u16string& text,std::int64_t index)->std::int64_t{
                if(wide || index<0)return index;return static_cast<std::int64_t>(utf16_to_utf8(text.substr(0,static_cast<std::size_t>(index))).size());};
            const unsigned pointer=x86_?32:64;
            if(arg(2))memory.store(GuestAddress{arg(2)},static_cast<std::uint8_t>(pointer),arg(0)+static_cast<std::uint64_t>(units(file,i)*static_cast<std::int64_t>(unit)));
            if(arg(3))memory.store(GuestAddress{arg(3)},static_cast<std::uint8_t>(pointer),arg(1)+static_cast<std::uint64_t>(units(match,j)*static_cast<std::int64_t>(unit)));
            return done(j<0?1U:0U);
        }
        const auto default_home=[&]{return std::u16string(x86_?windows_wow64_directory:windows_system_directory);};
        if(name=="SymSetHomeDirectory" || name=="SymSetHomeDirectoryW"){
            // (hProcess, dir)：設定家目錄（符號快取 dir\sym、原始碼 dir\src）；NULL 恢復預設。唯讀檔案系統改用 CSIDL_COMMON_APPDATA。
            // 成功時傳回 dir；NULL 時傳回程序內保存的家目錄字串。
            std::u16string home=default_home();
            if(arg(1)){home=normalized_windows_path(read_text(memory,arg(1)),runtime_.current_directory);while(home.size()>3 && (home.back()==u'\\' || home.back()==u'/'))home.pop_back();}
            if(home.empty())return fail(87);
            if(!filesystem_.writable() && arg(1)){
                const auto found=runtime_.environment.find(u"programdata");home=found!=runtime_.environment.end() && !found->second.second.empty()?found->second.second:u"C:\\ProgramData";}
            runtime_.symbol_home_directory=home;
            if(arg(1))return done(arg(1));
            auto& block=runtime_.symbol_home_text[wide?1:0];const auto bytes=encode(home);
            if(block)(void)free_heap(memory,block,state);block=allocate_heap(memory,bytes.size());if(!block)return fail(8);
            memory.write(GuestAddress{block},bytes);return done(block);
        }
        if(name=="SymGetHomeDirectory" || name=="SymGetHomeDirectoryW"){
            // (type, dir, size)：hdBase（0）家目錄、hdSym（1）dir\sym、hdSrc（2）dir\src；size 以字元計，不足為 ERROR_INSUFFICIENT_BUFFER。
            const auto type=static_cast<std::uint32_t>(arg(0));if(type>2 || !arg(1))return fail(87);
            auto home=runtime_.symbol_home_directory.empty()?default_home():runtime_.symbol_home_directory;
            if(type==1)home+=u"\\sym";else if(type==2)home+=u"\\src";
            const auto bytes=encode(home);if(arg(2)<bytes.size()/unit)return fail(122);
            memory.write(GuestAddress{arg(1)},bytes);return done(arg(1));
        }
        if(name.starts_with("SymSrvGetFileIndexInfo")){
            // (File, SYMSRV_INDEX_INFO(W)*, Flags)：sizeofstruct 須為 828（A）／1608（W）。
            const std::uint32_t size=wide?1608U:828U;
            if(!arg(0) || !arg(1) || memory.load(GuestAddress{arg(1)},32)!=size)return fail(87);
            const auto path=read_text(memory,arg(0));std::uint32_t error=0;const auto index=index_of(path,error);if(!index)return fail(error);
            memory.check(GuestAddress{arg(1)},size,Access::Write);std::vector<std::uint8_t> bytes(size);
            const auto put=[&](std::size_t at,std::uint32_t value){for(unsigned i=0;i<4;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
            const auto put_text=[&](std::size_t at,const std::u16string& text){auto data=encode(text);data.resize(std::min<std::size_t>(data.size(),260*unit));std::copy(data.begin(),data.end(),bytes.begin()+static_cast<std::ptrdiff_t>(at));};
            const std::size_t file_at=4,stripped_at=wide?528:268,timestamp_at=stripped_at+4,size_at=stripped_at+8,dbg_at=stripped_at+12,pdb_at=dbg_at+261*unit,guid_at=wide?1584:804;
            put(0,size);put_text(file_at,leaf_name(path));put(stripped_at,index->stripped?1U:0U);put(timestamp_at,index->timestamp);put(size_at,index->size);
            put_text(dbg_at,index->kind==FileIndex::Kind::Dbg?leaf_name(path):index->dbg);put_text(pdb_at,index->kind==FileIndex::Kind::Pdb?leaf_name(path):index->pdb);
            if(index->guid_present)std::copy(index->guid.begin(),index->guid.end(),bytes.begin()+static_cast<std::ptrdiff_t>(guid_at));
            put(guid_at+16,index->sig);put(guid_at+20,index->age);
            memory.write(GuestAddress{arg(1)},bytes);return done(1);
        }
        if(name.starts_with("SymSrvGetFileIndexes")){
            // (File, GUID* Id, DWORD* Val1, DWORD* Val2, Flags)：映像為 {時間戳記}、映像大小；PDB 為 GUID（或簽章）、Age。Val2 為 0。
            if(!arg(0))return fail(87);
            std::uint32_t error=0;const auto index=index_of(read_text(memory,arg(0)),error);if(!index)return fail(error);
            std::array<std::uint8_t,16> id{};std::uint32_t first=0,second=0;
            if(index->kind==FileIndex::Kind::Pdb){if(index->guid_present)id=index->guid;else first=index->sig;second=index->age;}
            else {first=index->timestamp;second=index->size;}
            if(!index->guid_present || index->kind!=FileIndex::Kind::Pdb)for(unsigned i=0;i<4;++i)id[i]=static_cast<std::uint8_t>(first>>(8*i));
            if(arg(1))memory.write(GuestAddress{arg(1)},id);if(arg(2))memory.store(GuestAddress{arg(2)},32,second);if(arg(3))memory.store(GuestAddress{arg(3)},32,0);
            return done(1);
        }
        if(name.starts_with("SymSrvGetFileIndexString")){
            // (hProcess, SrvPath, File, Index, Size, Flags)：符號存放區目錄中的索引字串；Size 以字元計。
            if(!arg(2) || !arg(3))return fail(87);
            std::uint32_t error=0;const auto index=index_of(read_text(memory,arg(2)),error);if(!index)return fail(error);
            const auto bytes=encode(index_string(*index));if(arg(4)<bytes.size()/unit)return fail(122);
            memory.write(GuestAddress{arg(3)},bytes);return done(1);
        }
    }catch(const MemoryFault&){return fail(998);}
    catch(const std::bad_alloc&){return fail(8);}
    throw HostCallFault("未知的 dbghelp 檔案工具："+std::string(name));
}
bool dbghelp_file_tool(std::string_view name){
    for(const auto* candidate:{"SymMatchFileName","SymSetHomeDirectory","SymGetHomeDirectory","EnumDirTree","SymSrvGetFileIndexInfo","SymSrvGetFileIndexes","SymSrvGetFileIndexString","SymFindFileInPath"}){
        const std::string_view base(candidate);if(name==base || (name.size()==base.size()+1 && name.starts_with(base) && name.back()=='W'))return true;}
    return false;
}
}

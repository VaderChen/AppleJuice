#include "x64arm/windows_services.hpp"
#include "x64arm/security_descriptor.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_version.hpp"
#include <algorithm>
#include <optional>
#include <variant>

// netapi32：與 NetUserGetInfo／NetGetJoinInformation 相同的虛擬工作群組電腦模型（電腦 X64ARM、未提升的本機帳號 user、群組來自已知帳號表）。
// 沒有伺服器、瀏覽器與 NetBIOS 服務，相關查詢回傳該狀態下 Windows 的結果；不讀取宿主帳號或網路共用。
namespace x64arm {
namespace {
constexpr std::uint32_t nerr_success=0,more_data=234,invalid_level=124,bad_netpath=53,access_denied=5,invalid_parameter=87,insufficient_buffer=122;
constexpr std::uint32_t user_not_found=2221,group_not_found=2220,local_group_not_found=1376,server_not_started=2114,use_not_found=2250,dc_not_found=2453,no_such_domain=1355,no_sitename=1919,no_browser_servers=6118;
std::u16string folded(std::u16string value){for(auto& c:value)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return value;}
using Field=std::variant<std::uint32_t,std::u16string,std::vector<std::uint8_t>,std::nullptr_t>;
using Record=std::vector<Field>;
struct LocalGroup {std::string_view name;std::u16string_view comment;};
constexpr LocalGroup local_groups[]{
    {"Administrators",u"Administrators have complete and unrestricted access to the computer/domain"},
    {"Guests",u"Guests have the same access as members of the Users group by default, except for the Guest account which is further restricted"},
    {"Users",u"Users are prevented from making accidental or intentional system-wide changes and can run most applications"}};
}

std::span<const WindowsServiceDescriptor> netapi_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"netapi32.dll","NetApiBufferAllocate",2,0,0,1},{"netapi32.dll","NetapipBufferAllocate",2,0,0,1},{"netapi32.dll","NetApiBufferReallocate",3,0,0,2},{"netapi32.dll","NetApiBufferSize",2},
        {"netapi32.dll","NetWkstaGetInfo",3,0,0,2},{"netapi32.dll","NetServerGetInfo",3,0,0,2},{"netapi32.dll","NetWkstaUserGetInfo",3,0,0,2},
        {"netapi32.dll","NetWkstaUserEnum",7,0,0,0xa},{"netapi32.dll","NetWkstaTransportEnum",7,0,0,0xa},
        {"netapi32.dll","NetUserEnum",8,0,0,0x16},{"netapi32.dll","NetUserGetLocalGroups",8,0,0,0x2c},{"netapi32.dll","NetUserGetGroups",7,0,0,0x14},
        {"netapi32.dll","NetLocalGroupEnum",7,0,0,0xa},{"netapi32.dll","NetLocalGroupGetInfo",4,0,0,4},{"netapi32.dll","NetLocalGroupGetMembers",8,0,0,0x14},
        {"netapi32.dll","NetGroupEnum",7,0,0,0xa},{"netapi32.dll","NetGroupGetInfo",4,0,0,4},{"netapi32.dll","NetQueryDisplayInformation",7,0,0,0x1e},
        {"netapi32.dll","NetRemoteTOD",2},{"netapi32.dll","NetGetDCName",3},{"netapi32.dll","NetGetAnyDCName",3},
        {"netapi32.dll","DsGetDcNameA",6,0,0,0x10},{"netapi32.dll","DsGetDcNameW",6,0,0,0x10},{"netapi32.dll","DsGetSiteNameA",2},{"netapi32.dll","DsGetSiteNameW",2},
        {"netapi32.dll","NetServerEnum",9,0,0,0x4a},{"netapi32.dll","NetServerEnumEx",9,0,0,0x4a},
        {"netapi32.dll","NetShareEnum",7,0,0,0xa},{"netapi32.dll","NetShareGetInfo",4,0,0,4},{"netapi32.dll","NetSessionEnum",9,0,0,0x28},{"netapi32.dll","NetFileEnum",9,0,0,0x28},
        {"netapi32.dll","NetUseEnum",7,0,0,0xa},{"netapi32.dll","NetUseGetInfo",4,0,0,4},{"netapi32.dll","NetUseDel",3,0,0,4},
        {"netapi32.dll","NetpGetComputerName",1},{"netapi32.dll","DavGetHTTPFromUNCPath",3},{"netapi32.dll","DavGetUNCFromHTTPPath",3}};
    return table;
}

std::optional<std::uint32_t> WindowsServices::invoke_netapi(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const unsigned unit=x86_?4U:8U;const auto width=static_cast<std::uint8_t>(unit*8);
    const auto finish=[&](std::uint64_t value)->std::optional<std::uint32_t>{arg.return_to(state,value);return std::nullopt;};
    const auto u32=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    const auto wide_text=[&](std::uint64_t pointer){return pointer?read_utf16(memory,GuestAddress{pointer},4096):std::u16string{};};
    // 本機名稱：NULL、空字串、"."、localhost 或電腦名稱，可帶 \\ 前綴。
    const auto local_server=[&](std::uint64_t pointer){auto server=folded(wide_text(pointer));if(server.starts_with(u"\\\\"))server.erase(0,2);return server.empty() || server==u"." || server==u"localhost" || server==u"x64arm";};
    initialize_tokens();const auto user_sid=runtime_.token_objects.at(1).user.sid;
    const auto user_rid=[&]{std::uint32_t rid=1001;if(user_sid.size()>=12)rid=static_cast<std::uint32_t>(user_sid[user_sid.size()-4]|user_sid[user_sid.size()-3]<<8|user_sid[user_sid.size()-2]<<16|user_sid[user_sid.size()-1]<<24);return rid;};
    // 依欄位型別（DWORD 4 位元組、指標依架構）排列結構，字串與 SID 附在全部結構之後，整塊以 NetApiBufferFree 釋放。
    const auto field_size=[&](const Field& field){return std::holds_alternative<std::uint32_t>(field)?4U:unit;};
    const auto record_size=[&](const Record& record){std::uint64_t size=0;for(const auto& field:record){const auto s=field_size(field);size=(size+s-1)/s*s+s;}return (size+unit-1)/unit*unit;};
    const auto build=[&](const std::vector<Record>& records)->std::uint64_t{
        const auto size_each=records.empty()?unit:record_size(records.front());std::vector<std::uint8_t> block(records.size()*size_each);std::vector<std::pair<std::size_t,std::size_t>> pointers;
        const auto put=[&](std::size_t at,std::uint64_t value,unsigned bytes){for(unsigned i=0;i<bytes;++i)block[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
        for(std::size_t r=0;r<records.size();++r){std::size_t offset=0;for(const auto& field:records[r]){const auto s=field_size(field);offset=(offset+s-1)/s*s;const auto at=r*size_each+offset;
            if(const auto* number=std::get_if<std::uint32_t>(&field))put(at,*number,4);
            else if(const auto* text=std::get_if<std::u16string>(&field)){const auto bytes=utf16_bytes(*text);block.resize((block.size()+1)/2*2);pointers.emplace_back(at,block.size());block.insert(block.end(),bytes.begin(),bytes.end());}
            else if(const auto* sid=std::get_if<std::vector<std::uint8_t>>(&field)){block.resize((block.size()+3)/4*4);pointers.emplace_back(at,block.size());block.insert(block.end(),sid->begin(),sid->end());}
            offset+=s;}}
        if(block.empty())block.resize(unit);const auto address=allocate_heap(memory,block.size());if(!address)throw std::bad_alloc();
        for(const auto& [at,offset]:pointers)put(at,address+offset,unit);memory.write(GuestAddress{address},block);return address;
    };
    // 列舉：(bufptr, prefmaxlen, entriesread, totalentries, resume)；prefmaxlen 不足時至少回傳一筆並以 ERROR_MORE_DATA 更新 resume。
    const auto enumerate=[&](const std::vector<Record>& records,std::uint64_t buffer_at,std::uint32_t preferred,std::uint64_t read_at,std::uint64_t total_at,std::uint64_t resume_at)->std::optional<std::uint32_t>{
        if(!buffer_at || !read_at || !total_at)return finish(invalid_parameter);
        memory.check(GuestAddress{buffer_at},unit,Access::Write);memory.store(GuestAddress{buffer_at},width,0);
        std::size_t start=resume_at?static_cast<std::size_t>(memory.load(GuestAddress{resume_at},32)):0;start=std::min(start,records.size());
        std::size_t count=records.size()-start;
        if(preferred!=UINT32_MAX && count){const auto each=record_size(records[start])+64;count=std::clamp<std::size_t>(preferred/each,1,count);}
        memory.store(GuestAddress{read_at},32,count);memory.store(GuestAddress{total_at},32,records.size()-start);
        if(count){memory.store(GuestAddress{buffer_at},width,build({records.begin()+static_cast<std::ptrdiff_t>(start),records.begin()+static_cast<std::ptrdiff_t>(start+count)}));}
        const bool more=start+count<records.size();if(resume_at)memory.store(GuestAddress{resume_at},32,more?start+count:0);
        return finish(more?more_data:nerr_success);
    };
    const auto single=[&](const Record& record,std::uint64_t buffer_at)->std::optional<std::uint32_t>{
        if(!buffer_at)return finish(invalid_parameter);memory.check(GuestAddress{buffer_at},unit,Access::Write);memory.store(GuestAddress{buffer_at},width,build({record}));return finish(nerr_success);
    };
    const auto user_record=[&](std::uint32_t level)->std::optional<Record>{
        switch(level){
        case 0:return Record{u"user"};
        case 1:return Record{u"user",nullptr,0U,1U,u"",u"",0x200U,u""};
        case 10:return Record{u"user",u"",u"",u""};
        case 20:return Record{u"user",u"",u"",0x200U,user_rid()};
        default:return std::nullopt;
        }
    };
    const auto group_name=[](std::string_view value){return utf8_to_utf16(std::string(value));};
    const auto find_group=[&](std::u16string value)->const LocalGroup*{value=folded(value);if(value.starts_with(u"builtin\\"))value.erase(0,8);for(const auto& group:local_groups)if(folded(group_name(group.name))==value)return &group;return nullptr;};
    const auto sid_of=[](std::string_view account){return security::account_sid(*security::find_account(account));};
    try{
        if(name=="NetApiBufferAllocate" || name=="NetapipBufferAllocate"){
            if(!arg(1))return finish(invalid_parameter);memory.check(GuestAddress{arg(1)},unit,Access::Write);const auto address=allocate_heap(memory,std::max<std::uint64_t>(u32(0),1));
            if(!address)return finish(8);memory.store(GuestAddress{arg(1)},width,address);return finish(nerr_success);
        }
        if(name=="NetApiBufferReallocate"){
            // (OldBuffer, NewByteCount, NewBuffer)：OldBuffer 為 NULL 時配置；NewByteCount 為 0 時釋放並回傳 NULL。
            if(!arg(2))return finish(invalid_parameter);memory.check(GuestAddress{arg(2)},unit,Access::Write);
            if(!u32(1)){if(arg(0))(void)free_heap(memory,arg(0),state);memory.store(GuestAddress{arg(2)},width,0);return finish(nerr_success);}
            const auto address=arg(0)?reallocate_heap(memory,arg(0),u32(1),state):allocate_heap(memory,u32(1));if(!address)return finish(8);
            memory.store(GuestAddress{arg(2)},width,address);return finish(nerr_success);
        }
        if(name=="NetApiBufferSize"){
            if(!arg(0) || !arg(1))return finish(invalid_parameter);memory.check(GuestAddress{arg(1)},4,Access::Write);// 宿主管理的區塊記在 allocations_；客體 heap 區塊由 chunk 標頭取得大小。
            std::uint64_t size=0;if(const auto found=allocations_.find(arg(0));found!=allocations_.end())size=found->second;else if(guest_heap_contains(arg(0)))size=guest_heap_block_size(memory,arg(0));
            if(size==UINT64_MAX || size==0)return finish(invalid_parameter);memory.store(GuestAddress{arg(1)},32,size);return finish(nerr_success);
        }
        if(name=="NetpGetComputerName"){
            // 輸出為字串本身（以 NetApiBufferFree 釋放）。
            if(!arg(0))return finish(invalid_parameter);memory.check(GuestAddress{arg(0)},unit,Access::Write);const auto bytes=utf16_bytes(u"X64ARM");const auto address=allocate_heap(memory,bytes.size());
            if(!address)return finish(8);memory.write(GuestAddress{address},bytes);memory.store(GuestAddress{arg(0)},width,address);return finish(nerr_success);
        }
        if(name=="DavGetHTTPFromUNCPath" || name=="DavGetUNCFromHTTPPath"){
            // UNC：\\server[@SSL][@port]\DavWWWRoot\path ↔ URL：http(s)://server[:port]/path。lpSize 以字元計並含結尾 NUL。
            if(!arg(0) || !arg(2))return finish(invalid_parameter);const auto input=wide_text(arg(0));std::u16string output;
            if(name=="DavGetHTTPFromUNCPath"){
                if(!input.starts_with(u"\\\\") || input.size()<3)return finish(invalid_parameter);
                auto rest=input.substr(2);const auto slash=rest.find(u'\\');auto host=rest.substr(0,slash);std::u16string path=slash==rest.npos?std::u16string{}:rest.substr(slash+1);
                bool ssl=false;std::u16string port;
                for(auto at=host.find(u'@');at!=host.npos;at=host.find(u'@')){auto part=host.substr(at+1);host.resize(at);const auto next=part.find(u'@');auto token=part.substr(0,next);
                    if(folded(token)==u"ssl")ssl=true;else if(!token.empty() && std::all_of(token.begin(),token.end(),[](char16_t c){return c>=u'0' && c<=u'9';}))port=token;else return finish(invalid_parameter);
                    if(next!=part.npos)host+=u"@"+part.substr(next+1);}
                if(host.empty())return finish(invalid_parameter);
                if(folded(path.substr(0,10))==u"davwwwroot" && (path.size()==10 || path[10]==u'\\'))path.erase(0,std::min<std::size_t>(path.size(),11));
                std::replace(path.begin(),path.end(),u'\\',u'/');output=(ssl?u"https://":u"http://")+host+(port.empty()?u"":u":"+port)+u"/"+path;
            }else{
                const auto lower=folded(input);const bool ssl=lower.starts_with(u"https://");if(!ssl && !lower.starts_with(u"http://"))return finish(invalid_parameter);
                auto rest=input.substr(ssl?8:7);const auto slash=rest.find(u'/');auto host=rest.substr(0,slash);auto path=slash==rest.npos?std::u16string{}:rest.substr(slash+1);
                std::u16string port;if(const auto colon=host.find(u':');colon!=host.npos){port=host.substr(colon+1);host.resize(colon);if(port==(ssl?u"443":u"80"))port.clear();}
                if(host.empty())return finish(invalid_parameter);while(!path.empty() && path.back()==u'/')path.pop_back();std::replace(path.begin(),path.end(),u'/',u'\\');
                output=u"\\\\"+host+(ssl?u"@SSL":u"")+(port.empty()?u"":u"@"+port)+u"\\DavWWWRoot"+(path.empty()?u"":u"\\"+path);
            }
            const auto capacity=memory.load(GuestAddress{arg(2)},32);const auto required=output.size()+1;memory.check(GuestAddress{arg(2)},4,Access::Write);memory.store(GuestAddress{arg(2)},32,required);
            if(!arg(1) || capacity<required)return finish(insufficient_buffer);memory.write(GuestAddress{arg(1)},utf16_bytes(output));return finish(nerr_success);
        }
        // 以下皆以第一個引數為伺服器名稱（NetWkstaUserGetInfo 為保留參數）。
        const bool reserved_first=name=="NetWkstaUserGetInfo";
        if(!reserved_first && !name.starts_with("DsGetSiteName") && !name.starts_with("DsGetDcName") && !local_server(arg(0)))return finish(bad_netpath);
        if(name.starts_with("DsGetDcName") || name.starts_with("DsGetSiteName")){
            // 工作群組電腦沒有網域與站台。
            const bool site=name.starts_with("DsGetSiteName");const auto out=arg(site?1:5);if(!out)return finish(invalid_parameter);memory.check(GuestAddress{out},unit,Access::Write);memory.store(GuestAddress{out},width,0);
            return finish(site?no_sitename:no_such_domain);
        }
        if(name=="NetGetDCName" || name=="NetGetAnyDCName"){if(!arg(2))return finish(invalid_parameter);memory.store(GuestAddress{arg(2)},width,0);return finish(name=="NetGetDCName"?dc_not_found:no_such_domain);}
        if(name=="NetWkstaGetInfo"){
            const auto level=u32(1);Record record{500U,u"X64ARM",u"WORKGROUP",static_cast<std::uint32_t>(windows_version_major),static_cast<std::uint32_t>(windows_version_minor)};
            if(level==101 || level==102)record.push_back(u"C:\\Windows");if(level==102)record.push_back(1U);
            if(level!=100 && level!=101 && level!=102)return finish(level==502?access_denied:invalid_level);return single(record,arg(2));
        }
        if(name=="NetServerGetInfo"){
            // 102 以上的層級需要 Administrators 或 Server Operators。
            const auto level=u32(1);if(level==100)return single({500U,u"X64ARM"},arg(2));
            if(level==101)return single({500U,u"X64ARM",static_cast<std::uint32_t>(windows_version_major),static_cast<std::uint32_t>(windows_version_minor),0x1003U,u""},arg(2));
            return finish(level==102 || level==402 || level==403 || level==502 || level==503?access_denied:invalid_level);
        }
        if(name=="NetWkstaUserGetInfo" || name=="NetWkstaUserEnum"){
            const bool enumerate_users=name=="NetWkstaUserEnum";const auto level=u32(1);std::optional<Record> record;
            if(level==0)record=Record{u"user"};else if(level==1)record=Record{u"user",u"X64ARM",u"",u"X64ARM"};else if(level==1101 && !enumerate_users)record=Record{u""};
            if(!record)return finish(invalid_level);
            if(!enumerate_users){if(arg(0))return finish(invalid_parameter);return single(*record,arg(2));}
            return enumerate(std::vector<Record>{*record},arg(2),u32(3),arg(4),arg(5),arg(6));
        }
        if(name=="NetWkstaTransportEnum"){if(u32(1)!=0)return finish(invalid_level);return enumerate({},arg(2),u32(3),arg(4),arg(5),arg(6));} // 沒有 NetBIOS 傳輸。
        if(name=="NetUserEnum"){
            // (servername, level, filter, bufptr, prefmaxlen, entriesread, totalentries, resume)：只有一般帳號 user。
            const auto record=user_record(u32(1));if(!record)return finish(u32(1)==2 || u32(1)==3 || u32(1)==11 || u32(1)==23?50U:invalid_level);
            const auto filter=u32(2);std::vector<Record> records;if(!filter || (filter&2U))records.push_back(*record);
            return enumerate(records,arg(3),u32(4),arg(5),arg(6),arg(7));
        }
        if(name=="NetUserGetLocalGroups" || name=="NetUserGetGroups"){
            // 本機群組：未提升的一般帳號屬於 Users；全域群組在工作群組電腦為 None。
            const bool local=name=="NetUserGetLocalGroups";if(!arg(1))return finish(invalid_parameter);const auto user=folded(wide_text(arg(1)));
            if(user!=u"user" && user!=u"x64arm\\user")return finish(user_not_found);const auto level=u32(2);
            if(local){if(level!=0)return finish(invalid_level);if(u32(3)&~1U)return finish(invalid_parameter);return enumerate(std::vector<Record>{Record{u"Users"}},arg(4),u32(5),arg(6),arg(7),0);}
            if(level>1)return finish(invalid_level);return enumerate(std::vector<Record>{level==0?Record{u"None"}:Record{u"None",7U}},arg(3),u32(4),arg(5),arg(6),0);
        }
        if(name=="NetLocalGroupEnum" || name=="NetLocalGroupGetInfo"){
            const bool get=name=="NetLocalGroupGetInfo";const auto level=u32(get?2:1);
            const auto make=[&](const LocalGroup& group)->std::optional<Record>{if(level==0)return Record{group_name(group.name)};if(level==1)return Record{group_name(group.name),std::u16string(group.comment)};if(level==1002 && get)return Record{std::u16string(group.comment)};return std::nullopt;};
            if(get){if(!arg(1))return finish(invalid_parameter);const auto* group=find_group(wide_text(arg(1)));if(!group)return finish(local_group_not_found);const auto record=make(*group);if(!record)return finish(invalid_level);return single(*record,arg(3));}
            std::vector<Record> records;for(const auto& group:local_groups){const auto record=make(group);if(!record)return finish(invalid_level);records.push_back(*record);}
            return enumerate(records,arg(2),u32(3),arg(4),arg(5),arg(6));
        }
        if(name=="NetLocalGroupGetMembers"){
            // Users 的成員為 user 與 Authenticated Users；Administrators 與 Guests 在虛擬系統中沒有帳號成員。
            if(!arg(1))return finish(invalid_parameter);const auto* group=find_group(wide_text(arg(1)));if(!group)return finish(local_group_not_found);const auto level=u32(2);if(level>3)return finish(invalid_level);
            struct Member {std::vector<std::uint8_t> sid;std::uint32_t use;std::u16string name,domain;};std::vector<Member> members;
            if(group->name=="Users"){members.push_back({user_sid,1,u"user",u"X64ARM"});members.push_back({sid_of("Authenticated Users"),5,u"Authenticated Users",u"NT AUTHORITY"});}
            std::vector<Record> records;for(const auto& member:members){
                if(level==0)records.push_back({member.sid});else if(level==1)records.push_back({member.sid,member.use,member.name});
                else if(level==2)records.push_back({member.sid,member.use,member.domain+u"\\"+member.name});else records.push_back({member.domain+u"\\"+member.name});}
            return enumerate(records,arg(3),u32(4),arg(5),arg(6),arg(7));
        }
        if(name=="NetGroupEnum" || name=="NetGroupGetInfo"){
            // 工作群組電腦唯一的全域群組 None（RID 513，SE_GROUP_MANDATORY|ENABLED_BY_DEFAULT|ENABLED）。
            const bool get=name=="NetGroupGetInfo";const auto level=u32(get?2:1);std::vector<std::uint8_t> sid=user_sid;
            if(sid.size()>=12){sid.resize(sid.size()-4);for(unsigned shift=0;shift<32;shift+=8)sid.push_back(static_cast<std::uint8_t>(513U>>shift));}
            std::optional<Record> record;if(level==0)record=Record{u"None"};else if(level==1)record=Record{u"None",u"Ordinary users"};else if(level==2)record=Record{u"None",u"Ordinary users",513U,7U};else if(level==3)record=Record{u"None",u"Ordinary users",sid,7U};
            if(!record)return finish(invalid_level);
            if(get){if(!arg(1))return finish(invalid_parameter);if(folded(wide_text(arg(1)))!=u"none")return finish(group_not_found);return single(*record,arg(3));}
            return enumerate(std::vector<Record>{*record},arg(2),u32(3),arg(4),arg(5),arg(6));
        }
        if(name=="NetQueryDisplayInformation"){
            // (ServerName, Level, Index, EntriesRequested, PreferredMaximumLength, ReturnedEntryCount, SortedBuffer)：1 使用者、2 電腦帳號、3 群組；next_index 為下一筆的索引。
            const auto level=u32(1),index=u32(2);if(level<1 || level>3)return finish(invalid_level);if(!arg(5) || !arg(6))return finish(invalid_parameter);
            std::vector<Record> records;
            if(level==1)records.push_back({u"user",u"",0x200U,u"",user_rid(),1U});
            else if(level==3)records.push_back({u"None",u"Ordinary users",513U,7U,1U});
            memory.store(GuestAddress{arg(6)},width,0);const auto start=std::min<std::size_t>(index,records.size());auto count=std::min<std::size_t>(records.size()-start,std::max<std::uint32_t>(u32(3),1));
            memory.store(GuestAddress{arg(5)},32,count);if(count)memory.store(GuestAddress{arg(6)},width,build({records.begin()+static_cast<std::ptrdiff_t>(start),records.begin()+static_cast<std::ptrdiff_t>(start+count)}));
            return finish(start+count<records.size()?more_data:nerr_success);
        }
        if(name=="NetRemoteTOD"){
            // TIME_OF_DAY_INFO（UTC，與客體時區一致）：elapsedt、msecs（開機毫秒）、時分秒與百分秒、timezone、tinterval（0.0001 秒）、日月年與星期。
            if(!arg(1))return finish(invalid_parameter);const auto filetime=runtime_.system_filetime();const auto unix_seconds=filetime/10000000ULL-11644473600ULL;
            const auto days=static_cast<std::int64_t>(unix_seconds/86400);const auto seconds=unix_seconds%86400;
            // 自 1970-01-01 起的日數轉換為公曆日期。
            auto z=days+719468;const auto era=(z>=0?z:z-146096)/146097;const auto doe=static_cast<std::uint64_t>(z-era*146097);const auto yoe=(doe-doe/1460+doe/36524-doe/146096)/365;
            const auto doy=doe-(365*yoe+yoe/4-yoe/100);const auto mp=(5*doy+2)/153;const auto day=doy-(153*mp+2)/5+1;const auto month=mp<10?mp+3:mp-9;const auto year=static_cast<std::int64_t>(yoe)+era*400+(month<=2?1:0);
            const std::array<std::uint32_t,12> fields{static_cast<std::uint32_t>(unix_seconds),static_cast<std::uint32_t>(runtime_.clock_ticks/10000),static_cast<std::uint32_t>(seconds/3600),static_cast<std::uint32_t>(seconds%3600/60),static_cast<std::uint32_t>(seconds%60),
                static_cast<std::uint32_t>(filetime/100000%100),0U,310U,static_cast<std::uint32_t>(day),static_cast<std::uint32_t>(month),static_cast<std::uint32_t>(year),static_cast<std::uint32_t>((days%7+11)%7)};
            const auto address=allocate_heap(memory,48);if(!address)return finish(8);for(unsigned i=0;i<12;++i)memory.store(GuestAddress{address+i*4},32,fields[i]);
            memory.check(GuestAddress{arg(1)},unit,Access::Write);memory.store(GuestAddress{arg(1)},width,address);return finish(nerr_success);
        }
        if(name=="NetServerEnum" || name=="NetServerEnumEx"){
            // 電腦瀏覽器服務不存在，與目前 Windows 相同回 ERROR_NO_BROWSER_SERVERS_FOUND。
            if(!arg(2) || !arg(4) || !arg(5))return finish(invalid_parameter);memory.store(GuestAddress{arg(2)},width,0);memory.store(GuestAddress{arg(4)},32,0);memory.store(GuestAddress{arg(5)},32,0);
            return finish(u32(1)==100 || u32(1)==101?no_browser_servers:invalid_level);
        }
        if(name=="NetShareEnum" || name=="NetShareGetInfo" || name=="NetSessionEnum" || name=="NetFileEnum"){
            // 虛擬系統沒有執行 Server（LanmanServer）服務。
            const auto out=name=="NetShareEnum"?arg(2):name=="NetShareGetInfo"?arg(3):arg(4);if(!out)return finish(invalid_parameter);memory.store(GuestAddress{out},width,0);return finish(server_not_started);
        }
        if(name=="NetUseEnum"){if(u32(1)>2)return finish(invalid_level);return enumerate({},arg(2),u32(3),arg(4),arg(5),arg(6));} // 沒有連線中的網路磁碟或資源。
        if(name=="NetUseGetInfo"){if(!arg(1) || !arg(3))return finish(invalid_parameter);memory.store(GuestAddress{arg(3)},width,0);return finish(use_not_found);}
        if(name=="NetUseDel"){if(!arg(1))return finish(invalid_parameter);if(u32(2)>2)return finish(invalid_parameter);return finish(use_not_found);}
        throw HostCallFault("尚未實作 netapi32 服務："+std::string(name));
    }catch(const MemoryFault&){return finish(invalid_parameter);}catch(const std::bad_alloc&){return finish(8);}
}
}

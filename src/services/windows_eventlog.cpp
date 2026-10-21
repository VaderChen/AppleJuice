#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/registry_store.hpp"
#include <algorithm>
#include <openssl/sha.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <ctime>
namespace x64arm {
namespace {
struct EventError {std::uint32_t code;};
struct File {int fd{-1};~File(){if(fd>=0)::close(fd);}};
std::uint32_t io_error(){return errno==EACCES || errno==EPERM || errno==ELOOP?5U:errno==ENOSPC?112U:29U;}
std::u16string lower(std::u16string value){for(auto& c:value)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return value;}
void put(std::vector<std::uint8_t>& bytes,std::size_t at,std::uint32_t value,unsigned count=4){for(unsigned i=0;i<count;++i)bytes.at(at+i)=static_cast<std::uint8_t>(value>>(i*8));}
std::uint32_t integer(std::span<const std::uint8_t> bytes){std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t{bytes[i]}<<(i*8);return value;}
std::string record_filename(std::u16string_view log){
    const auto bytes=utf16_bytes(log);std::array<unsigned char,SHA256_DIGEST_LENGTH> digest{};SHA256(bytes.data(),bytes.size(),digest.data());
    std::string name="eventlog-";constexpr char hex[]="0123456789abcdef";for(auto byte:digest){name+=hex[byte>>4];name+=hex[byte&15];}return name+".records";
}
std::vector<EventRecord> read_records(const std::filesystem::path& path,const std::u16string& channel){
    File file{::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW)};if(file.fd<0){if(errno==ENOENT)return {};throw EventError{io_error()};}
    while(::flock(file.fd,LOCK_SH)<0)if(errno!=EINTR)throw EventError{io_error()};
    struct stat status{};if(::fstat(file.fd,&status) || !S_ISREG(status.st_mode) || status.st_uid!=::geteuid() || status.st_size<0 || status.st_size>32*1024*1024)throw EventError{1500};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(status.st_size));std::size_t done=0;
    while(done<bytes.size()){const auto n=::read(file.fd,bytes.data()+done,bytes.size()-done);if(n<0 && errno==EINTR)continue;if(n<=0)throw EventError{1500};done+=static_cast<std::size_t>(n);}
    std::vector<EventRecord> records;std::size_t offset=0;while(offset<bytes.size()){
        if(bytes.size()-offset<60)throw EventError{1500};const auto size=integer(std::span(bytes).subspan(offset));
        if(size<60 || size>0x3ffff || (size&3) || size>bytes.size()-offset || integer(std::span(bytes).subspan(offset+4))!=0x654c664c || integer(std::span(bytes).subspan(offset+size-4))!=size)throw EventError{1500};
        records.push_back({{bytes.begin()+static_cast<std::ptrdiff_t>(offset),bytes.begin()+static_cast<std::ptrdiff_t>(offset+size)},channel,0});offset+=size;
    }return records;
}
std::vector<std::uint8_t> ansi_record(const EventRecord& input) {
    const auto& source=input.bytes;
    const auto get=[&](unsigned at,unsigned size=4){if(at>source.size()-4 || size>source.size()-4-at)throw EventError{1500};std::uint32_t value=0;for(unsigned i=0;i<size;++i)value|=std::uint32_t{source[at+i]}<<(i*8);return value;};
    const auto text=[&](std::size_t& at){std::u16string value;while(at+2<=source.size()-4){const auto ch=get(static_cast<unsigned>(at),2);at+=2;if(!ch)return utf16_to_utf8(value);value+=static_cast<char16_t>(ch);}throw EventError{1500};};
    std::vector<std::uint8_t> output(source.begin(),source.begin()+56);
    const auto add=[&](std::size_t& at){const auto value=text(at);output.insert(output.end(),value.begin(),value.end());output.push_back(0);};
    const auto align=[&](){output.resize((output.size()+3)&~std::size_t{3});};
    const auto copy=[&](unsigned length_at,unsigned offset_at){const auto count=get(length_at),at=get(offset_at);put(output,offset_at,count?static_cast<std::uint32_t>(output.size()):0);if(count){if(at>source.size()-4 || count>source.size()-4-at)throw EventError{1500};output.insert(output.end(),source.begin()+at,source.begin()+at+count);}};
    std::size_t at=56;add(at);add(at);align();copy(40,44);put(output,36,static_cast<std::uint32_t>(output.size()));at=get(36);
    for(unsigned i=0;i<get(26,2);++i)add(at);
    align();copy(48,52);align();output.resize(output.size()+4);put(output,0,static_cast<std::uint32_t>(output.size()));put(output,output.size()-4,static_cast<std::uint32_t>(output.size()));return output;
}
struct EventValue {std::uint32_t type{};std::uint64_t number{};std::vector<std::uint8_t> bytes;};
EventValue string_value(std::u16string_view value){return {1,0,utf16_bytes(value)};}
struct DecodedEvent {std::array<EventValue,18> system;std::vector<EventValue> user;std::string xml;};
template<class T,void(*release)(T*)>using Xml=std::unique_ptr<T,decltype(release)>;
struct XmlTextDeleter {void operator()(xmlChar* value)const{xmlFree(value);}};
using XmlText=std::unique_ptr<xmlChar,XmlTextDeleter>;
DecodedEvent decode_event(const EventRecord& record){
    const auto& bytes=record.bytes;const auto get=[&](std::size_t offset,unsigned size=4){if(offset+size>bytes.size()-4)throw EventError{1500};std::uint32_t value=0;for(unsigned i=0;i<size;++i)value|=std::uint32_t{bytes[offset+i]}<<(8*i);return value;};
    const auto read_text=[&](std::size_t& position){std::u16string value;while(position+2<=bytes.size()-4){const auto ch=get(position,2);position+=2;if(!ch)return value;value+=static_cast<char16_t>(ch);}throw EventError{1500};};
    DecodedEvent event;auto& s=event.system;std::size_t position=56;s[0]=string_value(read_text(position));s[15]=string_value(read_text(position));s[14]=string_value(record.channel);
    const auto id=get(20),type=get(24,2);s[2]={6,id&65535,{}};s[3]={6,id>>16,{}};s[4]={4,type==1?2U:type==2?3U:type==4?4U:0U,{}};s[5]={6,get(28,2),{}};
    s[8]={17,UINT64_C(116444736000000000)+std::uint64_t{get(12)}*10000000,{}};s[9]={10,get(8),{}};
    const auto sid_size=get(40),sid_offset=get(44);if(sid_size){if(sid_size<8 || sid_offset>bytes.size()-4 || sid_size>bytes.size()-4-sid_offset || bytes[sid_offset]!=1 || bytes[sid_offset+1]>15 || sid_size!=8+bytes[sid_offset+1]*4U)throw EventError{1500};s[16]={19,0,{bytes.begin()+sid_offset,bytes.begin()+sid_offset+sid_size}};}
    position=get(36);for(unsigned i=0;i<get(26,2);++i)event.user.push_back(string_value(read_text(position)));
    const auto raw_size=get(48),raw_offset=get(52);if(raw_size){if(raw_offset>bytes.size()-4 || raw_size>bytes.size()-4-raw_offset)throw EventError{1500};event.user.push_back({14,raw_size,{bytes.begin()+raw_offset,bytes.begin()+raw_offset+raw_size}});}
    Xml<xmlDoc,xmlFreeDoc> doc(xmlNewDoc(BAD_CAST "1.0"),xmlFreeDoc);if(!doc)throw std::bad_alloc();auto root=xmlNewNode(nullptr,BAD_CAST "Event");if(!root)throw std::bad_alloc();xmlDocSetRootElement(doc.get(),root);
    auto system=xmlNewChild(root,nullptr,BAD_CAST "System",nullptr);auto provider=xmlNewChild(system,nullptr,BAD_CAST "Provider",nullptr);
    const auto utf8=[](const EventValue& value){std::u16string text;for(std::size_t i=0;i+2<value.bytes.size();i+=2)text+=static_cast<char16_t>(value.bytes[i]|(static_cast<unsigned>(value.bytes[i+1])<<8));return utf16_to_utf8(text);};
    xmlNewProp(provider,BAD_CAST "Name",BAD_CAST utf8(s[0]).c_str());
    const auto child=[&](const char* name,const std::string& value){auto node=xmlNewChild(system,nullptr,BAD_CAST name,nullptr);xmlAddChild(node,xmlNewText(BAD_CAST value.c_str()));return node;};
    auto event_id=child("EventID",std::to_string(s[2].number));xmlNewProp(event_id,BAD_CAST "Qualifiers",BAD_CAST std::to_string(s[3].number).c_str());
    child("Level",std::to_string(s[4].number));child("Task",std::to_string(s[5].number));child("EventRecordID",std::to_string(s[9].number));child("Channel",utf8(s[14]));child("Computer",utf8(s[15]));
    const std::time_t seconds=get(12);std::tm utc{};if(!gmtime_r(&seconds,&utc))throw EventError{1500};char stamp[40]{};std::strftime(stamp,sizeof stamp,"%Y-%m-%dT%H:%M:%S.0000000Z",&utc);auto time=xmlNewChild(system,nullptr,BAD_CAST "TimeCreated",nullptr);xmlNewProp(time,BAD_CAST "SystemTime",BAD_CAST stamp);
    if(s[16].type){const auto& sid=s[16].bytes;std::uint64_t authority=0;for(unsigned i=2;i<8;++i)authority=(authority<<8)|sid[i];std::string value="S-1-"+std::to_string(authority);for(unsigned i=0;i<sid[1];++i)value+="-"+std::to_string(integer(std::span(sid).subspan(8+i*4)));auto security=xmlNewChild(system,nullptr,BAD_CAST "Security",nullptr);xmlNewProp(security,BAD_CAST "UserID",BAD_CAST value.c_str());}
    auto data=xmlNewChild(root,nullptr,BAD_CAST "EventData",nullptr);for(const auto& value:event.user){std::string content;if(value.type==1)content=utf8(value);else{constexpr char hex[]="0123456789ABCDEF";for(auto byte:value.bytes){content+=hex[byte>>4];content+=hex[byte&15];}}auto node=xmlNewChild(data,nullptr,BAD_CAST(value.type==1?"Data":"Binary"),nullptr);xmlAddChild(node,xmlNewText(BAD_CAST content.c_str()));}
    Xml<xmlBuffer,xmlBufferFree> buffer(xmlBufferCreate(),xmlBufferFree);if(!buffer || xmlNodeDump(buffer.get(),doc.get(),root,0,0)<0)throw std::bad_alloc();event.xml=reinterpret_cast<const char*>(xmlBufferContent(buffer.get()));return event;
}
Xml<xmlDoc,xmlFreeDoc> event_document(const std::string& xml){auto doc=xmlReadMemory(xml.data(),static_cast<int>(xml.size()),nullptr,nullptr,XML_PARSE_NONET|XML_PARSE_NOERROR|XML_PARSE_NOWARNING);if(!doc)throw EventError{1500};return {doc,xmlFreeDoc};}
Xml<xmlXPathObject,xmlXPathFreeObject> evaluate(xmlDoc* doc,const std::string& expression){
    Xml<xmlXPathContext,xmlXPathFreeContext> context(xmlXPathNewContext(doc),xmlXPathFreeContext);if(!context)throw std::bad_alloc();context->node=reinterpret_cast<xmlNode*>(doc);context->error=[](void*,xmlError*){};
    auto value=xmlXPathEvalExpression(BAD_CAST expression.c_str(),context.get());if(!value)throw EventError{15001};return {value,xmlXPathFreeObject};
}
constexpr const char* system_paths[]{"Event/System/Provider/@Name","Event/System/Provider/@Guid","Event/System/EventID","Event/System/EventID/@Qualifiers","Event/System/Level","Event/System/Task","Event/System/Opcode","Event/System/Keywords","Event/System/TimeCreated/@SystemTime","Event/System/EventRecordID","Event/System/Correlation/@ActivityID","Event/System/Correlation/@RelatedActivityID","Event/System/Execution/@ProcessID","Event/System/Execution/@ThreadID","Event/System/Channel","Event/System/Computer","Event/System/Security/@UserID","Event/System/Version"};
EventValue selected_value(xmlDoc* doc,const std::string& path,const DecodedEvent& decoded){
    auto selected=evaluate(doc,path);if(selected->type!=XPATH_NODESET)throw EventError{15001};if(!selected->nodesetval || !selected->nodesetval->nodeNr)return {};
    const auto node=selected->nodesetval->nodeTab[0];for(unsigned i=0;i<18;++i){auto canonical=evaluate(doc,system_paths[i]);if(canonical->nodesetval && canonical->nodesetval->nodeNr && canonical->nodesetval->nodeTab[0]==node)return decoded.system[i];}
    auto all=evaluate(doc,"Event/EventData/*");for(int i=0;all->nodesetval && i<all->nodesetval->nodeNr;++i)if(all->nodesetval->nodeTab[i]==node)return decoded.user.at(static_cast<unsigned>(i));
    XmlText value(xmlNodeGetContent(node));return string_value(utf8_to_utf16(value?reinterpret_cast<const char*>(value.get()):""));
}
void append(const std::filesystem::path& path,std::vector<std::uint8_t>& record){
    File file{::open(path.c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600)};if(file.fd<0)throw EventError{io_error()};
    while(::flock(file.fd,LOCK_EX)<0)if(errno!=EINTR)throw EventError{io_error()};
    struct stat status{};if(::fstat(file.fd,&status) || !S_ISREG(status.st_mode) || status.st_uid!=::geteuid() || status.st_size<0)throw EventError{5};
    if(static_cast<std::uint64_t>(status.st_size)+record.size()>32*1024*1024)throw EventError{1502};std::uint32_t number=1;
    if(status.st_size){std::array<std::uint8_t,4> tail{};if(status.st_size<60 || ::pread(file.fd,tail.data(),4,status.st_size-4)!=4)throw EventError{1500};const auto length=integer(tail);
        if(length<60 || length>0x3ffff || length>static_cast<std::uint64_t>(status.st_size))throw EventError{1500};std::array<std::uint8_t,12> head{};
        if(::pread(file.fd,head.data(),head.size(),status.st_size-length)!=static_cast<ssize_t>(head.size()) || integer(head)!=length || integer(std::span(head).subspan(4))!=0x654c664c)throw EventError{1500};
        number=integer(std::span(head).subspan(8));if(number==UINT32_MAX)throw EventError{1502};++number;}
    put(record,8,number);std::size_t written=0;
    while(written<record.size()){const auto count=::pwrite(file.fd,record.data()+written,record.size()-written,status.st_size+static_cast<off_t>(written));if(count<0 && errno==EINTR)continue;if(count<=0){const auto error=io_error();(void)::ftruncate(file.fd,status.st_size);throw EventError{error};}written+=static_cast<std::size_t>(count);}
    if(::fsync(file.fd)){const auto error=io_error();(void)::ftruncate(file.fd,status.st_size);throw EventError{error};}
}
}
std::uint32_t WindowsServices::data_protection_audit(std::u16string_view description,std::span<const std::uint8_t> key,std::uint32_t flags,bool protect,std::uint32_t status){
    try {
        if(!runtime_.network.external_io)return 50;initialize_tokens();std::vector<std::uint8_t> record(56);put(record,4,0x654c664c);const auto ticks=runtime_.system_filetime();
        const auto seconds=static_cast<std::uint32_t>(ticks<UINT64_C(116444736000000000)?0:(ticks-UINT64_C(116444736000000000))/10000000);
        put(record,12,seconds);put(record,16,seconds);put(record,20,protect?4694U:4695U);put(record,24,status?16:8,2);put(record,26,5,2);
        const auto add=[&](std::u16string_view value){const auto bytes=utf16_bytes(std::u16string(value));record.insert(record.end(),bytes.begin(),bytes.end());};
        add(u"MacWin DPAPI");add(u"X64ARM");record.resize((record.size()+3)&~std::size_t{3});const auto& sid=runtime_.token_objects.at(effective_token_id()).user.sid;
        put(record,40,static_cast<std::uint32_t>(sid.size()));put(record,44,static_cast<std::uint32_t>(record.size()));record.insert(record.end(),sid.begin(),sid.end());
        put(record,36,static_cast<std::uint32_t>(record.size()));std::u16string identifier;constexpr char16_t hex[]=u"0123456789abcdef";for(const auto byte:key){identifier+=hex[byte>>4];identifier+=hex[byte&15];}
        add(description);add(identifier);add(utf8_to_utf16(std::to_string(flags)));add(u"AES-256-GCM／HKDF-SHA256");add(utf8_to_utf16(std::to_string(status)));record.resize((record.size()+3)&~std::size_t{3});record.resize(record.size()+4);
        if(record.size()>0x3ffff)return 1783;put(record,0,static_cast<std::uint32_t>(record.size()));put(record,record.size()-4,static_cast<std::uint32_t>(record.size()));
        const auto directory=registry_file_.empty()?std::filesystem::path(mapping_session()):registry_file_.parent_path();append(directory/record_filename(u"security"),record);return 0;
    }catch(const EventError& error){return error.code;}catch(const std::bad_alloc&){return 8;}
}
std::span<const WindowsServiceDescriptor> eventlog_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"wevtapi.dll","EvtQuery",4},{"wevtapi.dll","EvtNext",6},{"wevtapi.dll","EvtClose",1},
        {"wevtapi.dll","EvtCreateRenderContext",3},{"wevtapi.dll","EvtRender",7},
        {"advapi32.dll","OpenEventLogA",2},{"advapi32.dll","OpenEventLogW",2},
        {"advapi32.dll","ReadEventLogA",7},{"advapi32.dll","ReadEventLogW",7},{"advapi32.dll","CloseEventLog",1},
        {"advapi32.dll","GetNumberOfEventLogRecords",2},{"advapi32.dll","GetOldestEventLogRecord",2},
        {"advapi32.dll","RegisterEventSourceA",2},{"advapi32.dll","RegisterEventSourceW",2},
        {"advapi32.dll","ReportEventA",9},{"advapi32.dll","ReportEventW",9},{"advapi32.dll","DeregisterEventSource",1}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_eventlog(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;const bool wide=symbol.ends_with('W');std::uint64_t result=0;
    const auto text=[&](std::uint64_t address,std::size_t maximum){if(!address)throw EventError{87};return wide?read_utf16(memory,GuestAddress{address},maximum):utf8_to_utf16(read_ansi(memory,address,maximum));};
    try {
        if(service.family=="wevtapi.dll"){
            auto& logs=runtime_.eventlog;const auto unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
            const auto string=[&](std::uint64_t pointer){if(!pointer)throw EventError{87};return read_utf16(memory,GuestAddress{pointer},16384);};
            const auto new_handle=[&](){if(logs.queries.size()+logs.events.size()+logs.contexts.size()>=4096)throw EventError{8};return logs.next++;};
            if(symbol=="EvtQuery"){
                if(arg(0))throw EventError{50};const auto flags=static_cast<unsigned>(arg(3));
                if((flags&~0x301U) || !(flags&1) || (flags&0x300)==0x300)throw EventError{50};
                const auto channel=lower(string(arg(1)));if(channel.empty() || channel.find_first_of(u"\\/:")!=channel.npos)throw EventError{15007};
                const auto query=arg(2)?utf16_to_utf8(string(arg(2))):"*";if(query.empty())throw EventError{15001};if(query.front()=='<')throw EventError{50};
                auto empty=event_document("<Event><System/></Event>");auto validation=evaluate(empty.get(),query);if(validation->type!=XPATH_NODESET)throw EventError{15001};
                if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許事件查詢 I/O");
                if(channel!=u"application" && channel!=u"system" && channel!=u"security"){
                    RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)throw EventError{transaction.error};
                    const auto prefix=std::u16string(1,u'\2')+u"\\system\\currentcontrolset\\services\\eventlog\\"+channel;
                    bool exists=false;for(const auto& [key,values]:runtime_.registry.keys){(void)values;if(key==prefix || key.starts_with(prefix+u"\\")){exists=true;break;}}if(!exists)throw EventError{15007};
                }
                const auto directory=registry_file_.empty()?std::filesystem::path(mapping_session()):registry_file_.parent_path();auto records=read_records(directory/record_filename(channel),channel==u"application"?u"Application":channel==u"system"?u"System":channel==u"security"?u"Security":string(arg(1)));
                EventQuery selected;selected.thread=threads_.current;
                for(auto& record:records){const auto decoded=decode_event(record);auto doc=event_document(decoded.xml);auto matching=evaluate(doc.get(),query);if(matching->nodesetval && matching->nodesetval->nodeNr)selected.records.push_back(std::move(record));}
                if(flags&0x200)std::reverse(selected.records.begin(),selected.records.end());const auto handle=new_handle();logs.queries.emplace(handle,std::move(selected));result=handle;
            }else if(symbol=="EvtNext"){
                const auto found=logs.queries.find(arg(0));if(found==logs.queries.end())throw EventError{6};auto& query=found->second;if(query.thread!=threads_.current)throw EventError{87};
                const auto capacity=static_cast<unsigned>(arg(1));if(!capacity || capacity>1024 || !arg(2) || !arg(5) || arg(4))throw EventError{87};
                memory.check(GuestAddress{arg(5)},4,Access::Write);memory.store(GuestAddress{arg(5)},32,0);
                const auto count=std::min<std::size_t>(capacity,query.records.size()-query.position);if(!count)throw EventError{259};
                if(logs.events.size()+logs.contexts.size()+logs.queries.size()+count>4096)throw EventError{8};memory.check(GuestAddress{arg(2)},count*unit,Access::Write);
                std::vector<std::uint64_t> handles;handles.reserve(count);try{for(std::size_t i=0;i<count;++i){auto event=query.records[query.position+i];event.parent=arg(0);const auto handle=new_handle();logs.events.emplace(handle,std::move(event));handles.push_back(handle);}}
                catch(...){for(auto handle:handles)logs.events.erase(handle);throw;}
                for(std::size_t i=0;i<count;++i)memory.store(GuestAddress{arg(2)+i*unit},bits,handles[i]);query.position+=count;memory.store(GuestAddress{arg(5)},32,count);result=1;
            }else if(symbol=="EvtClose"){
                if(logs.queries.erase(arg(0))){std::erase_if(logs.events,[&](const auto& item){return item.second.parent==arg(0);});result=1;}
                else if(logs.events.erase(arg(0)) || logs.contexts.erase(arg(0)))result=1;else throw EventError{6};
            }else if(symbol=="EvtCreateRenderContext"){
                EventRenderContext context;context.flags=static_cast<unsigned>(arg(2));const auto count=static_cast<unsigned>(arg(0));if(context.flags>2 || count>128 || (context.flags && (count || arg(1))) || (count && !arg(1)))throw EventError{87};
                auto empty=event_document("<Event/>");for(unsigned i=0;i<count;++i){auto path=utf16_to_utf8(string(memory.load(GuestAddress{arg(1)+i*unit},bits)));auto checked=evaluate(empty.get(),path);if(checked->type!=XPATH_NODESET)throw EventError{15001};context.paths.push_back(std::move(path));}
                const auto handle=new_handle();logs.contexts.emplace(handle,std::move(context));result=handle;
            }else if(symbol=="EvtRender"){
                const auto found=logs.events.find(arg(1));if(found==logs.events.end())throw EventError{6};if(!arg(5) || !arg(6))throw EventError{87};
                memory.check(GuestAddress{arg(5)},4,Access::Write);memory.check(GuestAddress{arg(6)},4,Access::Write);const auto decoded=decode_event(found->second);std::vector<std::uint8_t> output;unsigned properties=0;
                if(arg(2)==1){if(arg(0))throw EventError{87};auto xml=decoded.xml;const auto at=xml.find('>');if(at==xml.npos)throw EventError{1500};xml.insert(at," xmlns=\"http://schemas.microsoft.com/win/2004/08/events/event\"");output=utf16_bytes(utf8_to_utf16(xml));}
                else if(!arg(2)){
                    const auto context=logs.contexts.find(arg(0));if(context==logs.contexts.end())throw EventError{6};std::vector<EventValue> values;
                    if(context->second.flags==1)values.assign(decoded.system.begin(),decoded.system.end());else if(context->second.flags==2)values=decoded.user;
                    else{auto doc=event_document(decoded.xml);for(const auto& path:context->second.paths)values.push_back(selected_value(doc.get(),path,decoded));}
                    properties=static_cast<unsigned>(values.size());output.resize(values.size()*16);
                    for(std::size_t i=0;i<values.size();++i){const auto& value=values[i];put(output,i*16+12,value.type);std::uint64_t data=value.number;
                        if(value.type==1 || value.type==14 || value.type==19){output.resize((output.size()+7)&~std::size_t{7});data=arg(4)+output.size();output.insert(output.end(),value.bytes.begin(),value.bytes.end());if(value.type==14)put(output,i*16+8,static_cast<unsigned>(value.bytes.size()));}
                        put(output,i*16,static_cast<unsigned>(data));put(output,i*16+4,static_cast<unsigned>(data>>32));}
                }else throw EventError{50};
                memory.store(GuestAddress{arg(5)},32,output.size());memory.store(GuestAddress{arg(6)},32,properties);
                if(arg(3)<output.size() || (!output.empty() && !arg(4)))throw EventError{122};if(!output.empty())memory.write(GuestAddress{arg(4)},output);result=1;
            }
        }else if(symbol.starts_with("OpenEventLog")) {
            const auto server=arg(0)?lower(text(arg(0),256)):std::u16string{};
            if(!server.empty() && server!=u"." && server!=u"x64arm" && server!=u"\\\\x64arm")throw EventError{53};
            auto channel=lower(text(arg(1),254));if(channel.empty() || channel.find_first_of(u"\\/:")!=channel.npos)throw EventError{87};
            if(channel==u"security")throw EventError{5}; // 目前權杖的 privilege 集合為空，沒有 SeSecurityPrivilege。
            if(channel!=u"application" && channel!=u"system" && channel!=u"security"){
                RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)throw EventError{transaction.error};
                const auto key=std::u16string(1,u'\2')+u"\\system\\currentcontrolset\\services\\eventlog\\"+channel;
                if(!runtime_.registry.keys.contains(key))channel=u"application";
            }
            if(runtime_.eventlog.readers.size()>=4096)throw EventError{8};
            if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許事件記錄檔 I/O");
            const auto directory=registry_file_.empty()?std::filesystem::path(mapping_session()):registry_file_.parent_path();
            (void)read_records(directory/record_filename(channel),channel);
            const auto handle=runtime_.eventlog.next++;runtime_.eventlog.readers.emplace(handle,LegacyEventReader{channel,0,false});result=handle;
        }else if(symbol=="CloseEventLog") {
            if(!runtime_.eventlog.readers.erase(arg(0)))throw EventError{6};result=1;
        }else if(symbol.starts_with("ReadEventLog") || symbol=="GetNumberOfEventLogRecords" || symbol=="GetOldestEventLogRecord") {
            const auto found=runtime_.eventlog.readers.find(arg(0));if(found==runtime_.eventlog.readers.end())throw EventError{6};auto& reader=found->second;
            if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許事件記錄檔 I/O");
            const auto directory=registry_file_.empty()?std::filesystem::path(mapping_session()):registry_file_.parent_path();const auto records=read_records(directory/record_filename(reader.channel),reader.channel);
            if(symbol=="GetNumberOfEventLogRecords" || symbol=="GetOldestEventLogRecord") {
                if(!arg(1))throw EventError{87};memory.store(GuestAddress{arg(1)},32,symbol=="GetNumberOfEventLogRecords"?records.size():records.empty()?0U:integer(std::span(records.front().bytes).subspan(8)));result=1;
            }else {
                const auto flags=static_cast<std::uint32_t>(arg(1)),capacity=static_cast<std::uint32_t>(arg(4));
                if(!arg(3) || !arg(5) || !arg(6) || capacity>0x7ffffU || (flags&~15U) || ((flags&3)!=1 && (flags&3)!=2) || ((flags&12)!=4 && (flags&12)!=8))throw EventError{87};
                memory.check(GuestAddress{arg(5)},4,Access::Write);memory.check(GuestAddress{arg(6)},4,Access::Write);memory.store(GuestAddress{arg(5)},32,0);memory.store(GuestAddress{arg(6)},32,0);
                const std::int64_t step=(flags&4)?1:-1;auto position=reader.position;
                if(flags&2){const auto number=static_cast<std::uint32_t>(arg(2));const auto at=std::find_if(records.begin(),records.end(),[&](const auto& record){return integer(std::span(record.bytes).subspan(8))==number;});if(at==records.end())throw EventError{38};position=at-records.begin();}
                else if(!reader.positioned)position=step>0?0:static_cast<std::int64_t>(records.size())-1;
                if(position<0 || static_cast<std::uint64_t>(position)>=records.size())throw EventError{38};
                std::vector<std::uint8_t> output;
                while(position>=0 && static_cast<std::uint64_t>(position)<records.size()){
                    const auto& record=records[static_cast<std::size_t>(position)];const auto converted=wide?std::vector<std::uint8_t>{}:ansi_record(record);const auto& bytes=wide?record.bytes:converted;
                    if(bytes.size()>capacity-output.size()){
                        if(output.empty()){memory.store(GuestAddress{arg(6)},32,bytes.size());throw EventError{122};}break;
                    }
                    output.insert(output.end(),bytes.begin(),bytes.end());position+=step;
                }
                memory.write(GuestAddress{arg(3)},output);memory.store(GuestAddress{arg(5)},32,output.size());reader.position=position;reader.positioned=true;result=1;
            }
        }else if(symbol.starts_with("RegisterEventSource")){
            const auto server=arg(0)?lower(text(arg(0),256)):std::u16string{};
            if(!server.empty() && server!=u"." && server!=u"x64arm" && server!=u"\\\\x64arm")throw EventError{53};
            const auto source=text(arg(1),254),normalized=lower(source);if(source.empty() || source.find(u'\\')!=std::u16string::npos)throw EventError{87};
            if(normalized==u"security")throw EventError{5};if(runtime_.eventlog.sources.size()>=4096)throw EventError{8};
            std::u16string log=u"application";
            if(!registry_file_.empty() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許持久登錄檔 I/O");
            RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)throw EventError{transaction.error};
            const auto prefix=std::u16string(1,u'\2')+u"\\system\\currentcontrolset\\services\\eventlog\\";
            for(const auto& [key,values]:runtime_.registry.keys){(void)values;if(!key.starts_with(prefix))continue;const auto rest=key.substr(prefix.size());const auto slash=rest.find(u'\\');if(slash!=std::u16string::npos && rest.substr(slash+1)==normalized){log=rest.substr(0,slash);break;}}
            if(log==u"security")throw EventError{5};
            const auto handle=runtime_.eventlog.next++;runtime_.eventlog.sources.emplace(handle,EventSource{source,log});result=handle;
        }else if(symbol=="DeregisterEventSource"){
            result=runtime_.eventlog.sources.erase(arg(0))?1:0;if(!result)last_error_=6;
        }else{
            const auto found=runtime_.eventlog.sources.find(arg(0));if(found==runtime_.eventlog.sources.end())throw EventError{6};
            const auto type=static_cast<std::uint16_t>(arg(1)),category=static_cast<std::uint16_t>(arg(2)),count=static_cast<std::uint16_t>(arg(5));const auto raw_size=static_cast<std::uint32_t>(arg(6));
            if(type!=0 && type!=1 && type!=2 && type!=4 && type!=8 && type!=16)throw EventError{87};if(raw_size>61440)throw EventError{1783};
            if((count && !arg(7)) || (raw_size && !arg(8)))throw EventError{87};
            std::vector<std::uint8_t> record(56);put(record,4,0x654c664c);const auto ticks=runtime_.system_filetime();
            const auto seconds=static_cast<std::uint32_t>(ticks<UINT64_C(116444736000000000)?0:(ticks-UINT64_C(116444736000000000))/10000000);
            put(record,12,seconds);put(record,16,seconds);put(record,20,static_cast<std::uint32_t>(arg(3)));put(record,24,type,2);put(record,26,count,2);put(record,28,category,2);
            const auto add_text=[&](std::u16string_view value){auto bytes=utf16_bytes(value);if(bytes.size()>0x3ffff-record.size())throw EventError{1783};record.insert(record.end(),bytes.begin(),bytes.end());};
            const auto align=[&](){record.resize((record.size()+3)&~std::size_t{3});};
            add_text(found->second.source);add_text(u"X64ARM");align();
            if(arg(4)){const auto revision=memory.load(GuestAddress{arg(4)},8),subauthorities=memory.load(GuestAddress{arg(4)+1},8);if(revision!=1 || subauthorities>15)throw EventError{1337};const auto size=8+subauthorities*4;
                put(record,40,static_cast<std::uint32_t>(size));put(record,44,static_cast<std::uint32_t>(record.size()));const auto offset=record.size();record.resize(offset+static_cast<std::size_t>(size));memory.read(GuestAddress{arg(4)},std::span(record).subspan(offset));}
            put(record,36,static_cast<std::uint32_t>(record.size()));for(unsigned i=0;i<count;++i){const auto pointer=memory.load(GuestAddress{arg(7)+i*(x86_?4U:8U)},x86_?32:64);add_text(text(pointer,31839));}
            align();if(raw_size){if(raw_size>0x3ffff-record.size())throw EventError{1783};put(record,48,raw_size);put(record,52,static_cast<std::uint32_t>(record.size()));const auto offset=record.size();record.resize(offset+raw_size);memory.read(GuestAddress{arg(8)},std::span(record).subspan(offset));}
            align();record.resize(record.size()+4);if(record.size()>0x3ffff)throw EventError{1783};put(record,0,static_cast<std::uint32_t>(record.size()));put(record,record.size()-4,static_cast<std::uint32_t>(record.size()));
            // 有持久登錄檔時使用同一沙箱儲存位置；其餘與子程序共用受保護的 session。
            if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許事件記錄檔 I/O");
            const auto directory=registry_file_.empty()?std::filesystem::path(mapping_session()):registry_file_.parent_path();
            append(directory/record_filename(found->second.log),record);result=1;
        }
    }catch(const EventError& error){last_error_=error.code;}catch(const MemoryFault&){last_error_=998;}catch(const std::bad_alloc&){last_error_=8;}
    arg.return_to(state,result);return std::nullopt;
}
}

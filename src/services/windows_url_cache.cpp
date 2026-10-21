#include "x64arm/windows_services.hpp"
#include "x64arm/url_cache.hpp"
#include "x64arm/registry_store.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <curl/curl.h>
#include <charconv>
#include <cstdlib>
#include <csignal>
#include <unistd.h>
namespace x64arm {
namespace {
using url_cache::Entry;
struct CacheError{std::uint32_t code;};
void require(std::uint32_t error){if(error)throw CacheError{error};}
struct Transaction {
    RegistryState& state;RegistryTransaction native;RegistryState previous;bool saved{};
    Transaction(RegistryState& s,const std::filesystem::path& p):state(s),native(s,p),previous(s){require(native.error);}
    ~Transaction(){if(!saved)state=std::move(previous);}
    void commit(){require(native.commit());saved=true;}
};
constexpr auto directory=u"C:\\Users\\User\\AppData\\Local\\Microsoft\\Windows\\INetCache\\X64ARM";
std::u16string create_file(ReadOnlyFiles& files,std::u16string_view extension){
    const auto folder=utf16_to_utf8(directory);for(std::size_t pos=3;;){pos=folder.find('\\',pos);const auto error=files.create_directory(pos==std::string::npos?folder:folder.substr(0,pos));require(error==183?0:error);if(pos==std::string::npos)break;++pos;}
    if(extension.size()>32 || extension.find_first_of(u"/\\:.?*<>|\"")!=std::u16string_view::npos)throw CacheError{87};
    for(unsigned retry=0;retry<32;++retry){constexpr char16_t digits[]=u"0123456789abcdef";std::u16string path=std::u16string(directory)+u"\\";for(unsigned n=0;n<2;++n){const auto random=::arc4random();for(unsigned i=0;i<8;++i)path.push_back(digits[(random>>(i*4))&15]);}if(!extension.empty())path+=u"."+std::u16string(extension);
        const auto opened=files.open_host(utf16_to_utf8(path),0xc0000000U,1,0,7);if(opened.error==80 || opened.error==183)continue;require(opened.error);return path;}
    throw CacheError{80};
}
bool own_file(std::u16string_view path){auto a=std::u16string(path),b=std::u16string(directory)+u"\\";for(auto* text:{&a,&b})for(auto& c:*text){if(c==u'/')c=u'\\';if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);}if(!a.starts_with(b))return false;const auto rest=a.substr(b.size());return !rest.empty() && rest.find(u'\\')==rest.npos && rest!=u"." && rest!=u"..";}
void prune(Entry& entry){const auto now=url_cache::now();std::erase_if(entry.leases,[&](const auto& lease){return lease.until<=now || (::kill(static_cast<pid_t>(lease.pid),0)<0 && errno==ESRCH);});}
Entry lookup(RegistryState& state,ReadOnlyFiles& files,std::u16string_view url,bool redirects,Transaction& transaction){auto entry=url_cache::get(state,url,redirects);if(!entry)throw CacheError{2};prune(*entry);if(!entry->file.empty()){if(!own_file(entry->file))throw CacheError{13};const auto info=files.information(utf16_to_utf8(entry->file));require(info.error);entry->size=info.metadata.size;}if(entry->deleting && entry->leases.empty()){if(!entry->file.empty())require(files.remove(utf16_to_utf8(entry->file),false));url_cache::erase(state,entry->url);transaction.commit();throw CacheError{2};}return *entry;}
void retire(RegistryState& state,ReadOnlyFiles& files,Entry& entry,std::uint64_t lease){std::erase_if(entry.leases,[&](const auto& item){return item.id==lease;});prune(entry);if(entry.deleting && entry.leases.empty()){if(!entry.file.empty() && !own_file(entry.file))throw CacheError{13};if(!entry.file.empty())require(files.remove(utf16_to_utf8(entry.file),false));url_cache::erase(state,entry.url);}else url_cache::put(state,entry);}
std::string lower(std::string text){for(auto& c:text)if(c>='A' && c<='Z')c=static_cast<char>(c+32);return text;}
bool private_request(const HttpHandle& request){if(!request.input.username.empty() || !request.input.password.empty())return true;for(const auto& [name,value]:request.input.headers){(void)value;const auto key=lower(name);if(key=="authorization" || key=="cookie" || key=="range")return true;}return false;}
class CachedRequest final:public NativeHttp{HttpResult result_;public:explicit CachedRequest(HttpResult result):result_(std::move(result)){}bool done()const override{return true;}HttpResult take()override{return std::move(result_);}};
}
struct UrlCacheState {
    const WindowsServices* owner{};
    struct Borrow{std::u16string url;std::uint64_t id{};std::shared_ptr<HostFile> file;bool stream{};};
    struct Enumeration{std::vector<std::u16string> urls;std::size_t cursor{};};
    std::uint64_t next{0x68100000};
    std::map<std::uint64_t,Borrow> borrows;
    std::map<std::uint64_t,Enumeration> enumerations;
};
UrlCacheState& WindowsServices::url_cache_state(){if(!url_cache_){url_cache_=std::make_shared<UrlCacheState>();url_cache_->owner=this;}else if(url_cache_->owner!=this){auto next=std::make_shared<UrlCacheState>(*url_cache_);next->owner=this;next->borrows.clear();url_cache_=std::move(next);}return *url_cache_;}
void WindowsServices::release_url_cache()noexcept{
    if(!url_cache_ || url_cache_->owner!=this)return;
    if(url_cache_->borrows.empty()){url_cache_->owner=nullptr;return;}
    try{Transaction transaction(runtime_.registry,registry_file_);for(auto& [id,borrow]:url_cache_->borrows){(void)id;borrow.file.reset();auto entry=url_cache::get(runtime_.registry,borrow.url);if(entry)retire(runtime_.registry,filesystem_,*entry,borrow.id);}transaction.commit();}catch(...){/* 索引中的有界 lease 仍可由存活檢查／逾時收回。 */}
    url_cache_->borrows.clear();url_cache_->owner=nullptr;runtime_.http.cache_active=false;
}
std::optional<std::uint32_t> WindowsServices::invoke_url_cache(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(!runtime_.network.external_io)throw HostCallFault("比較模式不執行持久 URL 快取 I/O");
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;const bool wide=symbol.ends_with('W');const unsigned width=wide?2:1;std::uint64_t result=0;
    const auto d=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};const auto text=[&](std::uint64_t pointer){if(!pointer)return std::u16string{};return wide?read_utf16(memory,GuestAddress{pointer},4096):utf8_to_utf16(read_ansi(memory,pointer,16384));};
    const auto emit=[&](const Entry& entry,std::uint64_t out,std::uint64_t size_pointer){if(!size_pointer)throw CacheError{87};memory.check(GuestAddress{size_pointer},4,Access::Read);memory.check(GuestAddress{size_pointer},4,Access::Write);const auto bytes=url_cache::information(entry,wide,x86_,out);const auto capacity=memory.load(GuestAddress{size_pointer},32);memory.store(GuestAddress{size_pointer},32,bytes.size());if(!out || capacity<bytes.size())throw CacheError{122};memory.check(GuestAddress{out},bytes.size(),Access::Write);memory.write(GuestAddress{out},bytes);};
    auto& local=url_cache_state();
    try{
        if(symbol.starts_with("CreateUrlCacheEntry")){
            if(text(arg(0)).empty())throw CacheError{87};(void)url_cache::key(text(arg(0)));if(d(4) || !arg(3))throw CacheError{87};memory.check(GuestAddress{arg(3)},260U*width,Access::Write);const auto extension=text(arg(2));const auto path=create_file(filesystem_,extension);if(wide)memory.write(GuestAddress{arg(3)},utf16_bytes(path));else{auto name=utf16_to_utf8(path);name.push_back(0);memory.write(GuestAddress{arg(3)},std::span(reinterpret_cast<const std::uint8_t*>(name.data()),name.size()));}result=1;
        }else if(symbol=="ReadUrlCacheEntryStream" || symbol=="ReadUrlCacheEntryStreamEx"){
            const bool extended=symbol.ends_with("Ex");if(!extended && d(4))throw CacheError{87};const auto found=local.borrows.find(arg(0));if(found==local.borrows.end() || !found->second.stream)throw CacheError{6};if(!arg(3))throw CacheError{87};memory.check(GuestAddress{arg(3)},4,Access::Read);memory.check(GuestAddress{arg(3)},4,Access::Write);const auto count=memory.load(GuestAddress{arg(3)},32);if(count>output_limit)throw CacheError{87};memory.check(GuestAddress{arg(2)},static_cast<std::size_t>(count),Access::Write);std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));const auto io=found->second.file->read(bytes,extended?arg(1):d(1));require(io.error);memory.write(GuestAddress{arg(2)},std::span(bytes).first(static_cast<std::size_t>(io.bytes)));memory.store(GuestAddress{arg(3)},32,io.bytes);result=1;
        }else if(symbol=="FindCloseUrlCache"){if(!local.enumerations.erase(arg(0)))throw CacheError{6};result=1;
        }else{
            Transaction transaction(runtime_.registry,registry_file_);auto& registry=runtime_.registry;
            if(symbol.starts_with("FindFirstUrlCacheEntry") || symbol.starts_with("FindNextUrlCacheEntry")){
                const bool first=symbol.starts_with("FindFirst");UrlCacheState::Enumeration enumeration;UrlCacheState::Enumeration* current=nullptr;std::uint64_t handle=arg(0);
                if(first){const auto pattern=text(arg(0));if(!pattern.empty() && pattern!=u"cookie:" && pattern!=u"visited:")throw CacheError{87};const auto index=registry.keys.find(url_cache::index);if(index!=registry.keys.end())for(const auto& [name,value]:index->second){(void)name;const auto entry=url_cache::decode(value.bytes);if(pattern.empty()?(entry.type&1)!=0:pattern==u"cookie:"?(entry.type&0x100000U)!=0:(entry.type&0x200000U)!=0)enumeration.urls.push_back(entry.url);}current=&enumeration;
                }else{const auto found=local.enumerations.find(handle);if(found==local.enumerations.end())throw CacheError{6};current=&found->second;}
                if(first && local.enumerations.size()>=4096)throw CacheError{8};
                std::optional<Entry> entry;
                while(current->cursor<current->urls.size()){
                    try{entry=lookup(registry,filesystem_,current->urls[current->cursor],false,transaction);if(!entry->deleting)break;entry.reset();}
                    catch(const CacheError& error){if(error.code!=2 && error.code!=3)throw;}
                    ++current->cursor;
                }
                if(!entry)throw CacheError{259};emit(*entry,arg(1),arg(2));transaction.commit();++current->cursor;if(first){handle=local.next++;local.enumerations.emplace(handle,std::move(enumeration));}result=first?handle:1;
            }else if(symbol=="UnlockUrlCacheEntryStream" || symbol.starts_with("UnlockUrlCacheEntryFile")){
                if(d(1))throw CacheError{87};auto found=local.borrows.end();if(symbol=="UnlockUrlCacheEntryStream")found=local.borrows.find(arg(0));else{const auto url=text(arg(0));found=std::find_if(local.borrows.begin(),local.borrows.end(),[&](const auto& entry){return !entry.second.stream && entry.second.url==url;});}
                if(found==local.borrows.end() || (symbol=="UnlockUrlCacheEntryStream" && !found->second.stream))throw CacheError{6};auto entry=url_cache::get(registry,found->second.url);if(!entry)throw CacheError{2};const auto file=found->second.file;found->second.file.reset();try{retire(registry,filesystem_,*entry,found->second.id);transaction.commit();}catch(...){found->second.file=file;throw;}local.borrows.erase(found);runtime_.http.cache_active=!local.borrows.empty();result=1;
            }else if(symbol.starts_with("CommitUrlCacheEntry")){
                Entry entry;entry.url=text(arg(0));if(entry.url.empty())throw CacheError{87};(void)url_cache::key(entry.url);entry.file=text(arg(1));if(arg(7) || d(6)>1024*1024U/width)throw CacheError{87};if(!entry.file.empty() && !own_file(entry.file))throw CacheError{87};entry.type=d(4);entry.expires=arg(2);entry.modified=arg(3);entry.synced=entry.accessed=url_cache::now();entry.exemption=(entry.type&4)?86400:0;entry.header_wide=wide;
                if(!entry.file.empty()){const auto info=filesystem_.information(utf16_to_utf8(entry.file));require(info.error);entry.size=info.metadata.size;const auto dot=entry.file.find_last_of(u'.');if(dot!=entry.file.npos)entry.extension=entry.file.substr(dot+1);}
                entry.headers.resize(static_cast<std::size_t>(d(6))*width);if(arg(5))memory.read(GuestAddress{arg(5)},entry.headers);else if(!entry.headers.empty()){const auto file=filesystem_.open_reader(utf16_to_utf8(entry.file));require(file.error);const auto io=file.file->read(entry.headers,0);require(io.error);if(io.bytes!=entry.headers.size())throw CacheError{13};}
                const auto old=url_cache::get(registry,entry.url);if(old){auto previous=*old;prune(previous);if(!previous.leases.empty())throw CacheError{5};entry.hits=previous.hits;}
                url_cache::put(registry,entry);const auto original=text(arg(8));if(!original.empty() && original!=entry.url){Entry alias;alias.url=original;alias.redirect=entry.url;alias.type=0;alias.synced=alias.accessed=entry.synced;url_cache::put(registry,alias);}transaction.commit();if(old && old->file!=entry.file && !old->file.empty() && own_file(old->file))(void)filesystem_.remove(utf16_to_utf8(old->file),false);result=1;
            }else{
                const auto url=text(arg(0));if(url.empty())throw CacheError{87};(void)url_cache::key(url);const bool ex=symbol.starts_with("GetUrlCacheEntryInfoEx");if(ex && (arg(3) || arg(4) || arg(5) || d(6)))throw CacheError{87};auto entry=lookup(registry,filesystem_,url,ex,transaction);
                if(symbol.starts_with("DeleteUrlCacheEntry")){
                    if(!entry.leases.empty()){entry.deleting=true;url_cache::put(registry,entry);transaction.commit();throw CacheError{5};}if(!entry.file.empty())require(filesystem_.remove(utf16_to_utf8(entry.file),false));url_cache::erase(registry,url);transaction.commit();result=1;
                }else if(symbol.starts_with("SetUrlCacheEntryInfo")){
                    const auto fixed=x86_?80U:112U;if(!arg(1) || (d(2)&~0xbd4U))throw CacheError{87};memory.check(GuestAddress{arg(1)},fixed,Access::Read);if(memory.load(GuestAddress{arg(1)},32)!=fixed)throw CacheError{87};const auto type=x86_?12U:24U,times=x86_?32U:44U;const auto read=[&](unsigned offset,unsigned bits=32){return memory.load(GuestAddress{arg(1)+offset},static_cast<std::uint8_t>(bits));};if(d(2)&4)entry.type=static_cast<std::uint32_t>(read(type));if(d(2)&16)entry.hits=static_cast<std::uint32_t>(read(type+8));if(d(2)&64)entry.modified=read(times,64);if(d(2)&128)entry.expires=read(times+8,64);if(d(2)&256)entry.accessed=read(times+16,64);if(d(2)&512)entry.synced=read(times+24,64);if(d(2)&2048)entry.exemption=static_cast<std::uint32_t>(read(x86_?76:104));url_cache::put(registry,entry);transaction.commit();result=1;
                }else if(symbol.starts_with("RetrieveUrlCacheEntry")){
                    const bool stream=symbol.starts_with("RetrieveUrlCacheEntryStream");if(d(stream?4:3))throw CacheError{87};if(entry.file.empty() || entry.deleting)throw CacheError{2};if(local.borrows.size()>=1024)throw CacheError{8};const auto file=filesystem_.open_host(utf16_to_utf8(entry.file),0x80000000U,3,0,7);require(file.error);const auto lease=(static_cast<std::uint64_t>(::arc4random())<<32)|::arc4random();entry.leases.push_back({lease,url_cache::now()+UINT64_C(3000000000),host_process_id()});entry.accessed=url_cache::now();if(entry.hits!=UINT32_MAX)++entry.hits;emit(entry,arg(1),arg(2));const auto handle=local.next++;local.borrows.emplace(handle,UrlCacheState::Borrow{entry.url,lease,file.file,stream});try{url_cache::put(registry,entry);transaction.commit();}catch(...){local.borrows.erase(handle);throw;}runtime_.http.cache_active=true;result=stream?handle:1;
                }else if(symbol.starts_with("GetUrlCacheEntryInfo")){if(!(ex && !arg(1) && !arg(2)))emit(entry,arg(1),arg(2));url_cache::put(registry,entry);transaction.commit();result=1;
                }else throw CacheError{87};
            }
        }
    }catch(const CacheError& error){last_error_=error.code;}catch(const MemoryFault&){last_error_=998;}catch(const std::invalid_argument&){last_error_=13;}catch(const std::length_error&){last_error_=8;}catch(const std::bad_alloc&){last_error_=8;}
    arg.return_to(state,result);return std::nullopt;
}
std::shared_ptr<NativeHttp> WindowsServices::read_http_cache(HttpHandle& request){
    if(!request.inet || request.input.method!="GET" || (request.cache_flags&0x80000100U))return {};
    const bool offline=(request.cache_flags&0x01000000U)!=0;
    if(!offline && private_request(request))return {};
    try{
        Transaction transaction(runtime_.registry,registry_file_);auto entry=lookup(runtime_.registry,filesystem_,utf8_to_utf16(request.input.url),true,transaction);if(entry.file.empty() || entry.deleting || (!offline && (!entry.expires || entry.expires<=url_cache::now())) || entry.size>output_limit)return {};
        std::string header;if(entry.header_wide){std::u16string text;for(std::size_t i=0;i<entry.headers.size();i+=2)text.push_back(static_cast<char16_t>(entry.headers[i]|static_cast<unsigned>(entry.headers[i+1])<<8));header=utf16_to_utf8(text);}else header.assign(reinterpret_cast<const char*>(entry.headers.data()),entry.headers.size());
        HttpResult result;result.status=200;result.status_line="HTTP/1.1 200 OK";result.effective_url=utf16_to_utf8(entry.url);
        for(std::size_t at=0;at<header.size();){const auto end=header.find("\r\n",at);const auto line=header.substr(at,end==header.npos?header.size()-at:end-at);at=end==header.npos?header.size():end+2;if(line.starts_with("HTTP/")){result.status_line=line;const auto blank=line.find(' ');if(blank!=line.npos){unsigned status=0;const auto parsed=std::from_chars(line.data()+blank+1,line.data()+line.size(),status);if(parsed.ec==std::errc{})result.status=status;}}else{const auto colon=line.find(':');if(colon!=line.npos){const auto name=line.substr(0,colon),key=lower(name);auto value=line.substr(colon+1);const auto first=value.find_first_not_of(" \t");value=first==value.npos?"":value.substr(first);if(!offline && (key=="vary" || key=="set-cookie" || (key=="cache-control" && (lower(value).find("private")!=std::string::npos || lower(value).find("no-cache")!=std::string::npos))))return {};result.headers.emplace_back(name,value);}}}
        auto file=filesystem_.open_reader(utf16_to_utf8(entry.file));require(file.error);result.body.resize(static_cast<std::size_t>(entry.size));const auto io=file.file->read(result.body,0);require(io.error);if(io.bytes!=result.body.size())return {};entry.accessed=url_cache::now();if(entry.hits!=UINT32_MAX)++entry.hits;url_cache::put(runtime_.registry,entry);transaction.commit();return std::make_shared<CachedRequest>(std::move(result));
    }catch(const CacheError&){return {};}catch(const std::invalid_argument&){return {};}catch(const std::length_error&){return {};}catch(const std::bad_alloc&){return {};}
}
void WindowsServices::cache_http_response(HttpHandle& request){
    if(!request.inet || request.cache_hit || request.input.method!="GET" || request.response.error || request.response.status!=200 || (request.cache_flags&0x04000000U) || !filesystem_.writable() || private_request(request))return;
    std::u16string file;
    try{
        Entry entry;entry.url=utf8_to_utf16(request.response.effective_url.empty()?request.input.url:request.response.effective_url);entry.size=request.response.body.size();entry.accessed=entry.synced=url_cache::now();std::string header=request.response.status_line+"\r\n";
        for(const auto& [name,value]:request.response.headers){header+=name+": "+value+"\r\n";const auto key=lower(name);if(key=="cache-control"){const auto control=lower(value);if(control.find("no-store")!=control.npos)return;const auto at=control.find("max-age=");if(at!=control.npos){std::uint64_t seconds=0;const auto parsed=std::from_chars(control.data()+at+8,control.data()+control.size(),seconds);if(parsed.ec==std::errc{} && seconds<=(UINT64_MAX-entry.accessed)/10000000)entry.expires=entry.accessed+seconds*10000000;}}
            if(key=="expires" || key=="last-modified"){const auto time=::curl_getdate(value.c_str(),nullptr);if(time>=0){const auto ticks=UINT64_C(116444736000000000)+static_cast<std::uint64_t>(time)*10000000;if(key=="last-modified")entry.modified=ticks;else if(!entry.expires)entry.expires=ticks;}}}
        header+="\r\n";entry.headers.assign(header.begin(),header.end());file=create_file(filesystem_,u"dat");entry.file=file;entry.extension=u"dat";
        auto output=filesystem_.open_host(utf16_to_utf8(file),0x40000000U,3,0,7);require(output.error);const auto io=output.file->write(request.response.body,0);require(io.error);if(io.bytes!=request.response.body.size())throw CacheError{29};require(output.file->flush());output.file.reset();
        Transaction transaction(runtime_.registry,registry_file_);const auto previous=url_cache::get(runtime_.registry,entry.url);if(previous){auto old=*previous;prune(old);if(!old.leases.empty())throw CacheError{5};entry.hits=old.hits;}url_cache::put(runtime_.registry,entry);const auto original=utf8_to_utf16(request.input.url);if(original!=entry.url){Entry alias;alias.url=original;alias.redirect=entry.url;alias.type=0;alias.synced=alias.accessed=entry.synced;url_cache::put(runtime_.registry,alias);}transaction.commit();file.clear();if(previous && !previous->file.empty() && previous->file!=entry.file && own_file(previous->file))(void)filesystem_.remove(utf16_to_utf8(previous->file),false);
    }catch(const CacheError&){}catch(const std::invalid_argument&){}catch(const std::length_error&){}catch(const std::bad_alloc&){}
    if(!file.empty())(void)filesystem_.remove(utf16_to_utf8(file),false);
}
}

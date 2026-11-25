#include "icu_resource_files.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#if !defined(_WIN32)
#include <cstdlib>
#include <unistd.h>
#endif

namespace x64arm::icu_resource_files {
namespace {
using Input=std::span<const std::uint8_t>;
using Output=std::span<std::uint8_t>;

std::uint32_t word(Input bytes,std::size_t at,unsigned width){
    std::uint32_t result{};
    for(unsigned i=0;i<width;++i)result|=std::uint32_t(bytes[at+i])<<(8*i);
    return result;
}
void store(Output bytes,std::size_t at,std::uint32_t value,unsigned width){
    for(unsigned i=0;i<width;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));
}
bool fits(Input bytes,std::size_t at,std::size_t length){
    return at<=bytes.size() && length<=bytes.size()-at;
}
std::string_view piece(Input bytes,std::size_t from,std::size_t limit){
    if(from>=limit || limit>bytes.size())return {};
    const auto begin=bytes.subspan(from,limit-from);
    const auto end=std::find(begin.begin(),begin.end(),0);
    if(end==begin.end())return {};
    return {reinterpret_cast<const char*>(begin.data()),static_cast<std::size_t>(end-begin.begin())};
}
std::size_t header(Input data,std::string_view type){
    if(data.size()<24 || data[2]!=0xda || data[3]!=0x27 || data[8] || data[9] || data[10]!=2 ||
       std::string_view(reinterpret_cast<const char*>(data.data()+12),4)!=type)return 0;
    const auto size=word(data,0,2),info=word(data,4,2);
    return size>=24 && size<=data.size() && !(size&3) && info>=20 && info<=size-4?size:0;
}

struct Resource {
    Output body;
    std::uint32_t root{},attributes{},checksum{},pool_strings{},pool_strings16{};
    std::size_t keys_first{},keys_end{},units_end{},data_begin{};
    bool indexed{},pool{},uses_pool{};
    std::string alias,parent;

    explicit Resource(Bytes& bytes){
        const auto start=header(bytes,"ResB");
        if(!start || bytes[16]<1 || bytes[16]>3 || bytes.size()-start<4)return;
        body=Output(bytes).subspan(start);
        root=word(body,0,4);
        const auto root_type=root>>28;
        if(root_type!=2 && root_type!=4 && root_type!=5){body={};return;}
        indexed=bytes[16]!=1 || bytes[17]!=0;
        keys_first=4;
        if(!indexed){
            keys_end=units_end=body.size();
            data_begin=4;
            return;
        }

        if(body.size()<24){body={};return;}
        const auto indexes=word(body,4,4);
        const auto count=indexes&255U;
        if(count<5 || !fits(body,4,std::size_t(count)*4)){body={};return;}
        keys_first=4+4*std::size_t(count);
        const auto keys=word(body,8,4), resources=word(body,12,4), end=word(body,16,4);
        const auto units=count>6?word(body,28,4):keys;
        if(keys<keys_first/4 || keys>units || units>resources || resources>end || end>body.size()/4){
            body={};return;
        }
        keys_end=4*std::size_t(keys);
        data_begin=units_end=4*std::size_t(units);
        body=body.first(4*std::size_t(end));
        if(count>5)attributes=word(body,24,4);
        pool=(attributes&2U)!=0;
        uses_pool=(attributes&4U)!=0;
        if((pool || uses_pool) && (count<8 || (pool && uses_pool))){body={};return;}
        if(count>7)checksum=word(body,32,4);
        if(bytes[16]==3){
            pool_strings=(indexes>>8)|((attributes&0xf000U)<<12);
            pool_strings16=attributes>>16;
            if(pool_strings16>pool_strings){body={};return;}
        }
    }
    Input keys()const{return body.subspan(keys_first,keys_end-keys_first);}
    Input units()const{return indexed?body.subspan(keys_end,units_end-keys_end):Input{};}
};

struct Text {
    Input bytes;
    std::uint32_t length{};
    explicit operator bool()const{return !bytes.empty();}
};

Text text16(Input in,std::size_t at){
    if(!fits(in,at,2))return {};
    const auto first=word(in,at,2);
    std::uint32_t length{};
    if(first<0xdc00 || first>0xdfff){
        const auto begin=at;
        while(fits(in,at,2) && word(in,at,2))at+=2;
        if(!fits(in,at,2))return {};
        return {in.subspan(begin,at-begin+2),static_cast<std::uint32_t>((at-begin)/2)};
    }
    at+=2;
    if(first<0xdfef)length=first&0x3ffU;
    else if(first<0xdfff && fits(in,at,2)){
        length=((first-0xdfefU)<<16)|word(in,at,2);
        at+=2;
    }else if(first==0xdfff && fits(in,at,4)){
        length=(word(in,at,2)<<16)|word(in,at+2,2);
        at+=4;
    }else return {};
    const auto size=2*(std::size_t(length)+1);
    if(!fits(in,at,size) || word(in,at+size-2,2))return {};
    return {in.subspan(at,size),length};
}

Text resource_text(const Resource& res,const Resource* pool,std::uint32_t value){
    const auto type=value>>28,offset=value&0x0fffffffU;
    static constexpr std::array<std::uint8_t,2> empty{};
    if(type==6){
        if(offset<res.pool_strings)
            return pool?text16(pool->units(),2*std::size_t(offset)):Text{};
        const auto units=res.units();
        const auto local=offset-res.pool_strings;
        if(!local && units.empty())return {empty,0};
        return text16(units,2*std::size_t(local));
    }
    if(type!=0 && type!=3)return {};
    if(!offset)return {empty,0};
    const auto at=4*std::size_t(offset);
    if(at<res.data_begin || !fits(res.body,at,4))return {};
    const auto length=word(res.body,at,4);
    const auto size=2*(std::size_t(length)+1);
    if(!fits(res.body,at+4,size) || word(res.body,at+4+size-2,2))return {};
    return {res.body.subspan(at+4,size),length};
}

std::string invariant(Text text){
    std::string result;
    result.reserve(text.length);
    for(std::uint32_t i=0;i<text.length;++i){
        const auto unit=word(text.bytes,2*std::size_t(i),2);
        if(unit<0x20 || unit>=0x7f)return {};
        result.push_back(static_cast<char>(unit));
    }
    return result;
}

class Walk {
    struct AliasRewrite {
        std::size_t length;
        std::string target;
    };
    Resource& res_;
    const Resource* pool_;
    std::string_view package_;
    std::vector<std::uint16_t> words_;
    std::vector<std::uint8_t> units_;
    std::vector<std::uint32_t> active_;
    std::map<std::size_t,AliasRewrite> aliases_;
    bool check_rewrites_{};

    bool unmodified(std::size_t start,std::size_t length,bool own_alias=false)const{
        if(!check_rewrites_)return true;
        auto next=aliases_.lower_bound(start);
        if(own_alias && next!=aliases_.end() && next->first==start)++next;
        if(next!=aliases_.end() && next->first<start+length)return false;
        auto previous=aliases_.lower_bound(start);
        if(previous==aliases_.begin())return true;
        --previous;
        return previous->first+previous->second.length<=start;
    }

    std::string_view key(std::uint32_t value,unsigned width)const{
        if(width==4 && (value&0x80000000U))
            return pool_?piece(pool_->keys(),value&0x7fffffffU,pool_->keys().size()):std::string_view{};
        const auto key_limit=res_.indexed && res_.keys_first==res_.keys_end?0:res_.keys_end;
        if(width==2 && value>=key_limit)
            return pool_?piece(pool_->keys(),value-key_limit,pool_->keys().size()):std::string_view{};
        if(value<res_.keys_first)return {};
        const auto name=piece(res_.body,value,res_.keys_end);
        return !name.empty() && unmodified(value,name.size()+1)?name:std::string_view{};
    }

    bool alias(std::uint32_t value){
        const auto text=resource_text(res_,pool_,value);
        if(!text || !text.length)return false;
        const auto offset=4*std::size_t(value&0x0fffffffU);
        const auto length=4+text.bytes.size();
        bool rewrite=false;
        auto path=invariant(text);
        if(path.empty() || path.find('\\')!=path.npos || path.find(':')!=path.npos)return false;
        if(path.front()=='/'){
            const auto end=path.find('/',1);
            if(end==path.npos)return path=="/ICUDATA" && unmodified(offset,length);
            const auto scope=std::string_view(path).substr(1,end-1);
            if(scope=="LOCALE")return end+1<path.size() && unmodified(offset,length);
            if(scope!="ICUDATA"){
                if(scope!=package_)return false;
                rewrite=true;
                path.erase(0,end+1);
            }else path.erase(0,end+1);
        }
        if(!item_name(std::string_view(path).substr(0,path.find('/'))) || !unmodified(offset,length,rewrite))return false;
        if(rewrite && !check_rewrites_)aliases_.try_emplace(offset,AliasRewrite{length,std::move(path)});
        return true;
    }

    bool metadata(std::string_view name,std::uint32_t value){
        if(name!="%%ALIAS" && name!="%%Parent")return true;
        const auto text=resource_text(res_,pool_,value);
        if(!text || !text.length)return true;
        if(text.length>99)return false;
        auto target=invariant(text);
        if(!item_name(target))return false;
        (name=="%%ALIAS"?res_.alias:res_.parent)=std::move(target);
        return true;
    }

    bool table_keys(Input data,std::size_t keys,std::size_t values,std::uint32_t count,
                    unsigned key_width,unsigned value_width,bool root){
        std::string_view previous;
        for(std::uint32_t i=0;i<count;++i){
            const auto current=key(word(data,keys+key_width*std::size_t(i),key_width),key_width);
            if(current.empty() || (i && current<=previous))return false;
            previous=current;
            const auto raw=word(data,values+value_width*std::size_t(i),value_width);
            const auto value=value_width==2?from16(raw):raw;
            if((root && !metadata(current,value)) || !visit(value))return false;
        }
        return true;
    }

    std::uint32_t from16(std::uint32_t raw)const{
        if(raw>=res_.pool_strings16)raw=raw-res_.pool_strings16+res_.pool_strings;
        return raw<=0x0fffffffU?(6U<<28)|raw:UINT32_MAX;
    }

    bool visit(std::uint32_t value,bool root=false){
        const auto type=value>>28, offset=value&0x0fffffffU;
        if(type==7)return true;
        if(type==6){
            const auto text=resource_text(res_,pool_,value);
            return bool(text);
        }
        if(type==5 || type==9){
            const auto data=res_.units();
            const auto start=std::size_t(offset)*2;
            if(!start && data.empty())return true;
            if(!fits(data,start,2))return false;
            if(units_[offset]&(type==5?1U:2U))return true;
            const auto count=word(data,start,2), stride=type==5?4U:2U;
            if(!fits(data,start+2,std::size_t(count)*stride))return false;
            if(type==5){
                if(!table_keys(data,start+2,start+2+2*std::size_t(count),count,2,2,root))return false;
            }else for(std::uint32_t i=0;i<count;++i){
                if(!visit(from16(word(data,start+2+2*std::size_t(i),2))))return false;
            }
            units_[offset]|=type==5?1U:2U;
            return true;
        }
        if(type!=0 && type!=1 && type!=2 && type!=3 && type!=4 && type!=8 && type!=14)return false;
        if(!offset)return !root || type==2 || type==4;
        const auto start=std::size_t(offset)*4;
        if(start<res_.data_begin || !fits(res_.body,start,4))return false;
        const auto bit=static_cast<std::uint16_t>(1U<<type);
        if(words_[offset]&bit)return true;
        if(active_.size()==128 || std::find(active_.begin(),active_.end(),value)!=active_.end())return false;
        active_.push_back(value);
        const auto ok=visit_at(value,type,start,root);
        active_.pop_back();
        if(ok)words_[offset]|=bit;
        return ok;
    }

    bool visit_at(std::uint32_t value,std::uint32_t type,std::size_t start,bool root){
        if(type==0){
            const auto text=resource_text(res_,pool_,value);
            return text && unmodified(start,4+text.bytes.size());
        }
        if(type==3)return alias(value);
        const auto count=word(res_.body,start,type==2?2U:4U);
        if(type==1 || type==14){
            const auto length=std::size_t(count)*(type==1?1:4);
            return fits(res_.body,start+4,length) && unmodified(start,4+length);
        }
        if(type==2 || type==4){
            const unsigned key_width=type==2?2U:4U;
            const auto keys=start+key_width;
            const auto data=(keys+key_width*std::size_t(count)+3)&~std::size_t{3};
            return fits(res_.body,data,std::size_t(count)*4) &&
                   unmodified(start,data-start+std::size_t(count)*4) &&
                   table_keys(res_.body,keys,data,count,key_width,4,root);
        }
        if(!fits(res_.body,start+4,std::size_t(count)*4) || !unmodified(start,4+std::size_t(count)*4))return false;
        for(std::uint32_t i=0;i<count;++i)
            if(!visit(word(res_.body,start+4+4*std::size_t(i),4)))return false;
        return true;
    }

public:
    Walk(Resource& resource,const Resource* pool,std::string_view package)
        :res_(resource),pool_(pool),package_(package),words_(resource.body.size()/4),
         units_(resource.units().size()/2){}
    bool run(){
        if(!visit(res_.root,true))return false;
        if(aliases_.empty())return true;
        // 先確認絕對別名的原始資料不與任何其他資源共用，避免改寫影響資源結構或字串。
        check_rewrites_=true;
        std::fill(words_.begin(),words_.end(),0);
        std::fill(units_.begin(),units_.end(),0);
        if(!visit(res_.root,true))return false;
        for(const auto& [offset,alias]:aliases_){
            store(res_.body,offset,static_cast<std::uint32_t>(alias.target.size()),4);
            for(std::size_t i=0;i<alias.target.size();++i)
                store(res_.body,offset+4+2*i,static_cast<std::uint8_t>(alias.target[i]),2);
            store(res_.body,offset+4+2*alias.target.size(),0,2);
        }
        return true;
    }
};
}

bool item_name(std::string_view name){
    return !name.empty() && name.size()<=255 && std::all_of(name.begin(),name.end(),[](char c){
        return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') ||
               c=='_' || c=='-' || c=='.';
    }) && name!="." && name!="..";
}

Files unpack(Input bytes,std::string_view tree,UErrorCode& status){
    const auto invalid=[&](){status=U_INVALID_FORMAT_ERROR;return Files{};};
    const auto start=header(bytes,"CmnD");
    if(!start || bytes[16]!=1 || bytes.size()-start<4)return invalid();
    const auto data=bytes.subspan(start);
    const auto count=word(data,0,4);
    if(!count || count>(data.size()-4)/8)return invalid();
    const auto table=4+8*std::size_t(count);
    struct Entry{std::uint32_t key{},data{};};
    std::vector<Entry> entries;
    entries.reserve(count);
    for(std::size_t i=0;i<count;++i){
        const auto key=word(data,4+8*i,4),offset=word(data,8+8*i,4);
        if(key<table || offset<table || offset>=data.size() || (offset&3))return invalid();
        entries.push_back({key,offset});
    }
    std::sort(entries.begin(),entries.end(),[](auto a,auto b){return a.data<b.data;});
    const auto names_end=entries.front().data;
    Files files;
    for(std::size_t i=0;i<entries.size();++i){
        auto name=piece(data,entries[i].key,names_end);
        auto next=i+1==entries.size()?data.size():std::size_t(entries[i+1].data);
        const auto begin=entries[i].data;
        if(name.empty() || next<=begin)return invalid();
        if(!name.ends_with(".res"))continue;
        const auto slash=name.find('/');
        if(slash==name.npos || !item_name(name.substr(0,slash)))return invalid();
        name.remove_prefix(slash+1);
        if(!tree.empty()){
            const auto separator=name.find('/');
            if(separator==name.npos || name.substr(0,separator)!=tree)continue;
            name.remove_prefix(separator+1);
        }else if(name.find('/')!=name.npos)continue;
        name.remove_suffix(4);
        if(!item_name(name) || !files.emplace(std::string(name),Bytes(data.begin()+begin,data.begin()+std::ptrdiff_t(next))).second)
            return invalid();
    }
    if(files.empty())status=U_MISSING_RESOURCE_ERROR;
    return files;
}

bool validate(Files& files,std::string_view package,UErrorCode& status){
    const auto invalid=[&](){status=U_INVALID_FORMAT_ERROR;return false;};
    if(files.empty()){status=U_MISSING_RESOURCE_ERROR;return false;}
    std::map<std::string,Resource,std::less<>> entries;
    for(auto& [name,data]:files){
        if(!item_name(name))return invalid();
        const auto it=entries.try_emplace(name,data).first;
        if(it->second.body.empty() || (it->second.pool && name!="pool"))return invalid();
    }
    const auto pool=entries.find("pool");
    const auto* shared=pool!=entries.end() && pool->second.pool?&pool->second:nullptr;
    for(auto& [name,res]:entries){
        if(res.uses_pool && (!shared || shared->checksum!=res.checksum))return invalid();
        if(!Walk(res,res.uses_pool?shared:nullptr,package).run())return invalid();
    }

    // ICU 會遞迴讀取整包別名與上層地區，載入前先驗證同一命名空間的相依圖。
    std::map<std::string_view,bool,std::less<>> seen;
    const auto visit=[&](auto&& self,std::string_view name,unsigned depth)->bool{
        const auto found=entries.find(name);
        if(found==entries.end())return true;
        if(depth>=128)return false;
        const auto [item,inserted]=seen.emplace(name,false);
        if(!inserted)return item->second;
        const auto& res=found->second;
        const auto separator=name.rfind('_');
        const auto implicit=name=="root"?std::string_view{}:
            (separator==name.npos?std::string_view("root"):name.substr(0,separator));
        if(!res.alias.empty() && !self(self,res.alias,depth+1))return false;
        if(!(res.attributes&1U) && !res.pool){
            const auto parent=res.parent.empty()?implicit:std::string_view(res.parent);
            if(!parent.empty() && !self(self,parent,depth+1))return false;
        }
        item->second=true;
        return true;
    };
    for(const auto& [name,res]:entries)if(!visit(visit,name,0))return invalid();
    return true;
}

Package::~Package(){
    if(directory.empty())return;
    std::error_code ignored;
    std::filesystem::remove_all(directory,ignored);
}

std::shared_ptr<const Package> stage(const Files& files,std::string direct_locale,UErrorCode& status){
#if !defined(_WIN32)
    std::error_code error;
    auto pattern=(std::filesystem::temp_directory_path(error)/"x64arm-icu-XXXXXX").string();
    if(error){status=U_FILE_ACCESS_ERROR;return {};}
    if(!item_name(direct_locale) && !direct_locale.empty()){status=U_ILLEGAL_ARGUMENT_ERROR;return {};}
    auto package=std::make_shared<Package>();
    if(!::mkdtemp(pattern.data())){status=U_FILE_ACCESS_ERROR;return {};}
    package->directory=pattern;
    package->native_path=pattern+'/';
    package->direct_locale=std::move(direct_locale);
    for(const auto& [name,contents]:files){
        if(!item_name(name)){status=U_INVALID_FORMAT_ERROR;return {};}
        std::ofstream output(package->directory/(name+".res"),std::ios::binary);
        output.write(reinterpret_cast<const char*>(contents.data()),static_cast<std::streamsize>(contents.size()));
        output.close();
        if(!output){status=U_FILE_ACCESS_ERROR;return {};}
        std::filesystem::permissions(package->directory/(name+".res"),std::filesystem::perms::owner_read,
                                     std::filesystem::perm_options::replace,error);
        if(error){status=U_FILE_ACCESS_ERROR;return {};}
    }
    return package;
#else
    (void)files;(void)direct_locale;
    status=U_UNSUPPORTED_ERROR;
    return {};
#endif
}
}

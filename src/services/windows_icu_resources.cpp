#include "x64arm/windows_services.hpp"
#include "icu_guest_abi.hpp"
#include "icu_resources_layout.hpp"
#include "icu_resource_files.hpp"
#include "service_callback_task.hpp"
#include <unicode/uenum.h>
#include <unicode/ustring.h>

namespace x64arm {
namespace {
using OwnedBundle=std::unique_ptr<UResourceBundle,decltype(&ures_close)>;
using icu_resources::View;
using icu_abi::IcuStatus;

struct Bundle {
    std::shared_ptr<const icu_resource_files::Package> package;
    OwnedBundle native{nullptr,ures_close};
    std::map<std::pair<std::uintptr_t,std::size_t>,std::uint64_t> aliases;
    std::vector<std::uint64_t> allocations;
    View view;
};

// ICU 的共用資料快取只以檔名識別。直接交付客體路徑會跨越磁碟映射及資料隔離。
bool builtin_package(std::string_view name){
    constexpr std::string_view base=U_ICUDATA_NAME;
    constexpr std::string_view trees[]{"-brkitr","-coll","-curr","-lang","-rbnf","-region",
                                      "-translit","-unit","-zone"};
    if(name.empty() || name==base)return true;
    if(!name.starts_with(base))return false;
    name.remove_prefix(base.size());
    return std::find(std::begin(trees),std::end(trees),name)!=std::end(trees);
}

bool resource_name(std::string_view name){
    return name!="." && name!=".." && name.find_first_of("/\\:")==name.npos;
}

UErrorCode package_error(std::uint32_t error){
    if(error==2 || error==3)return U_MISSING_RESOURCE_ERROR;
    return error==8?U_MEMORY_ALLOCATION_ERROR:U_FILE_ACCESS_ERROR;
}

bool package_suffix(std::string_view path,std::string_view suffix){
    if(path.size()<suffix.size())return false;
    path.remove_prefix(path.size()-suffix.size());
    return std::equal(path.begin(),path.end(),suffix.begin(),[](char left,char right){
        return ((left>='A' && left<='Z')?left+('a'-'A'):left)==right;
    });
}

std::shared_ptr<const icu_resource_files::Package> guest_package(ReadOnlyFiles& filesystem,std::string path,
                                                               UErrorCode& status){
    using namespace icu_resource_files;
    std::replace(path.begin(),path.end(),'\\','/');
    if(path.empty()){status=U_MISSING_RESOURCE_ERROR;return {};}
    const auto slash=path.find_last_of('/');
    const auto leaf_begin=slash==path.npos?0:slash+1;
    const auto leaf=std::string_view(path).substr(leaf_begin);
    const bool resource=package_suffix(leaf,".res"), archive=package_suffix(leaf,".dat");
    FileSnapshot source;
    std::string tree, identity;
    if(!resource && !leaf.empty()){
        source=filesystem.open(archive?path:path+".dat");
        identity=std::string(archive?leaf.substr(0,leaf.size()-4):leaf);
        if((source.error==2 || source.error==3) && !archive){
            const auto separator=leaf.find('-');
            if(separator!=leaf.npos && item_name(leaf.substr(separator+1))){
                tree=leaf.substr(separator+1);
                identity=leaf.substr(0,separator);
                source=filesystem.open(path.substr(0,leaf_begin)+identity+".dat");
            }
        }
        if(source.bytes){
            auto files=unpack(*source.bytes,tree,status);
            if(U_FAILURE(status) || !validate(files,identity,status))return {};
            return stage(files,{},status);
        }
        if(archive || (source.error!=2 && source.error!=3)){
            status=package_error(source.error);
            return {};
        }
    }

    const auto directory=resource?(leaf_begin?path.substr(0,leaf_begin):std::string(".")):path;
    auto direct=resource?std::string(leaf.substr(0,leaf.size()-4)):std::string{};
    std::string lower_direct;
    FileSnapshot explicit_source;
    if(resource){
        if(!item_name(direct)){status=U_ILLEGAL_ARGUMENT_ERROR;return {};}
        explicit_source=filesystem.open(path);
        if(!explicit_source.bytes){status=package_error(explicit_source.error);return {};}
        lower_direct=direct;
        for(auto& c:lower_direct)if(c>='A' && c<='Z')c=static_cast<char>(c+('a'-'A'));
    }

    const auto listing=filesystem.list(directory,[](std::string_view name){return package_suffix(name,".res");});
    if(listing.error && !resource){status=package_error(listing.error);return {};}
    const auto full_directory=directory.ends_with('/')?directory:directory+'/';
    Files files;
    std::size_t bytes{};
    constexpr std::size_t package_limit=64U*1024U*1024U;
    const auto add=[&](std::string_view name,const FileSnapshot& file){
        if(file.bytes->size()>package_limit-bytes){status=U_MEMORY_ALLOCATION_ERROR;return false;}
        bytes+=file.bytes->size();
        const auto inserted=files.emplace(std::string(name),*file.bytes).second;
        if(!inserted)status=U_INVALID_FORMAT_ERROR;
        return inserted;
    };
    constexpr std::uint32_t file_attribute_directory=0x10;
    bool selected=false;
    for(const auto& item:listing.entries){
        if((item.metadata.attributes&file_attribute_directory) || item.name.size()<5)continue;
        const auto name=std::string_view(item.name).substr(0,item.name.size()-4);
        if(!item_name(name))continue;
        const bool exact=resource && name.size()==lower_direct.size() && package_suffix(name,lower_direct);
        if(exact){
            if(selected){status=U_INVALID_FORMAT_ERROR;return {};}
            selected=true;
            direct=name;
            if(!add(name,explicit_source))return {};
        }else{
            const auto part=filesystem.open(full_directory+item.name);
            // 目錄 ACL 與個別檔案的資料 ACL 獨立。略過無法讀取的相鄰檔案。
            if(part.error==2 || part.error==3 || part.error==5)continue;
            if(!part.bytes){status=package_error(part.error);return {};}
            if(!add(name,part))return {};
        }
    }
    if(resource && !selected){
        if(!add(direct,explicit_source))return {};
        if(direct!="pool" && !files.contains("pool")){
            const auto pool=filesystem.open(full_directory+"pool.res");
            if(pool.bytes && !add("pool",pool))return {};
        }
    }
    const auto end=directory.find_last_not_of('/');
    const auto previous=end==directory.npos?directory.npos:directory.rfind('/',end);
    const auto name_start=previous==directory.npos?0:previous+1;
    const auto name=std::string_view(directory).substr(name_start,end==directory.npos?0:end+1-name_start);
    if(!validate(files,name,status))return {};
    return stage(files,direct,status);
}

struct PackageInput {
    std::shared_ptr<const icu_resource_files::Package> package;
    const char* provider{};
};

PackageInput resolve_package(ReadOnlyFiles& filesystem,const std::string& name,UErrorCode& status){
    if(builtin_package(name))return {{},name.empty() || name==U_ICUDATA_NAME?nullptr:name.c_str()};
    auto package=guest_package(filesystem,name,status);
    const auto* provider=package?package->native_path.c_str():nullptr;
    return {std::move(package),provider};
}
}

struct IcuResourceBundleState {
    std::map<std::uint64_t,Bundle> bundles;
};

std::span<const WindowsServiceDescriptor> icu_resources_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_RES_API(name,count) {"icu.dll",#name,count,0,0,0,true},
#define ICU_RES_QUERY(name,count) {"icu.dll",#name,count,0,0,0,true},
#include "icu_resources_services.inc"
#undef ICU_RES_QUERY
#undef ICU_RES_API
    };
    return entries;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_resources(GuestState& state,GuestMemory& memory,
        const WindowsServiceDescriptor& service){
    using namespace icu_abi;
    const auto name=service.symbol;
    if(name=="ures_openAvailableLocales")
        return nested_continuation(state,memory,"ICU 資源包地區列舉",
            [this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);
                IcuStatus status(m,call(1));
                std::uint64_t handle{};
                if(U_SUCCESS(status.value)){
                    try{
                        const auto name=call(0)?read_ansi(m,call(0)):std::string{};
                        const auto package=resolve_package(filesystem_,name,status.value);
                        std::unique_ptr<UEnumeration,decltype(&uenum_close)> locales(U_SUCCESS(status.value)?
                            ures_openAvailableLocales(package.provider,&status.value):nullptr,uenum_close);
                        if(locales)handle=co_await export_icu_enumeration(s,m,*locales,status);
                        else status.commit();
                    }catch(const std::bad_alloc&){
                        status.value=U_MEMORY_ALLOCATION_ERROR;
                        status.commit();
                    }
                }
                call.return_to(s,handle);
                co_return std::nullopt;
            });

    const WindowsCall call(state,memory,service);
    const auto number=[&](unsigned arg){return signed_argument(call(arg));};
    const auto finish=[&](std::uint64_t result=0){
        call.return_to(state,result);
        return std::optional<std::uint32_t>{};
    };
    if(!icu_resources_)icu_resources_=std::make_shared<IcuResourceBundleState>();
    auto& records=icu_resources_->bundles;
    const auto get=[&](std::uint64_t handle)->Bundle* {
        const auto found=records.find(handle);
        return found!=records.end()?&found->second:nullptr;
    };
    const auto address=call(0);
    auto* owner=get(address);
    auto* native=owner?owner->native.get():nullptr;
    if(name=="ures_close"){
        if(owner){
            for(const auto pointer:owner->allocations)(void)free_heap(memory,pointer,state);
            records.erase(address);
            (void)free_heap(memory,address,state);
        }
        return finish();
    }
    if(name=="ures_getKey")return finish(owner?owner->view.key:0);
    if(name=="ures_getSize")return finish(native?static_cast<std::uint32_t>(owner->view.size):0);
    if(name=="ures_getType")return finish(static_cast<std::uint32_t>(owner?owner->view.type:static_cast<int32_t>(URES_NONE)));
    if(name=="ures_hasNext")return finish(owner?owner->view.has_next:0);
    const auto update_iterator=[&](){
        if(!owner)return;
        owner->view.has_next=ures_hasNext(native)!=0;
        write_icu_record(memory,address,owner->view);
    };
    if(name=="ures_resetIterator"){
        ures_resetIterator(native);
        update_iterator();
        return finish();
    }
    if(name=="ures_getVersion"){
        if(native && call(1)){
            UVersionInfo version{};
            ures_getVersion(native,version);
            memory.write(GuestAddress{call(1)},icu_bytes(version,U_MAX_VERSION_LENGTH));
        }
        return finish();
    }

    const bool integer=name=="ures_getInt" || name=="ures_getUInt";
    const bool next_resource=name=="ures_getNextResource";
    const bool child=next_resource || name=="ures_getByIndex" || name=="ures_getByKey";
    const auto fill_address=child?call(next_resource?1U:2U):0;
    auto* fill=get(fill_address);
    IcuStatus status(memory,call(service.arguments-1));
    const auto initial=status.value;
    if(U_FAILURE(initial))return finish(integer?UINT32_MAX:(fill?fill_address:0));
    const auto done=[&](std::uint64_t result=0){
        if(status.value!=initial)status.commit();
        return finish(result);
    };
    const auto illegal=[&](std::uint64_t result=0){
        status.value=U_ILLEGAL_ARGUMENT_ERROR;
        return done(result);
    };
    const auto copy=[&](Bundle& target,const void* data,std::size_t bytes)->std::uint64_t{
        if(!data)return 0;
        const auto key=std::pair{reinterpret_cast<std::uintptr_t>(data),bytes};
        if(const auto found=target.aliases.find(key);found!=target.aliases.end())return found->second;
        const auto pointer=allocate_heap(memory,std::max(bytes,std::size_t{1}));
        if(!pointer){status.value=U_MEMORY_ALLOCATION_ERROR;return 0;}
        try{
            if(bytes)memory.write(GuestAddress{pointer},{static_cast<const std::uint8_t*>(data),bytes});
            target.allocations.push_back(pointer);
            try{target.aliases.emplace(key,pointer);}
            catch(...){target.allocations.pop_back();throw;}
        }catch(...){
            (void)free_heap(memory,pointer,state);
            throw;
        }
        return pointer;
    };
    const auto copy_string=[&](Bundle& target,const char* text){
        return text?copy(target,text,std::char_traits<char>::length(text)+1):0;
    };
    const auto describe=[&](Bundle& target,UResourceBundle* bundle){
        return View{copy_string(target,ures_getKey(bundle)),ures_getSize(bundle),
                    static_cast<int32_t>(ures_getType(bundle)),ures_hasNext(bundle)!=0};
    };
    const auto adopt=[&](OwnedBundle selected,std::shared_ptr<const icu_resource_files::Package> package)->std::uint64_t{
        if(!selected || U_FAILURE(status.value))return 0;
        const auto handle=allocate_heap(memory,sizeof(View));
        if(!handle){status.value=U_MEMORY_ALLOCATION_ERROR;return 0;}
        Bundle bundle;
        bundle.package=std::move(package);
        bundle.native=std::move(selected);
        try{
            bundle.view=describe(bundle,bundle.native.get());
            if(U_SUCCESS(status.value)){
                write_icu_record(memory,handle,bundle.view);
                records.emplace(handle,std::move(bundle));
                return handle;
            }
        }catch(...){
            for(const auto allocation:bundle.allocations)(void)free_heap(memory,allocation,state);
            (void)free_heap(memory,handle,state);
            throw;
        }
        for(const auto allocation:bundle.allocations)(void)free_heap(memory,allocation,state);
        (void)free_heap(memory,handle,state);
        return 0;
    };

    try{
        if(name=="ures_open" || name=="ures_openDirect" || name=="ures_openU"){
            std::string package;
            if(call(0))package=name=="ures_openU"?utf16_to_utf8(read_utf16(memory,GuestAddress{call(0)})):
                                                 read_ansi(memory,call(0));
            const auto source=resolve_package(filesystem_,package,status.value);
            if(U_FAILURE(status.value))return done();
            const auto input=call(1);
            const auto locale=input?read_ansi(memory,input):std::string{};
            const auto* id=input?locale.c_str():icu_default_locale();
            if(!resource_name(id))return illegal();
            if(source.package && !source.package->direct_locale.empty() && (!input || locale.empty()))
                id=source.package->direct_locale.c_str();
            return done(adopt(OwnedBundle(name=="ures_openDirect"?ures_openDirect(source.provider,id,&status.value):
                                                                ures_open(source.provider,id,&status.value),ures_close),
                              source.package));
        }
        if(!native)return illegal(integer?UINT32_MAX:(fill?fill_address:0));
        if(child){
            if(fill_address && !fill)return illegal();
            const char* key=nullptr;
            std::string input;
            if(name=="ures_getByKey"){
                if(!call(1))return illegal(fill?fill_address:0);
                input=read_ansi(memory,call(1));
                key=input.c_str();
            }
            OwnedBundle selected(next_resource?ures_getNextResource(native,nullptr,&status.value):
                key?ures_getByKey(native,key,nullptr,&status.value):ures_getByIndex(native,number(1),nullptr,&status.value),
                ures_close);
            if(next_resource)update_iterator();
            if(!selected || U_FAILURE(status.value))return done(fill?fill_address:0);
            if(!fill)return done(adopt(std::move(selected),owner->package));
            // 新物件可重用宿主位址。先準備新快照，既有借用資料仍保留到 close。
            auto previous_aliases=std::move(fill->aliases);
            fill->aliases.clear();
            try{
                const auto view=describe(*fill,selected.get());
                if(U_SUCCESS(status.value)){
                    write_icu_record(memory,fill_address,view);
                    fill->view=view;
                    fill->native=std::move(selected);
                    fill->package=owner->package;
                }else fill->aliases=std::move(previous_aliases);
            }catch(...){
                fill->aliases=std::move(previous_aliases);
                throw;
            }
            return done(fill_address);
        }
        if(integer){
            const auto result=name=="ures_getInt"?static_cast<std::uint32_t>(ures_getInt(native,&status.value)):
                                                 ures_getUInt(native,&status.value);
            return done(result);
        }
        if(name=="ures_getLocaleByType"){
            const auto type=number(1);
            if(type<ULOC_ACTUAL_LOCALE || type>ULOC_VALID_LOCALE)return illegal();
            return done(copy_string(*owner,ures_getLocaleByType(native,static_cast<ULocDataLocaleType>(type),&status.value)));
        }

        const bool binary=name=="ures_getBinary";
        const bool vector=name=="ures_getIntVector";
        const bool next=name=="ures_getNextString";
        const bool indexed=name.ends_with("ByIndex");
        const bool keyed=name.ends_with("ByKey");
        const bool utf8=name.starts_with("ures_getUTF8String");
        if(!(binary || vector || next || name.starts_with("ures_getString") || utf8))
            throw HostCallFault("ICU 資源包服務未登錄："+std::string(name));
        if(keyed && !call(1))return illegal();
        std::int32_t length=-1;
        const void* data=nullptr;
        const char key_sentinel='\0';
        const char* key=&key_sentinel;
        if(binary)data=ures_getBinary(native,&length,&status.value);
        else if(vector)data=ures_getIntVector(native,&length,&status.value);
        else if(next){
            data=ures_getNextString(native,&length,&key,&status.value);
            update_iterator();
        }else if(indexed)data=ures_getStringByIndex(native,number(1),&length,&status.value);
        else if(keyed)data=ures_getStringByKey(native,read_ansi(memory,call(1)).c_str(),&length,&status.value);
        else data=ures_getString(native,&length,&status.value);

        const unsigned selector=(keyed || indexed)?2U:1U;
        const auto length_pointer=call(selector+(utf8?1U:0U));
        if(next && call(2) && key!=&key_sentinel){
            const auto pointer=copy_string(*owner,key);
            if(U_FAILURE(status.value))return done();
            memory.store(GuestAddress{call(2)},x86_?32:64,pointer);
        }
        if(!data || length<0 || U_FAILURE(status.value))return done();
        if(!utf8){
            const auto items=static_cast<std::size_t>(length)+(binary || vector?0U:1U);
            const auto bytes=items*(binary?1U:(vector?sizeof(std::int32_t):sizeof(UChar)));
            const auto result=copy(*owner,data,bytes);
            if(result && length_pointer)memory.store(GuestAddress{length_pointer},32,static_cast<std::uint32_t>(length));
            return done(result);
        }

        const auto destination=call(selector);
        const auto capacity=length_pointer?signed_argument(memory.load(GuestAddress{length_pointer},32)):0;
        const bool force=static_cast<std::uint8_t>(call(selector+2))!=0;
        if(capacity<0 || (capacity>0 && !destination))return illegal();
        if(!length && !force){
            static constexpr char empty[]="";
            const auto result=copy(*owner,empty,1);
            if(result && length_pointer)memory.store(GuestAddress{length_pointer},32,0);
            return done(result);
        }

        // ICU 在未要求固定位置時，可能從緩衝區後段開始。保留實際位置與未覆寫的位元組。
        const auto window=icu_resources::utf8_window(length,capacity,force);
        const auto output_size=window.size;
        const auto offset=static_cast<std::uint64_t>(window.offset);
        if(offset>UINT64_MAX-destination)throw MemoryFault(GuestAddress{destination},Access::Write,offset,"ICU 資源輸出位址溢位");
        const auto start=destination+offset;
        std::vector<char> buffer(static_cast<std::size_t>(output_size));
        if(output_size)memory.read(GuestAddress{start},{reinterpret_cast<std::uint8_t*>(buffer.data()),buffer.size()});
        std::int32_t required=-1;
        char empty{};
        auto* target=output_size?buffer.data():(!length && destination?&empty:nullptr);
        const auto result=u_strToUTF8(target,output_size,&required,static_cast<const UChar*>(data),length,&status.value);
        if(output_size)memory.write(GuestAddress{start},icu_bytes(buffer.data(),buffer.size()));
        if(required>=0 && length_pointer)memory.store(GuestAddress{length_pointer},32,static_cast<std::uint32_t>(required));
        return done(result?start:0);
    }catch(const std::bad_alloc&){
        status.value=U_MEMORY_ALLOCATION_ERROR;
        return done();
    }
}
}

#include "x64arm/windows_services.hpp"
#include "icu_guest_abi.hpp"
#include "icu_trie_binary.hpp"
#include "icu_trie_guest.hpp"
#include "service_callback_task.hpp"
#include <unicode/uchar.h>
#include <unicode/umutablecptrie.h>
#include <ucptrie_impl.h>

namespace x64arm {
namespace {
using namespace icu_abi;
using OwnedTrie=std::unique_ptr<UCPTrie,decltype(&ucptrie_close)>;
using MutableTrie=std::unique_ptr<UMutableCPTrie,decltype(&umutablecptrie_close)>;

struct TrieObject {
    // 從二進位還原的 ICU 物件引用這份對齊且已驗證的宿主副本。
    std::vector<std::uint32_t> binary;
    OwnedTrie owned{nullptr,ucptrie_close};
    const UCPTrie* borrowed{};
    const UCPTrie* get()const{return owned?owned.get():borrowed;}
};

constexpr std::int32_t unicode_limit=0x110000;
constexpr std::int32_t surrogate_first=0xd800;

template<class Pointer>
icu_trie::View<Pointer> guest_view(const UCPTrie& trie,std::uint64_t index,std::uint64_t data){
    return {static_cast<Pointer>(index),static_cast<Pointer>(data),trie.indexLength,trie.dataLength,trie.highStart,
        trie.shifted12HighStart,trie.type,trie.valueWidth,trie.reserved32,trie.reserved16,trie.index3NullOffset,
        trie.dataNullOffset,trie.nullValue};
}
static_assert(sizeof(icu_trie::View<std::uint64_t>)==sizeof(UCPTrie));
static_assert(offsetof(icu_trie::View<std::uint64_t>,indexLength)==offsetof(UCPTrie,indexLength));
static_assert(offsetof(icu_trie::View<std::uint64_t>,type)==offsetof(UCPTrie,type));
static_assert(offsetof(icu_trie::View<std::uint64_t>,dataNullOffset)==offsetof(UCPTrie,dataNullOffset));


TrieObject read_binary(const GuestMemory& memory,std::uint64_t address,std::int32_t capacity,
                       UCPTrieType type,UCPTrieValueWidth width,std::int32_t& used,UErrorCode& error){
    TrieObject result;
    if(!address || (address&3) || capacity<=0 || type<UCPTRIE_TYPE_ANY || type>UCPTRIE_TYPE_SMALL ||
       width<UCPTRIE_VALUE_BITS_ANY || width>UCPTRIE_VALUE_BITS_8){
        error=U_ILLEGAL_ARGUMENT_ERROR;return result;
    }
    error=U_INVALID_FORMAT_ERROR;
    if(capacity<static_cast<std::int32_t>(sizeof(UCPTrieHeader)))return result;
    UCPTrieHeader header{};
    memory.read(GuestAddress{address},std::span(reinterpret_cast<std::uint8_t*>(&header),sizeof(header)));
    const auto size=icu_trie::binary_size(header,type,width);
    if(!size || size>static_cast<std::size_t>(capacity))return result;
    result.binary.resize((size+3)/4);
    auto* storage=reinterpret_cast<std::uint8_t*>(result.binary.data());
    std::memcpy(storage,&header,sizeof(header));
    memory.read(GuestAddress{address+sizeof(header)},std::span(storage+sizeof(header),size-sizeof(header)));
    if(!icu_trie::valid_binary({storage,size},type,width))return result;
    error=U_ZERO_ERROR;
    result.owned.reset(ucptrie_openFromBinary(type,width,result.binary.data(),static_cast<std::int32_t>(size),&used,&error));
    return result;
}
}

struct IcuTrieState {
    std::map<std::uint64_t,TrieObject> tries;
    std::map<const UCPTrie*,std::uint64_t> borrowed;
    std::map<std::uint64_t,MutableTrie> builders;
    const UCPTrie* trie(std::uint64_t handle)const{
        const auto found=tries.find(handle);
        return found==tries.end()?nullptr:found->second.get();
    }
    UMutableCPTrie* builder(std::uint64_t handle)const{
        const auto found=builders.find(handle);
        return found==builders.end()?nullptr:found->second.get();
    }
};

std::span<const WindowsServiceDescriptor> icu_trie_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_TRIE_SERVICE(name,count) {"icu.dll",#name,count,0,0,0,true},
#define ICU_TRIE_QUERY ICU_TRIE_SERVICE
#include "icu_trie_services.inc"
#undef ICU_TRIE_QUERY
#undef ICU_TRIE_SERVICE
    };
    return entries;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_trie(GuestState& state,GuestMemory& memory,
        const WindowsServiceDescriptor& service){
    if(!icu_tries_)icu_tries_=std::make_shared<IcuTrieState>();
    auto& objects=*icu_tries_;
    const WindowsCall arg(state,memory,service);
    const auto i=[&](unsigned index){return signed_argument(arg(index));};
    const auto u=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto name=service.symbol;
    const auto install=[&](TrieObject object,std::uint64_t binary)->std::uint64_t{
        const auto* trie=object.get();
        if(!trie)return 0;
        const auto structure=x86_?sizeof(icu_trie::View<std::uint32_t>):sizeof(icu_trie::View<std::uint64_t>);
        const auto index_size=static_cast<std::size_t>(trie->indexLength)*2;
        const auto data_size=static_cast<std::size_t>(trie->dataLength)*icu_trie::value_size(trie->valueWidth);
        const auto data_offset=(structure+index_size+3)&~std::size_t{3};
        const auto handle=allocate_heap(memory,binary?structure:data_offset+data_size);
        if(!handle)return 0;
        try{
            const auto index=binary?binary+sizeof(UCPTrieHeader):handle+structure;
            const auto data=binary?index+index_size:handle+data_offset;
            if(!binary){
                memory.write(GuestAddress{index},icu_bytes(trie->index,static_cast<std::size_t>(trie->indexLength)));
                memory.write(GuestAddress{data},std::span(trie->data.ptr8,data_size));
            }
            if(x86_){
                const auto view=guest_view<std::uint32_t>(*trie,index,data);
                memory.write(GuestAddress{handle},icu_bytes(&view,1));
            }else{
                const auto view=guest_view<std::uint64_t>(*trie,index,data);
                memory.write(GuestAddress{handle},icu_bytes(&view,1));
            }
            objects.tries.emplace(handle,std::move(object));
        }catch(...){
            (void)free_heap(memory,handle,state);throw;
        }
        return handle;
    };
    if(name=="u_getIntPropertyMap"){
        IcuStatus error(memory,arg(1));
        const auto* map=u_getIntPropertyMap(static_cast<UProperty>(i(0)),&error.value);
        const auto* trie=reinterpret_cast<const UCPTrie*>(map); // 隨附版本的 UCPMap 使用同一種唯讀 trie。
        std::uint64_t handle=0;
        if(trie){
            if(const auto old=objects.borrowed.find(trie);old!=objects.borrowed.end())handle=old->second;
            else{
                TrieObject entry;
                entry.borrowed=trie;
                handle=install(std::move(entry),0);
                if(handle)objects.borrowed.emplace(trie,handle);
                else error.value=U_MEMORY_ALLOCATION_ERROR;
            }
        }
        error.commit();return finish(handle);
    }
    if(name=="ucptrie_openFromBinary"){
        IcuStatus error(memory,arg(5));
        if(U_FAILURE(error.value))return finish(0);
        const auto initial=error.value;
        std::int32_t used=0;
        auto object=read_binary(memory,arg(2),i(3),static_cast<UCPTrieType>(i(0)),static_cast<UCPTrieValueWidth>(i(1)),used,error.value);
        if(!object.get()){error.commit();return finish(0);}
        if(arg(4))memory.check(GuestAddress{arg(4)},4,Access::Write);
        const auto handle=install(std::move(object),arg(2));
        if(handle){
            error.value=initial;
            if(arg(4))memory.store(GuestAddress{arg(4)},32,static_cast<std::uint32_t>(used));
        }else error.value=U_MEMORY_ALLOCATION_ERROR;
        error.commit();return finish(handle);
    }
    const auto* trie=objects.trie(arg(0));
    if(name=="ucptrie_toBinary"){
        IcuStatus error(memory,arg(3));
        if(U_FAILURE(error.value))return finish(0);
        if(!trie || (i(2)>0 && (arg(1)&3))){
            error.value=U_ILLEGAL_ARGUMENT_ERROR;error.commit();return finish(0);
        }
        return finish(icu_output<std::uint8_t>(memory,arg(1),i(2),error,IcuOutput::Array,[&](auto* data,auto capacity,auto* status){
            return ucptrie_toBinary(trie,data,capacity,status);
        }));
    }
    if(name=="ucptrie_close"){
        if(const auto found=objects.tries.find(arg(0));found!=objects.tries.end() && found->second.owned){
            (void)free_heap(memory,found->first,state);
            objects.tries.erase(found);
        }
        return finish(0);
    }
    if(name=="ucptrie_get" || name=="ucpmap_get")return finish(trie?ucptrie_get(trie,i(1)):UINT32_MAX);
    if(name=="ucptrie_getType")return finish(trie?static_cast<std::uint32_t>(ucptrie_getType(trie)):UINT32_MAX);
    if(name=="ucptrie_getValueWidth")return finish(trie?static_cast<std::uint32_t>(ucptrie_getValueWidth(trie)):UINT32_MAX);
    if(name=="ucptrie_internalSmallIndex" || name=="ucptrie_internalSmallU8Index"){
        std::uint64_t code=u(1);
        const bool utf8=name.ends_with("U8Index");
        if(utf8)code=(code<<12)|(std::uint64_t{static_cast<std::uint8_t>(arg(2))}<<6)|static_cast<std::uint8_t>(arg(3));
        if(!trie)return finish(UINT32_MAX);
        if(code>=unicode_limit)return finish(static_cast<std::uint32_t>(trie->dataLength-1));
        const auto cp=static_cast<UChar32>(code);
        const auto fast_max=trie->type==UCPTRIE_TYPE_FAST?0xffff:UCPTRIE_SMALL_MAX;
        return finish(static_cast<std::uint32_t>(_UCPTRIE_CP_INDEX(trie,fast_max,cp)));
    }
    if(name=="ucptrie_internalU8PrevIndex"){
        if(!trie || trie->type!=UCPTRIE_TYPE_FAST || arg(3)<arg(2) || u(1)>0xff)return finish(UINT32_MAX);
        std::array<std::uint8_t,7> text{};
        const auto length=static_cast<std::size_t>(std::min<std::uint64_t>(text.size(),arg(3)-arg(2)));
        if(length)memory.read(GuestAddress{arg(3)-length},std::span(text).first(length));
        return finish(static_cast<std::uint32_t>(ucptrie_internalU8PrevIndex(trie,i(1),text.data(),text.data()+length)));
    }
    if(name=="ucptrie_getRange" || name=="ucpmap_getRange"){
        if(!trie)return finish(UINT32_MAX);
        if(arg(4)){
            // 保留原呼叫堆疊，讓同版本 ICU 直接執行客體 filter 與回寫。
            const auto base=icu_trie::image_address(x86_);
            const auto& image=runtime_.builtin_images.at(base);
            state.rip=GuestAddress{base+image.export_names.at(std::string(name))};
            return {};
        }
        std::uint32_t value{};
        const auto end=ucptrie_getRange(trie,i(1),static_cast<UCPMapRangeOption>(i(2)),u(3),nullptr,nullptr,&value);
        if(end>=0 && arg(6))memory.store(GuestAddress{arg(6)},32,value);
        return finish(static_cast<std::uint32_t>(end));
    }

    auto* builder=objects.builder(arg(0));
    if(name=="umutablecptrie_close"){
        if(builder){(void)free_heap(memory,arg(0),state);objects.builders.erase(arg(0));}
        return finish(0);
    }
    if(name=="umutablecptrie_get")return finish(builder?umutablecptrie_get(builder,i(1)):UINT32_MAX);
    if(name=="umutablecptrie_getRange"){
        if(!builder || u(1)>=unicode_limit)return finish(UINT32_MAX);
        const auto option=static_cast<UCPMapRangeOption>(i(2));
        if(!arg(4)){
            std::uint32_t value{};
            const auto end=umutablecptrie_getRange(builder,i(1),option,u(3),nullptr,nullptr,&value);
            if(end>=0 && arg(6))memory.store(GuestAddress{arg(6)},32,value);
            return finish(static_cast<std::uint32_t>(end));
        }
        return nested_continuation(state,memory,"ICU mutable trie 篩選",[this,service](GuestState& s,GuestMemory& m)
                ->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);
            const auto handle=call(0),callback=call(4),context=call(5),output=call(6);
            const auto option=signed_argument(call(2));
            const auto surrogate=static_cast<std::uint32_t>(call(3));
            const auto start=signed_argument(call(1));
            const auto fixed_end=option==UCPMAP_RANGE_NORMAL?-1:(option==UCPMAP_RANGE_FIXED_ALL_SURROGATES?0xdfff:0xdbff);
            std::int32_t end=start-1;
            std::uint32_t selected=0;
            for(auto first=start;first<unicode_limit;first=end+1){
                const auto* current=icu_tries_->builder(handle);
                if(!current)break;
                std::uint32_t value=surrogate;
                auto last=fixed_end;
                if(first<surrogate_first || first>fixed_end){
                    last=umutablecptrie_getRange(current,first,UCPMAP_RANGE_NORMAL,0,nullptr,nullptr,&value);
                    if(first<surrogate_first && fixed_end>=0)last=std::min(last,surrogate_first-1);
                    value=co_await service_callback::call(callback,{context,value});
                }
                if(first==start){
                    selected=value;
                    if(output)m.store(GuestAddress{output},32,selected);
                }else if(value!=selected)break;
                end=last;
            }
            call.return_to(s,static_cast<std::uint32_t>(end>=start?end:-1));
            co_return std::nullopt;
        });
    }
    if(name=="umutablecptrie_set" || name=="umutablecptrie_setRange" || name=="umutablecptrie_buildImmutable"){
        IcuStatus error(memory,arg(name=="umutablecptrie_setRange"?4:3));
        if(U_FAILURE(error.value))return finish(0);
        if(!builder){error.value=U_ILLEGAL_ARGUMENT_ERROR;error.commit();return finish(0);}
        std::uint64_t handle=0;
        if(name=="umutablecptrie_set")umutablecptrie_set(builder,i(1),u(2),&error.value);
        else if(name=="umutablecptrie_setRange")umutablecptrie_setRange(builder,i(1),i(2),u(3),&error.value);
        else {
            TrieObject object;
            object.owned.reset(umutablecptrie_buildImmutable(builder,static_cast<UCPTrieType>(i(1)),
                static_cast<UCPTrieValueWidth>(i(2)),&error.value));
            handle=install(std::move(object),0);
            if(!handle && U_SUCCESS(error.value))error.value=U_MEMORY_ALLOCATION_ERROR;
        }
        error.commit();return finish(handle);
    }
    IcuStatus error(memory,arg(name=="umutablecptrie_open"?2:1));
    if(U_FAILURE(error.value))return finish(0);
    MutableTrie created(nullptr,umutablecptrie_close);
    if(name=="umutablecptrie_open")created.reset(umutablecptrie_open(u(0),u(1),&error.value));
    else if(name=="umutablecptrie_clone" && builder)created.reset(umutablecptrie_clone(builder,&error.value));
    else if(name=="umutablecptrie_fromUCPTrie" && trie)created.reset(umutablecptrie_fromUCPTrie(trie,&error.value));
    else if(name=="umutablecptrie_fromUCPMap" && trie)created.reset(umutablecptrie_fromUCPMap(
        reinterpret_cast<const UCPMap*>(trie),&error.value));
    else error.value=U_ILLEGAL_ARGUMENT_ERROR;
    std::uint64_t handle=0;
    if(created){
        handle=allocate_heap(memory,1);
        if(handle){
            try{objects.builders.emplace(handle,std::move(created));}
            catch(...){(void)free_heap(memory,handle,state);throw;}
        }else error.value=U_MEMORY_ALLOCATION_ERROR;
    }
    error.commit();return finish(handle);
}
}

#include "x64arm/windows_services.hpp"
#include "icu_guest_abi.hpp"
#include "icu_set_operations.h"
#include "icu_set_state.hpp"
#include <type_traits>

namespace x64arm {
namespace {
using namespace icu_abi;
using OwnedSet=OwnedIcuSet;
template<class T> struct IcuInput {
    std::basic_string<T> text;
    const T* data{};
    IcuInput(const GuestMemory& memory,std::uint64_t pointer,std::int32_t length){
        if(!pointer)return;
        if(length<0){
            const auto end=static_cast<std::size_t>(std::min<std::uint64_t>(INT32_MAX,memory.limit()/sizeof(T)));
            if constexpr(sizeof(T)==2)text=read_utf16(memory,GuestAddress{pointer},end);
            else text=read_ansi(memory,pointer,end);
        }else{
            const auto count=static_cast<std::size_t>(length);
            if(count){
                memory.check(GuestAddress{pointer},count*sizeof(T),Access::Read);
                text.resize(count);
                memory.read(GuestAddress{pointer},std::span(reinterpret_cast<std::uint8_t*>(text.data()),count*sizeof(T)));
            }
        }
        data=text.c_str();
    }
};

template<auto Function> struct SetCall;
template<class Result,class... Args,Result(*Function)(Args...)>
struct SetCall<Function> {
    static constexpr unsigned count=sizeof...(Args);
    template<class T>
    static T argument(const IcuSetState& sets,std::uint64_t raw,USet* working){
        if constexpr(std::is_same_v<T,USet*>)return working;
        else if constexpr(std::is_same_v<T,const USet*>)return sets.get(raw);
        else return static_cast<T>(signed_argument(raw));
    }
    template<std::size_t... I>
    static std::uint32_t run(const WindowsCall& call,const IcuSetState& sets,USet* working,std::index_sequence<I...>){
        const auto valid=[](auto value){
            if constexpr(std::is_pointer_v<decltype(value)>)return value!=nullptr;
            else return true;
        };
        if(!(valid(argument<Args>(sets,call(I),working)) && ...)){
            if constexpr(std::is_same_v<Result,std::int32_t> && count==2)return UINT32_MAX;
            return 0;
        }
        if constexpr(std::is_void_v<Result>){
            Function(argument<Args>(sets,call(I),working)...);return 0;
        }else return static_cast<std::uint32_t>(Function(argument<Args>(sets,call(I),working)...));
    }
    static std::uint32_t run(const WindowsCall& call,const IcuSetState& sets,USet* working){
        return run(call,sets,working,std::index_sequence_for<Args...>{});
    }
};

struct SetEntry {
    WindowsServiceDescriptor descriptor;
    std::uint32_t (*scalar)(const WindowsCall&,const IcuSetState&,USet*){};
    void (*text)(USet*,const UChar*,std::int32_t){};
    bool mutating{};
};
constexpr SetEntry entries[]{
#define ICU_SET_READ(symbol) {{"icu.dll",#symbol,SetCall<&symbol>::count,0,0,0,true},SetCall<&symbol>::run},
#define ICU_SET_MUTATE(symbol) {{"icu.dll",#symbol,SetCall<&symbol>::count,0,0,0,true},SetCall<&symbol>::run,nullptr,true},
#define ICU_SET_TEXT_MUTATE(symbol) {{"icu.dll",#symbol,3,0,0,0,true},nullptr,&symbol,true},
#define ICU_SET_SPECIAL(symbol,count) {{"icu.dll",#symbol,count,0,0,0,true}},
#include "icu_set_services.inc"
#undef ICU_SET_READ
#undef ICU_SET_MUTATE
#undef ICU_SET_TEXT_MUTATE
#undef ICU_SET_SPECIAL
};

IcuSetView describe_set(const USet* set,std::vector<IcuSetRange>& ranges){
    IcuSetView view{};
    view.range_count=uset_getRangeCount(set);view.item_count=uset_getItemCount(set);view.size=uset_size(set);
    if(uset_isFrozen(set))view.flags|=ICU_SET_FROZEN;
    if(uset_hasStrings(set))view.flags|=ICU_SET_HAS_STRINGS;
    ranges.reserve(static_cast<std::size_t>(view.range_count));
    std::int32_t index=0;
    for(std::int32_t n=0;n<view.range_count;++n){
        IcuSetRange range{};UErrorCode status=U_ZERO_ERROR;
        uset_getItem(set,n,&range.start,&range.limit,nullptr,0,&status);
        if(U_FAILURE(status))throw HostCallFault("UnicodeSet 無法取得碼點區間");
        ++range.limit;
        range.index=index;index+=range.limit-range.start;ranges.push_back(range);
    }
    return view;
}
}

std::span<const WindowsServiceDescriptor> icu_set_service_descriptors(){
    static const auto descriptors=[] {
        std::array<WindowsServiceDescriptor,std::size(entries)> table{};
        for(std::size_t n=0;n<table.size();++n)table[n]=entries[n].descriptor;
        return table;
    }();
    return descriptors;
}

bool WindowsServices::publish_icu_set(GuestState& state,GuestMemory& memory,std::uint64_t handle,
        IcuSetObject& object,const USet* replacement){
    std::vector<IcuSetRange> ranges;
    auto view=describe_set(replacement,ranges);
    if(!ranges.empty()){
        view.ranges=allocate_heap(memory,ranges.size()*sizeof(IcuSetRange));
        if(!view.ranges)return false;
    }
    try{
        if(view.ranges)memory.write(GuestAddress{view.ranges},icu_bytes(ranges.data(),ranges.size()));
        memory.write(GuestAddress{handle},icu_bytes(&view,1));
    }catch(...){
        if(view.ranges)(void)free_heap(memory,view.ranges,state);
        throw;
    }
    if(const auto previous=std::exchange(object.ranges,view.ranges))(void)free_heap(memory,previous,state);
    return true;
}

std::uint64_t WindowsServices::adopt_icu_set(GuestState& state,GuestMemory& memory,IcuSetObject object){
    if(!object.get())return 0;
    if(!icu_sets_)icu_sets_=std::make_shared<IcuSetState>();
    const auto handle=allocate_heap(memory,sizeof(IcuSetView));
    if(!handle)return 0;
    try{
        if(!publish_icu_set(state,memory,handle,object,object.get())){
            (void)free_heap(memory,handle,state);return 0;
        }
        icu_sets_->objects.emplace(handle,std::move(object));
    }catch(...){
        if(object.ranges)(void)free_heap(memory,object.ranges,state);
        (void)free_heap(memory,handle,state);throw;
    }
    return handle;
}

std::uint64_t WindowsServices::install_icu_set(GuestState& state,GuestMemory& memory,std::uint64_t handle,USet* replacement){
    OwnedSet set(replacement);
    if(!set)return 0;
    if(!handle)return adopt_icu_set(state,memory,{std::move(set),nullptr,0});
    if(!icu_sets_)return 0;
    const auto found=icu_sets_->objects.find(handle);
    if(found==icu_sets_->objects.end() || !found->second.owned ||
       !publish_icu_set(state,memory,handle,found->second,set.get()))return 0;
    found->second.owned=std::move(set);
    return handle;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_set(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);
    const auto name=service.symbol;
    const auto i=[&](unsigned n){return signed_argument(arg(n));};
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    if(!icu_sets_)icu_sets_=std::make_shared<IcuSetState>();
    auto& sets=*icu_sets_;
    const auto publish=[&](std::uint64_t handle,IcuSetObject& object,const USet* replacement){
        return publish_icu_set(state,memory,handle,object,replacement);
    };
    const auto install=[&](IcuSetObject object){return adopt_icu_set(state,memory,std::move(object));};
    const auto mutate=[&](auto operation){
        const auto found=sets.objects.find(arg(0));
        if(found==sets.objects.end() || !found->second.owned || uset_isFrozen(found->second.get()))return true;
        auto& object=found->second;
        OwnedSet working(uset_clone(object.get()));
        if(!working)return false;
        operation(working.get());
        if(!publish(found->first,object,working.get()))return false;
        object.owned=std::move(working);
        return true;
    };
    const auto translated=std::ranges::find_if(entries,[&](const auto& entry){return entry.descriptor.symbol==name;});
    if(translated==std::end(entries))throw HostCallFault("未知 UnicodeSet 服務入口");
    if(translated->mutating){
        if(translated->text){
            if(i(2)<-1 || (!arg(1) && i(2)))return finish(0);
            const IcuInput<UChar> input(memory,arg(1),i(2));
            (void)mutate([&](USet* set){translated->text(set,input.data,i(2));});
        }else (void)mutate([&](USet* set){translated->scalar(arg,sets,set);});
        return finish(0);
    }
    if(translated->scalar){
        if(name=="uset_isEmpty" && !sets.get(arg(0)))return finish(1);
        return finish(translated->scalar(arg,sets,nullptr));
    }
    if(name=="uset_openEmpty" || name=="uset_open")
        return finish(install({OwnedSet(name=="uset_openEmpty"?uset_openEmpty():uset_open(i(0),i(1))),nullptr,0}));
    if(name=="uset_clone" || name=="uset_cloneAsThawed"){
        const auto* set=sets.get(arg(0));
        if(!set)return finish(0);
        return finish(install({OwnedSet(name=="uset_clone"?uset_clone(set):uset_cloneAsThawed(set)),nullptr,0}));
    }
    if(name=="uset_close"){
        const auto found=sets.objects.find(arg(0));
        if(found!=sets.objects.end() && found->second.owned){
            if(found->second.ranges)(void)free_heap(memory,found->second.ranges,state);
            (void)free_heap(memory,found->first,state);
            sets.objects.erase(found);
        }
        return finish(0);
    }
    if(name=="u_getBinaryPropertySet"){
        IcuStatus status(memory,arg(1));
        if(U_FAILURE(status.value))return finish(0);
        const auto* set=u_getBinaryPropertySet(static_cast<UProperty>(i(0)),&status.value);
        std::uint64_t handle=0;
        if(set){
            if(const auto found=sets.borrowed.find(set);found!=sets.borrowed.end())handle=found->second;
            else {
                handle=install({{},set,0});
                if(handle)sets.borrowed.emplace(set,handle);
                else status.value=U_MEMORY_ALLOCATION_ERROR;
            }
        }
        status.commit();return finish(handle);
    }
    if(name=="uset_openPattern" || name=="uset_openPatternOptions"){
        const bool options=name=="uset_openPatternOptions";
        IcuStatus status(memory,arg(options?3U:2U));
        if(U_FAILURE(status.value))return finish(0);
        if(i(1)<-1 || !arg(0)){status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();return finish(0);}
        const IcuInput<UChar> pattern(memory,arg(0),i(1));
        OwnedSet set(options?uset_openPatternOptions(pattern.data,i(1),static_cast<std::uint32_t>(arg(2)),&status.value):
                         uset_openPattern(pattern.data,i(1),&status.value));
        const auto handle=U_SUCCESS(status.value)?install({std::move(set),nullptr,0}):0;
        if(!handle && U_SUCCESS(status.value))status.value=U_MEMORY_ALLOCATION_ERROR;
        status.commit();return finish(handle);
    }
    if(name=="uset_applyPattern" || name=="uset_applyIntPropertyValue" || name=="uset_applyPropertyAlias"){
        const bool pattern=name=="uset_applyPattern";
        const bool aliases=name=="uset_applyPropertyAlias";
        IcuStatus status(memory,arg(pattern?4U:aliases?5U:3U));
        if(U_FAILURE(status.value))return finish(0);
        const auto* set=sets.get(arg(0));
        if(!set || (pattern && (!arg(1) || i(2)<-1)) ||
           (aliases && ((!arg(1) && i(2)) || i(2)<-1 || (!arg(3) && i(4)) || i(4)<-1))){
            status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();return finish(0);
        }
        std::uint32_t length=0;
        const auto changed=mutate([&](USet* value){
            if(pattern){
                const IcuInput<UChar> input(memory,arg(1),i(2));
                length=static_cast<std::uint32_t>(uset_applyPattern(value,input.data,i(2),static_cast<std::uint32_t>(arg(3)),&status.value));
            }else if(aliases){
                const IcuInput<UChar> property(memory,arg(1),i(2)),alias(memory,arg(3),i(4));
                uset_applyPropertyAlias(value,property.data,i(2),alias.data,i(4),&status.value);
            }else uset_applyIntPropertyValue(value,static_cast<UProperty>(i(1)),i(2),&status.value);
        });
        if(!changed)status.value=U_MEMORY_ALLOCATION_ERROR;
        status.commit();return finish(length);
    }
    if(name=="uset_resemblesPattern"){
        if(!arg(0) || i(1)<-1 || i(2)<0)return finish(0);
        const IcuInput<UChar> pattern(memory,arg(0),i(1));
        return finish(uset_resemblesPattern(pattern.data,i(1),i(2))!=0);
    }
    const auto* set=sets.get(arg(0));
    if(name=="uset_containsString" || name=="uset_containsAllCodePoints" || name.starts_with("uset_span")){
        if(!set || i(2)<-1 || (!arg(1) && i(2)))return finish(0);
        const bool span=name.starts_with("uset_span");
        const auto condition=span?static_cast<USetSpanCondition>(i(3)):USET_SPAN_NOT_CONTAINED;
        if(span && static_cast<unsigned>(condition)>=USET_SPAN_CONDITION_COUNT)return finish(0);
        if(name.ends_with("UTF8")){
            const IcuInput<char> input(memory,arg(1),i(2));
            const auto count=name=="uset_spanUTF8"?uset_spanUTF8(set,input.data,i(2),condition):
                uset_spanBackUTF8(set,input.data,i(2),condition);
            return finish(static_cast<std::uint32_t>(count));
        }
        const IcuInput<UChar> input(memory,arg(1),i(2));
        if(name=="uset_containsAllCodePoints")return finish(uset_containsAllCodePoints(set,input.data,i(2))!=0);
        if(name=="uset_containsString")return finish(uset_containsString(set,input.data,i(2))!=0);
        return finish(static_cast<std::uint32_t>(name=="uset_span"?uset_span(set,input.data,i(2),condition):
            uset_spanBack(set,input.data,i(2),condition)));
    }
    if(name=="uset_toPattern" || name=="uset_serialize"){
        const bool pattern=name=="uset_toPattern";
        IcuStatus status(memory,arg(pattern?4U:3U));
        if(U_FAILURE(status.value))return finish(0);
        if(!set){status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();return finish(0);}
        if(pattern)return finish(icu_output<UChar>(memory,arg(1),i(2),status,IcuOutput::String,[&](auto* output,auto capacity,auto* error){
            return uset_toPattern(set,output,capacity,static_cast<UBool>(arg(3)),error);
        }));
        return finish(icu_output<std::uint16_t>(memory,arg(1),i(2),status,IcuOutput::Array,[&](auto* output,auto capacity,auto* error){
            return uset_serialize(set,output,capacity,error);
        }));
    }
    if(name=="uset_getItem"){
        IcuStatus status(memory,arg(6));
        if(U_FAILURE(status.value))return finish(0);
        if(!set){status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();return finish(UINT32_MAX);}
        const auto index=i(1);
        UChar32 start=0,end=0;
        if(index>=0 && index<uset_getRangeCount(set)){
            if(!arg(2) || !arg(3)){status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();return finish(UINT32_MAX);}
            uset_getItem(set,index,&start,&end,nullptr,0,&status.value);
            memory.store(GuestAddress{arg(2)},32,static_cast<std::uint32_t>(start));
            memory.store(GuestAddress{arg(3)},32,static_cast<std::uint32_t>(end));
            status.commit();return finish(0);
        }
        return finish(icu_output<UChar>(memory,arg(4),i(5),status,IcuOutput::String,[&](auto* output,auto capacity,auto* error){
            return uset_getItem(set,index,&start,&end,output,capacity,error);
        }));
    }
    // USerializedSet 的實際指標寬度與內嵌 16 位元儲存隨客體 ABI。
    const auto pointer_bits=static_cast<std::uint8_t>(x86_?32:64);
    const auto unit=static_cast<unsigned>(pointer_bits/8);
    const auto serialized_at=arg(0);
    const auto store_serialized=[&](const USerializedSet& data,std::uint64_t chars){
        if(chars)memory.store(GuestAddress{serialized_at},pointer_bits,chars);
        memory.store(GuestAddress{serialized_at+unit},32,static_cast<std::uint32_t>(data.bmpLength));
        memory.store(GuestAddress{serialized_at+unit+4},32,static_cast<std::uint32_t>(data.length));
    };
    if(!serialized_at)return finish(0);
    if(name=="uset_setSerializedToOne"){
        USerializedSet serialized{};
        uset_setSerializedToOne(&serialized,i(1));
        if(serialized.array){
            const auto at=serialized_at+unit+8;
            memory.write(GuestAddress{at},icu_bytes(serialized.staticArray,static_cast<std::size_t>(serialized.length)));
            store_serialized(serialized,at);
        }
        return finish(0);
    }
    if(name=="uset_getSerializedSet"){
        USerializedSet serialized{};
        const auto source=arg(1);
        const auto length=i(2);
        if(!source || length<=0){store_serialized(serialized,0);return finish(0);}
        const auto word=static_cast<std::uint16_t>(memory.load(GuestAddress{source},16));
        const auto header=(word&0x8000)?2U:1U;
        const auto size=header+(word&0x7fffU);
        if(static_cast<std::uint32_t>(length)<size){store_serialized(serialized,0);return finish(0);}
        std::vector<std::uint16_t> copy(size);
        memory.read(GuestAddress{source},std::span(reinterpret_cast<std::uint8_t*>(copy.data()),copy.size()*2));
        const auto valid=uset_getSerializedSet(&serialized,copy.data(),static_cast<std::int32_t>(copy.size())) &&
            serialized.bmpLength<=serialized.length && !((serialized.length-serialized.bmpLength)&1);
        if(!valid)serialized.length=serialized.bmpLength=0;
        store_serialized(serialized,valid?source+header*2:0);
        return finish(valid);
    }
    USerializedSet serialized{};
    serialized.bmpLength=signed_argument(memory.load(GuestAddress{serialized_at+unit},32));
    serialized.length=signed_argument(memory.load(GuestAddress{serialized_at+unit+4},32));
    if(serialized.length<0 || serialized.length>0x7fff || serialized.bmpLength<0 || serialized.bmpLength>serialized.length ||
       ((serialized.length-serialized.bmpLength)&1))return finish(0);
    const auto count=uset_getSerializedRangeCount(&serialized);
    if(name=="uset_getSerializedRangeCount")return finish(static_cast<std::uint32_t>(count));
    if(name=="uset_getSerializedRange" && (i(1)<0 || i(1)>=count || !arg(2) || !arg(3)))return finish(0);
    const auto address=memory.load(GuestAddress{serialized_at},pointer_bits);
    if(!address && serialized.length)return finish(0);
    std::vector<std::uint16_t> copy(static_cast<std::size_t>(serialized.length));
    if(!copy.empty())memory.read(GuestAddress{address},std::span(reinterpret_cast<std::uint8_t*>(copy.data()),copy.size()*2));
    serialized.array=copy.data();
    if(name=="uset_serializedContains")return finish(uset_serializedContains(&serialized,i(1))!=0);
    UChar32 start=0,end=0;
    if(!uset_getSerializedRange(&serialized,i(1),&start,&end))return finish(0);
    memory.store(GuestAddress{arg(2)},32,static_cast<std::uint32_t>(start));
    memory.store(GuestAddress{arg(3)},32,static_cast<std::uint32_t>(end));
    return finish(1);
}
}

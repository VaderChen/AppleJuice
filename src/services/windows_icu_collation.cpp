#include "x64arm/windows_services.hpp"
#include "ansi_codepage.hpp"
#include "icu_break_state.hpp"
#include "icu_collation_binary.hpp"
#include "icu_collation_binary_reader.hpp"
#include "icu_collation_iterator.hpp"
#include "icu_collation_key_buffer.hpp"
#include "icu_collation_key_math.hpp"
#include "icu_guest_abi.hpp"
#include "icu_set_state.hpp"
#include "service_callback_task.hpp"
#include <map>
#include <memory>
#include <collation.h>
#include <unicode/ucol.h>
#include <unicode/ucoleitr.h>
#include <unicode/uenum.h>
#include <unicode/usearch.h>

// ICU 排序規則與字串搜尋（.NET 的 System.Globalization 以 icu.dll 取得這些入口做字串比較、排序鍵與
// 文化資料）。物件建立在宿主 ICU；客體 handle 是客體 heap 上的小區塊位址，宿主以表對應原生物件。ICU 保留指標的輸入
// （collation elements／string search 的文字與樣式）複製到物件自己的穩定緩衝；回傳 const 指標的查詢（規則、資源字串、鍵名）
// 複製到客體 heap，生命期跟隨物件。
namespace x64arm {
namespace {
using namespace icu_abi;

template<class T, auto Close>
struct IcuCloser {
    void operator()(T* object)const{Close(object);}
};
template<class T, auto Close>
using IcuHandle=std::unique_ptr<T,IcuCloser<T,Close>>;
using Text=std::shared_ptr<const std::u16string>;
using CollatorProvider=std::shared_ptr<UCollator>;

Text read_collation_text(const GuestMemory& memory,std::uint64_t address,std::int32_t length,UErrorCode& error){
    const IcuTextInput<UChar> input(memory,address,length,error);
    if(U_FAILURE(error))return {};
    const auto* data=input.get();
    if(!data)return std::make_shared<const std::u16string>();
    return std::make_shared<const std::u16string>(data,length<0?std::char_traits<UChar>::length(data):static_cast<std::size_t>(length));
}

std::shared_ptr<const void> retain_search_text(const icu_break::BreakObject& iterator,const Text& text){
    if(!iterator.text || iterator.text==text)return text;
    return std::make_shared<const std::pair<std::shared_ptr<const void>,Text>>(iterator.text,text);
}

std::vector<std::uint8_t> read_key(const GuestMemory& memory,std::uint64_t address,std::int32_t length){
    if(!address || length< -1 || !length)return {};
    if(length<0){
        const auto text=read_ansi(memory,address,INT32_MAX-4);
        const auto bytes=icu_bytes(text.c_str(),text.size()+1);
        return {bytes.begin(),bytes.end()};
    }
    memory.check(GuestAddress{address},static_cast<std::size_t>(length),Access::Read);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    memory.read(GuestAddress{address},bytes);
    return bytes;
}
}

struct IcuCollationState {
    struct Collator {CollatorProvider native;std::uint64_t rules{},search_owner{};};
    struct Elements {Text text;CollatorProvider collator;IcuHandle<UCollationElements,ucol_closeElements> native;};
    struct Search {
        Text pattern,text;
        CollatorProvider collator;
        IcuHandle<UStringSearch,usearch_close> native;
        std::uint64_t pattern_address{},text_address{},collator_handle{},break_handle{};
        bool ready{true};
    };
    std::map<std::uint64_t,Collator> collators;
    std::map<std::uint64_t,Elements> elements;
    std::map<std::uint64_t,Search> searches;
};

std::span<const WindowsServiceDescriptor> icu_collation_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_COLL_API(name,count,legacy) {"icu.dll",#name,count,0,0,0,true},
#define ICU_COLL_RESIDENT ICU_COLL_API
#include "icu_collation_services.inc"
#undef ICU_COLL_RESIDENT
#undef ICU_COLL_API
    };
    return entries;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_collation(GuestState& state,GuestMemory& memory,
        const WindowsServiceDescriptor& service){
    const auto name=service.symbol;
    if(!icu_collation_)icu_collation_=std::make_shared<IcuCollationState>();
    if(name.starts_with("usearch_"))return invoke_icu_string_search(state,memory,service);
    if(name=="ucol_strcollIter" || name=="ucol_nextSortKeyPart")
        return nested_continuation(state,memory,"ICU 排序字元迭代器",
            [this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                using namespace icu_collation_iterator;
                const WindowsCall call(s,m,service);
                IcuStatus status(m,call(service.arguments-1));
                const auto initial=status.value;
                const auto done=[&](std::int32_t value=0){
                    if(status.value!=initial)status.commit();
                    call.return_to(s,static_cast<std::uint32_t>(value));
                    return std::optional<std::uint32_t>{};
                };
                const auto invalid=[&](){status.value=U_ILLEGAL_ARGUMENT_ERROR;return done();};
                if(U_FAILURE(initial))co_return done();
                const bool comparison=service.symbol=="ucol_strcollIter";
                const auto handle=call(0),first=call(1),other=call(2);
                const auto found=icu_collation_->collators.find(handle);
                if(!first || !other || found==icu_collation_->collators.end())co_return invalid();
                if(comparison && first==other)co_return done(UCOL_EQUAL);
                try{
                    std::int32_t count{};
                    std::array<std::uint32_t,2> offsets{};
                    std::vector<std::uint8_t> bytes;
                    std::uint64_t destination{};
                    if(!comparison){
                        count=signed_argument(call(4));
                        destination=call(3);
                        if(count<0 || (count && !destination))co_return invalid();
                        if(!count)co_return done();
                        m.check(GuestAddress{other},sizeof(offsets),Access::Read);
                        m.check(GuestAddress{other},sizeof(offsets),Access::Write);
                        m.check(GuestAddress{destination},static_cast<std::size_t>(count),Access::Write);
                        read_icu_record(m,other,offsets);
                        if(offsets[0]>icu::Collation::ZERO_LEVEL || offsets[1]>INT32_MAX)co_return invalid();
                        bytes.resize(static_cast<std::size_t>(count));
                    }

                    // 回呼可重新進入排序服務。以複本隔離本次計算和原客體排序器的生命週期。
                    const auto source=found->second.native;
                    IcuHandle<UCollator,ucol_close> provider(ucol_clone(source.get(),&status.value));
                    if(!provider || U_FAILURE(status.value))co_return done();
                    std::int32_t result{};
                    auto native_status=status.value;
                    CallbackJob job;
                    job.start([&]{
                        Iterator left(job,first);
                        if(comparison){
                            Iterator right(job,other);
                            result=ucol_strcollIter(provider.get(),left.get(),right.get(),&native_status);
                        }else result=ucol_nextSortKeyPart(provider.get(),left.get(),offsets.data(),bytes.data(),count,&native_status);
                    });
                    const bool win32=m.execution_mode()==ExecutionMode::X86;
                    const auto bits=static_cast<std::uint8_t>(win32?32:64);
                    while(const auto request=job.wait()){
                        const auto offset=callback_offset(win32,request->callback);
                        if(request->iterator>UINT64_MAX-offset)co_return invalid();
                        const auto target=m.load(GuestAddress{request->iterator+offset},bits);
                        if(!target)co_return invalid();
                        std::vector<std::uint64_t> arguments{request->iterator};
                        if(request->callback==Callback::Move)arguments.push_back(request->delta);
                        if(request->callback==Callback::Move || request->callback==Callback::Index)arguments.push_back(request->origin);
                        const auto raw=co_await service_callback::call_value(target,std::move(arguments));
                        const auto boolean=request->callback==Callback::HasNext || request->callback==Callback::HasPrevious;
                        const auto value=signed_argument(boolean?raw&0xffU:raw);
                        if(!valid_reply(request->callback,value))co_return invalid();
                        job.reply(value);
                    }
                    status.value=native_status;
                    if(!comparison && U_SUCCESS(status.value)){
                        m.check(GuestAddress{other},sizeof(offsets),Access::Write);
                        m.check(GuestAddress{destination},bytes.size(),Access::Write);
                        write_icu_record(m,other,offsets);
                        m.write(GuestAddress{destination},bytes);
                    }
                    co_return done(result);
                }catch(const std::bad_alloc&){status.value=U_MEMORY_ALLOCATION_ERROR;co_return done();}
                catch(const std::system_error&){status.value=U_MEMORY_ALLOCATION_ERROR;co_return done();}
            });
    if(name=="ucol_openAvailableLocales" || name=="ucol_getKeywords" || name=="ucol_getKeywordValues" ||
       name=="ucol_getKeywordValuesForLocale")
        return nested_continuation(state,memory,"ICU 排序規則列舉",
            [this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);
                IcuStatus status(m,call(service.arguments-1));
                const auto initial=status.value;
                std::uint64_t handle{};
                try{
                    if(U_SUCCESS(status.value)){
                        IcuHandle<UEnumeration,uenum_close> values;
                        const auto symbol=service.symbol;
                        if(symbol=="ucol_getKeywords")values.reset(ucol_getKeywords(&status.value));
                        else if(symbol=="ucol_openAvailableLocales")values.reset(ucol_openAvailableLocales(&status.value));
                        else if(!call(0))status.value=U_ILLEGAL_ARGUMENT_ERROR;
                        else{
                            const auto keyword=read_ansi(m,call(0));
                            if(symbol=="ucol_getKeywordValues")values.reset(ucol_getKeywordValues(keyword.c_str(),&status.value));
                            else{
                                const auto locale=call(1)?read_ansi(m,call(1)):std::string{};
                                values.reset(ucol_getKeywordValuesForLocale(keyword.c_str(),call(1)?locale.c_str():icu_default_locale(),
                                                                             (call(2)&0xffU)!=0,&status.value));
                            }
                        }
                        if(values && U_SUCCESS(status.value))handle=co_await export_icu_enumeration(s,m,*values,status);
                    }
                }catch(const std::bad_alloc&){status.value=U_MEMORY_ALLOCATION_ERROR;}
                if(status.value!=initial)status.commit();
                call.return_to(s,handle);
                co_return std::nullopt;
            });
    const WindowsCall call(state,memory,service);
    const auto i=[&](unsigned n){return signed_argument(call(n));};
    const auto finish=[&](std::uint64_t result){call.return_to(state,result);return std::optional<std::uint32_t>{};};
    const auto finish_int=[&](std::int64_t value){return finish(static_cast<std::uint32_t>(static_cast<std::int32_t>(value)));};
    auto& objects=*icu_collation_;
    const auto guest_copy=[&](const void* data,std::size_t bytes)->std::uint64_t {
        const auto address=allocate_heap(memory,std::max<std::size_t>(bytes,1));
        if(address && bytes)memory.write(GuestAddress{address},{static_cast<const std::uint8_t*>(data),bytes});
        return address;};
    const auto collator=[&](unsigned n)->UCollator* {const auto found=objects.collators.find(call(n));return found==objects.collators.end()?nullptr:found->second.native.get();};
    const auto elements=[&](unsigned n)->IcuCollationState::Elements* {const auto found=objects.elements.find(call(n));return found==objects.elements.end()?nullptr:&found->second;};
    // 讀客體 UTF-16（長度 -1 為以 NUL 結尾）到穩定緩衝。
    const auto load_text=[&](std::uint64_t address,std::int32_t length,UErrorCode& error)->Text {
        return read_collation_text(memory,address,length,error);};
    const auto ascii=[&](std::uint64_t address)->std::optional<std::string>{
        if(!address)return std::nullopt;return read_ansi(memory,address,1024);};

    // ---- 沒有 UErrorCode 的入口 ----
    if(name=="u_uastrncpy"){
        // 預設碼頁（Windows 的 icu.dll 為系統 ANSI 碼頁）位元組字串轉為 UTF-16；不足 n 個字元時補 NUL 結尾，滿 n 個不補。
        const auto destination=call(0),source=call(1);const auto limit=i(2);
        if(!destination || limit<=0)return finish(destination);
        std::string bytes;
        for(std::int32_t n=0;n<limit;++n){const auto byte=static_cast<char>(memory.load(GuestAddress{source+static_cast<std::uint64_t>(n)},8));if(!byte)break;bytes.push_back(byte);}
        auto text=ansi_to_utf16_lossy(bytes);
        if(text.size()>static_cast<std::size_t>(limit))text.resize(static_cast<std::size_t>(limit));
        if(text.size()<static_cast<std::size_t>(limit))text.push_back(u'\0');
        memory.write(GuestAddress{destination},icu_bytes(text.data(),text.size()));
        return finish(destination);
    }
    const auto close=[&](auto& map){
        const auto found=map.find(call(0));
        if(found!=map.end()){
            if constexpr(std::is_same_v<std::decay_t<decltype(map)>,decltype(objects.collators)>){
                if(found->second.search_owner)return finish(0);
                if(found->second.rules)(void)free_heap(memory,found->second.rules,state);
            }
            map.erase(found);(void)free_heap(memory,call(0),state);}
        return finish(0);};
    if(name=="ucol_close")return close(objects.collators);
    if(name=="ucol_closeElements")return close(objects.elements);
    if(name=="ucol_countAvailable")return finish_int(ucol_countAvailable());
    if(name=="ucol_getAvailable"){
        const auto index=i(0);
        return finish(index>=0 && index<ucol_countAvailable()?publish_icu_literal(memory,ucol_getAvailable(index)):0);
    }
    if(name=="ucol_primaryOrder")return finish(icu_collation_key::primary(static_cast<std::uint32_t>(call(0))));
    if(name=="ucol_secondaryOrder")return finish(icu_collation_key::secondary(static_cast<std::uint32_t>(call(0))));
    if(name=="ucol_tertiaryOrder")return finish(icu_collation_key::tertiary(static_cast<std::uint32_t>(call(0))));
    if(name=="ucol_keyHashCode"){
        const auto address=call(0);
        const auto length=address && i(1)>0?static_cast<std::uint32_t>(i(1)):0U;
        if(length)memory.check(GuestAddress{address},length,Access::Read);
        return finish(icu_collation_key::hash(length,[&](std::uint32_t offset){
            return static_cast<std::uint32_t>(memory.load(GuestAddress{address+offset},8));
        }));
    }
    if(name=="ucol_getStrength"){const auto* c=collator(0);return finish_int(c?ucol_getStrength(c):UCOL_DEFAULT);}
    if(name=="ucol_setStrength"){if(auto* c=collator(0))ucol_setStrength(c,static_cast<UCollationStrength>(i(1)));return finish(0);}
    if(name=="ucol_getMaxVariable"){const auto* c=collator(0);return finish_int(c?ucol_getMaxVariable(c):UCOL_REORDER_CODE_PUNCTUATION);}
    if(name=="ucol_getOffset"){const auto* e=elements(0);return finish_int(e?ucol_getOffset(e->native.get()):0);}
    if(name=="ucol_reset"){if(auto* e=elements(0))ucol_reset(e->native.get());return finish(0);}
    if(name=="ucol_getMaxExpansion"){const auto* e=elements(0);return finish_int(e?ucol_getMaxExpansion(e->native.get(),i(1)):1);}
    if(name=="ucol_getVersion" || name=="ucol_getUCAVersion"){
        UVersionInfo version{};
        if(const auto* c=collator(0)){
            if(name=="ucol_getUCAVersion")ucol_getUCAVersion(c,version);
            else ucol_getVersion(c,version);
        }
        if(call(1))memory.write(GuestAddress{call(1)},icu_bytes(version,U_MAX_VERSION_LENGTH));
        return finish(0);
    }
    if(name=="ucol_getRules"){
        auto found=objects.collators.find(call(0));
        if(found==objects.collators.end()){if(call(1))memory.store(GuestAddress{call(1)},32,0);return finish(0);}
        std::int32_t length=0;const auto* rules=ucol_getRules(found->second.native.get(),&length);
        if(!found->second.rules){
            std::u16string copy(rules?rules:u"",static_cast<std::size_t>(std::max(length,0)));copy.push_back(u'\0');
            found->second.rules=guest_copy(copy.data(),copy.size()*sizeof(UChar));
        }
        if(call(1))memory.store(GuestAddress{call(1)},32,static_cast<std::uint32_t>(length));
        return finish(found->second.rules);
    }
    if(name=="ucol_getRulesEx"){
        const auto* c=collator(0);
        if(!c || (i(1)!=UCOL_TAILORING_ONLY && i(1)!=UCOL_FULL_RULES))return finish(0);
        const auto mode=static_cast<UColRuleOption>(i(1));
        const auto needed=ucol_getRulesEx(c,mode,nullptr,0);
        const auto capacity=i(3);
        if(call(2) && capacity>0 && capacity>=needed && needed>=0){
            const auto size=needed+(capacity>needed?1:0);
            std::vector<UChar> rules(static_cast<std::size_t>(size));
            const auto result=ucol_getRulesEx(c,mode,rules.data(),size);
            if(result==needed && !rules.empty())memory.write(GuestAddress{call(2)},icu_bytes(rules.data(),rules.size()));
        }
        return finish_int(needed);
    }
    if(name=="ucol_mergeSortkeys"){
        const auto output=call(4);
        const auto capacity=i(5);
        const auto invalid=[&](){if(output && capacity>0)memory.store(GuestAddress{output},8,0);return finish(0);};
        if(capacity<0 || (capacity && !output))return invalid();
        const auto first=read_key(memory,call(0),i(1)),second=read_key(memory,call(2),i(3));
        if(first.empty() || second.empty() || first.back() || second.back() || first.size()+second.size()>INT32_MAX)
            return invalid();
        const auto needed=static_cast<std::int32_t>(first.size()+second.size());
        if(capacity<needed)return finish_int(needed);
        std::vector<std::uint8_t> merged(static_cast<std::size_t>(needed));
        const auto length=ucol_mergeSortkeys(first.data(),static_cast<std::int32_t>(first.size()),second.data(),
            static_cast<std::int32_t>(second.size()),merged.data(),needed);
        if(length>0)memory.write(GuestAddress{output},std::span(merged).first(static_cast<std::size_t>(length)));
        return finish_int(length);
    }
    if(name=="ucol_strcoll" || name=="ucol_equal" || name=="ucol_greater" || name=="ucol_greaterOrEqual"){
        UErrorCode error=U_ZERO_ERROR;
        const auto source=load_text(call(1),i(2),error),target=load_text(call(3),i(4),error);
        const auto* c=collator(0);
        if(!c || U_FAILURE(error))return finish_int(name=="ucol_strcoll"?UCOL_EQUAL:0);
        const auto result=ucol_strcoll(c,source->data(),static_cast<std::int32_t>(source->size()),target->data(),static_cast<std::int32_t>(target->size()));
        if(name=="ucol_strcoll")return finish_int(result);
        if(name=="ucol_equal")return finish(result==UCOL_EQUAL);
        if(name=="ucol_greater")return finish(result==UCOL_GREATER);
        return finish(result!=UCOL_LESS);
    }
    if(name=="ucol_getSortKey"){
        UErrorCode error=U_ZERO_ERROR;const auto source=load_text(call(1),i(2),error);const auto* c=collator(0);
        const auto capacity=i(4);
        if(!c || U_FAILURE(error) || capacity<0 || (capacity && !call(3)))return finish(0);
        const auto needed=ucol_getSortKey(c,source->data(),static_cast<std::int32_t>(source->size()),nullptr,0);
        if(capacity && needed>0){
            std::vector<std::uint8_t> key(static_cast<std::size_t>(needed));
            (void)ucol_getSortKey(c,source->data(),static_cast<std::int32_t>(source->size()),key.data(),needed);
            memory.write(GuestAddress{call(3)},std::span<const std::uint8_t>(key).first(static_cast<std::size_t>(std::min(capacity,needed))));
        }
        return finish_int(needed);
    }
    // ---- 帶 UErrorCode（最後一個參數）的入口：進入時已失敗就不動作 ----
    IcuStatus status(memory,call(service.arguments-1));
    const auto done=[&](std::uint64_t value){status.commit();return finish(value);};
    const auto done_int=[&](std::int64_t value){status.commit();return finish_int(value);};
    if(U_FAILURE(status.value)){
        if(name=="ucol_next" || name=="ucol_previous")return finish_int(UCOL_NULLORDER);
        return finish(0);
    }
    const auto adopt=[&](auto& map,auto object)->std::uint64_t {
        const auto handle=allocate_heap(memory,16);
        if(!handle){status.value=U_MEMORY_ALLOCATION_ERROR;return 0;}
        try{map.emplace(handle,std::move(object));}
        catch(...){(void)free_heap(memory,handle,state);throw;}
        return handle;};
    const auto missing=[&](std::uint64_t value=0){status.value=U_ILLEGAL_ARGUMENT_ERROR;return done(value);};
    if(name=="ucol_openBinary"){
        const auto base=objects.collators.find(call(2));
        if(base==objects.collators.end())return missing();
        auto native=icu_collation_binary::open(memory,call(0),i(1),base->second.native,status.value);
        return done(native?adopt(objects.collators,IcuCollationState::Collator{std::move(native)}):0);
    }
    if(name=="ucol_cloneBinary"){
        const auto address=call(1);
        const auto result=icu_collation_binary::clone(collator(0),address,i(2),status.value,
            [&](std::size_t offset,std::span<const std::uint8_t> bytes){
                if(offset>UINT64_MAX-address)throw HostCallFault("ICU 排序映像位址溢位");
                memory.write(GuestAddress{address+offset},bytes);
            });
        return done_int(result);
    }
    if(name=="ucol_getBound"){
        const auto length=i(1),mode=i(2),capacity=i(5);
        if(length< -1 || length>INT32_MAX-4 || mode<UCOL_BOUND_LOWER || mode>UCOL_BOUND_UPPER_LONG ||
           capacity<0 || (capacity && !call(4)))return missing();
        const auto result=icu_collation_key::make_bound(read_key(memory,call(0),length),length,
            static_cast<UColBoundMode>(mode),static_cast<std::uint32_t>(call(3)),capacity,status.value);
        if(!result.bytes.empty())memory.write(GuestAddress{call(4)},result.bytes);
        return done_int(result.required);
    }
    if(name=="ucol_getDisplayName"){
        const auto object=ascii(call(0)),display=ascii(call(1));
        return finish(icu_preserving_output<UChar>(memory,call(2),i(3),status,
            [&](auto* output,auto count,auto* error){return ucol_getDisplayName(object?object->c_str():icu_default_locale(),
                display?display->c_str():icu_default_locale(),output,count,error);}));
    }
    if(name=="ucol_getFunctionalEquivalent"){
        const auto keyword=ascii(call(2)),locale=ascii(call(3));
        if(!keyword)return missing();
        UBool available{};
        read_icu_record(memory,call(4),available);
        const auto length=icu_preserving_output<char>(memory,call(0),i(1),status,[&](auto* output,auto count,auto* error){
            return ucol_getFunctionalEquivalent(output,count,keyword->c_str(),locale?locale->c_str():icu_default_locale(),
                                                 call(4)?&available:nullptr,error);
        });
        write_icu_record(memory,call(4),available);
        return finish(length);
    }
    if(name=="ucol_getEquivalentReorderCodes" || name=="ucol_getReorderCodes"){
        const bool equivalent=name=="ucol_getEquivalentReorderCodes";
        const auto* c=equivalent?nullptr:collator(0);
        if(!equivalent && !c)return missing();
        return finish(icu_output<std::int32_t>(memory,call(1),i(2),status,IcuOutput::Array,
            [&](auto* output,auto count,auto* error){return equivalent?ucol_getEquivalentReorderCodes(i(0),output,count,error):
                                                                      ucol_getReorderCodes(c,output,count,error);}));
    }
    if(name=="ucol_setReorderCodes"){
        auto* c=collator(0);
        const auto length=i(2);
        if(!c || length<0 || (length && !call(1)))return missing();
        const auto size=static_cast<std::size_t>(length)*sizeof(std::int32_t);
        if(size)memory.check(GuestAddress{call(1)},size,Access::Read);
        std::vector<std::int32_t> codes(static_cast<std::size_t>(length));
        if(size)memory.read(GuestAddress{call(1)},{reinterpret_cast<std::uint8_t*>(codes.data()),size});
        ucol_setReorderCodes(c,codes.data(),length,&status.value);
        return done(0);
    }
    if(name=="ucol_getLocaleByType"){
        const auto* c=collator(0);
        if(!c)return missing();
        return done(publish_icu_literal(memory,ucol_getLocaleByType(c,static_cast<ULocDataLocaleType>(i(1)),&status.value)));
    }
    if(name=="ucol_strcollUTF8"){
        const auto* c=collator(0);
        if(!c)return missing();
        const IcuTextInput<char> left(memory,call(1),i(2),status.value),right(memory,call(3),i(4),status.value);
        return done_int(U_SUCCESS(status.value)?ucol_strcollUTF8(c,left.get(),left.length,right.get(),right.length,&status.value):UCOL_EQUAL);
    }
    if(name=="ucol_getTailoredSet" || name=="ucol_getContractionsAndExpansions"){
        const auto* c=collator(0);
        if(!c)return missing();
        if(name=="ucol_getTailoredSet"){
            OwnedIcuSet value(ucol_getTailoredSet(c,&status.value));
            if(!value || U_FAILURE(status.value))return done(0);
            const auto handle=install_icu_set(state,memory,0,value.release());
            if(!handle)status.value=U_MEMORY_ALLOCATION_ERROR;
            return done(handle);
        }
        const auto mutable_set=[&](std::uint64_t handle)->const USet*{
            if(!handle || !icu_sets_)return nullptr;
            const auto found=icu_sets_->objects.find(handle);
            return found==icu_sets_->objects.end()?nullptr:found->second.owned.get();
        };
        const auto* contract=mutable_set(call(1));
        const auto* expand=mutable_set(call(2));
        if((call(1) && !contract) || (call(2) && !expand))return missing();
        const bool shared=contract && call(1)==call(2);
        OwnedIcuSet contractions(contract?uset_clone(contract):nullptr),expansions(!shared && expand?uset_clone(expand):nullptr);
        if((contract && !contractions) || (!shared && expand && !expansions))status.value=U_MEMORY_ALLOCATION_ERROR;
        else ucol_getContractionsAndExpansions(c,contractions.get(),shared?contractions.get():expansions.get(),
                                               (call(3)&0xffU)!=0,&status.value);
        if(U_SUCCESS(status.value)){
            if(contractions && !install_icu_set(state,memory,call(1),contractions.release()))status.value=U_MEMORY_ALLOCATION_ERROR;
            if(U_SUCCESS(status.value) && expansions && !install_icu_set(state,memory,call(2),expansions.release()))
                status.value=U_MEMORY_ALLOCATION_ERROR;
        }
        return done(0);
    }
    if(name=="ucol_open"){
        const auto locale=ascii(call(0));
        IcuHandle<UCollator,ucol_close> native(ucol_open(locale?locale->c_str():nullptr,&status.value));
        return done(native && U_SUCCESS(status.value)?adopt(objects.collators,IcuCollationState::Collator{std::move(native)}):0);
    }
    if(name=="ucol_openRules"){
        const auto rules=load_text(call(0),i(1),status.value);
        UParseError parse{};
        IcuHandle<UCollator,ucol_close> native(U_SUCCESS(status.value)?ucol_openRules(rules->data(),static_cast<std::int32_t>(rules->size()),
            static_cast<UColAttributeValue>(i(2)),static_cast<UCollationStrength>(i(3)),&parse,&status.value):nullptr);
        if(call(4))memory.write(GuestAddress{call(4)},icu_bytes(&parse,1));
        return done(native && U_SUCCESS(status.value)?adopt(objects.collators,IcuCollationState::Collator{std::move(native)}):0);
    }
    if(name=="ucol_clone" || name=="ucol_safeClone"){
        const auto* c=collator(0);if(!c)return missing();
        IcuHandle<UCollator,ucol_close> native;
        if(name=="ucol_clone")native.reset(ucol_clone(c,&status.value));
        else {
            // 舊介面：給了 pBufferSize 就寫回 1，值為 0 時只是預估（不建立物件）；ICU 52 起不使用呼叫端緩衝。
            std::int32_t size=call(2)?signed_argument(memory.load(GuestAddress{call(2)},32)):0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            native.reset(ucol_safeClone(c,nullptr,call(2)?&size:nullptr,&status.value));
#pragma clang diagnostic pop
            if(call(2))memory.store(GuestAddress{call(2)},32,static_cast<std::uint32_t>(size));
        }
        if(!native || U_FAILURE(status.value))return done(0);
        auto owner=icu_collation_binary::adopt_clone(native.release(),objects.collators.at(call(0)).native);
        return done(adopt(objects.collators,IcuCollationState::Collator{std::move(owner)}));
    }
    if(name=="ucol_setAttribute"){auto* c=collator(0);if(!c)return missing();ucol_setAttribute(c,static_cast<UColAttribute>(i(1)),static_cast<UColAttributeValue>(i(2)),&status.value);return done(0);}
    if(name=="ucol_getAttribute"){const auto* c=collator(0);if(!c)return missing(static_cast<std::uint32_t>(UCOL_DEFAULT));return done_int(ucol_getAttribute(c,static_cast<UColAttribute>(i(1)),&status.value));}
    if(name=="ucol_setMaxVariable"){auto* c=collator(0);if(!c)return missing();ucol_setMaxVariable(c,static_cast<UColReorderCode>(i(1)),&status.value);return done(0);}
    if(name=="ucol_getVariableTop"){const auto* c=collator(0);if(!c)return missing();return done(ucol_getVariableTop(c,&status.value));}
    if(name=="ucol_setVariableTop"){
        auto* c=collator(0);if(!c)return missing();
        const auto top=load_text(call(1),i(2),status.value);if(U_FAILURE(status.value))return done(0);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        const auto value=ucol_setVariableTop(c,top->data(),static_cast<std::int32_t>(top->size()),&status.value);
#pragma clang diagnostic pop
        return done(value);
    }
    if(name=="ucol_openElements"){
        const auto found=objects.collators.find(call(0));if(found==objects.collators.end())return missing();
        auto text=load_text(call(1),i(2),status.value);if(U_FAILURE(status.value))return done(0);
        auto provider=found->second.native;
        IcuHandle<UCollationElements,ucol_closeElements> native(ucol_openElements(provider.get(),text->data(),static_cast<std::int32_t>(text->size()),&status.value));
        return done(native && U_SUCCESS(status.value)?adopt(objects.elements,IcuCollationState::Elements{std::move(text),std::move(provider),std::move(native)}):0);
    }
    if(name=="ucol_next" || name=="ucol_previous"){
        auto* e=elements(0);if(!e){status.value=U_ILLEGAL_ARGUMENT_ERROR;return done_int(UCOL_NULLORDER);}
        return done_int(name=="ucol_next"?ucol_next(e->native.get(),&status.value):ucol_previous(e->native.get(),&status.value));
    }
    if(name=="ucol_setOffset"){auto* e=elements(0);if(!e)return missing();ucol_setOffset(e->native.get(),i(1),&status.value);return done(0);}
    if(name=="ucol_setText"){
        auto* e=elements(0);if(!e)return missing();
        auto text=load_text(call(1),i(2),status.value);if(U_FAILURE(status.value))return done(0);
        ucol_setText(e->native.get(),text->data(),static_cast<std::int32_t>(text->size()),&status.value);
        if(U_SUCCESS(status.value))e->text=std::move(text);
        return done(0);
    }
    throw HostCallFault("ICU 排序規則服務未登錄："+std::string(name));
}

void WindowsServices::detach_icu_search_break(std::uint64_t handle){
    if(!icu_collation_)return;
    for(auto& [address,search]:icu_collation_->searches){
        if(search.break_handle!=handle)continue;
        auto status=U_ZERO_ERROR;
        usearch_setBreakIterator(search.native.get(),nullptr,&status);
        search.break_handle=0;
    }
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_string_search(GuestState& state,GuestMemory& memory,
        const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);
    const auto name=service.symbol;
    const auto i=[&](unsigned index){return signed_argument(call(index));};
    const auto finish=[&](std::uint64_t result=0){call.return_to(state,result);return std::optional<std::uint32_t>{};};
    auto& objects=*icu_collation_;
    const auto handle=call(0);
    const auto found=objects.searches.find(handle);
    auto* search=found==objects.searches.end()?nullptr:&found->second;
    auto* native=search?search->native.get():nullptr;
    const auto get_break=[&](std::uint64_t address){return address && icu_breaks_?icu_breaks_->find(address):nullptr;};
    const auto retire_collator=[&](std::uint64_t address){
        const auto entry=objects.collators.find(address);
        if(entry==objects.collators.end() || entry->second.search_owner!=handle)return;
        if(entry->second.rules)(void)free_heap(memory,entry->second.rules,state);
        objects.collators.erase(entry);
        (void)free_heap(memory,address,state);
    };
    if(name=="usearch_close"){
        if(search){
            retire_collator(search->collator_handle);
            objects.searches.erase(found);
            (void)free_heap(memory,handle,state);
        }
        return finish();
    }
    if(name=="usearch_getBreakIterator")return finish(search?search->break_handle:0);
    if(name=="usearch_getAttribute")return finish(static_cast<std::uint32_t>(usearch_getAttribute(native,static_cast<USearchAttribute>(i(1)))));
    if(name=="usearch_getOffset")return finish(static_cast<std::uint32_t>(usearch_getOffset(search && search->ready?native:nullptr)));
    if(name=="usearch_getMatchedLength")return finish(static_cast<std::uint32_t>(usearch_getMatchedLength(native)));
    if(name=="usearch_getMatchedStart")return finish(static_cast<std::uint32_t>(usearch_getMatchedStart(native)));
    if(name=="usearch_reset"){
        if(search && search->ready)usearch_reset(native);
        return finish();
    }
    if(name=="usearch_getPattern" || name=="usearch_getText"){
        if(!search)return finish();
        const bool pattern=name=="usearch_getPattern";
        const auto length=static_cast<std::int32_t>((pattern?search->pattern:search->text)->size());
        write_icu_record(memory,call(1),length);
        return finish(pattern?search->pattern_address:search->text_address);
    }
    if(name=="usearch_getCollator"){
        if(!search)return finish();
        const auto entry=objects.collators.find(search->collator_handle);
        if(entry!=objects.collators.end() && entry->second.native==search->collator)return finish(entry->first);
        const auto address=allocate_heap(memory,16);
        if(!address)return finish();
        try{objects.collators.emplace(address,IcuCollationState::Collator{search->collator,0,handle});}
        catch(const std::bad_alloc&){(void)free_heap(memory,address,state);return finish();}
        search->collator_handle=address;
        return finish(address);
    }

    const auto failure=name=="usearch_getMatchedText" || name=="usearch_first" || name=="usearch_last" ||
        name=="usearch_next" || name=="usearch_previous" || name=="usearch_following" || name=="usearch_preceding"
        ?static_cast<std::uint32_t>(USEARCH_DONE):0U;
    IcuStatus status(memory,call(service.arguments-1));
    const auto initial=status.value;
    const auto done=[&](std::uint64_t result=0){if(status.value!=initial)status.commit();return finish(result);};
    const auto reject=[&](UErrorCode error){status.value=error;return done(failure);};
    if(U_FAILURE(initial))return finish(failure);
    try{
        if(name=="usearch_open" || name=="usearch_openFromCollator"){
            if(!call(0) || !call(2) || !call(4))return reject(U_ILLEGAL_ARGUMENT_ERROR);
            IcuCollationState::Search value;
            value.pattern=read_collation_text(memory,call(0),i(1),status.value);
            value.text=read_collation_text(memory,call(2),i(3),status.value);
            if(U_FAILURE(status.value))return done();
            if(name=="usearch_open"){
                const auto locale=read_ansi(memory,call(4));
                value.collator=CollatorProvider(ucol_open(locale.c_str(),&status.value),ucol_close);
            }else{
                const auto entry=objects.collators.find(call(4));
                if(entry==objects.collators.end())return reject(U_ILLEGAL_ARGUMENT_ERROR);
                value.collator=entry->second.native;
                value.collator_handle=entry->first;
            }
            auto* breaker=get_break(call(5));
            if(call(5) && !breaker)return reject(U_ILLEGAL_ARGUMENT_ERROR);
            if(U_FAILURE(status.value) || !value.collator)return done();
            const auto hold=breaker?retain_search_text(*breaker,value.text):nullptr;
            value.native.reset(usearch_openFromCollator(value.pattern->data(),static_cast<std::int32_t>(value.pattern->size()),
                value.text->data(),static_cast<std::int32_t>(value.text->size()),value.collator.get(),
                breaker?breaker->get():nullptr,&status.value));
            if(breaker)breaker->text=value.native && U_SUCCESS(status.value)?value.text:hold;
            if(!value.native || U_FAILURE(status.value))return done();
            if(status.value!=initial)memory.check(status.address,4,Access::Write);
            value.pattern_address=call(0);
            value.text_address=call(2);
            value.break_handle=call(5);
            const auto address=allocate_heap(memory,16);
            if(!address)return reject(U_MEMORY_ALLOCATION_ERROR);
            try{objects.searches.emplace(address,std::move(value));}
            catch(...){(void)free_heap(memory,address,state);throw;}
            return done(address);
        }
        if(!search)return reject(U_ILLEGAL_ARGUMENT_ERROR);
        if(name=="usearch_setCollator"){
            const auto entry=objects.collators.find(call(1));
            if(entry==objects.collators.end())return reject(U_ILLEGAL_ARGUMENT_ERROR);
            auto provider=entry->second.native;
            usearch_setCollator(native,provider.get(),&status.value);
            if(search->collator_handle!=entry->first)retire_collator(search->collator_handle);
            search->collator_handle=entry->first;
            search->collator=std::move(provider);
            search->ready=U_SUCCESS(status.value);
            return done();
        }
        if(!search->ready)return reject(U_INVALID_STATE_ERROR);
        if(name=="usearch_setBreakIterator"){
            const auto address=call(1);
            auto* breaker=get_break(address);
            if(address && !breaker)return reject(U_ILLEGAL_ARGUMENT_ERROR);
            const auto hold=breaker?retain_search_text(*breaker,search->text):nullptr;
            usearch_setBreakIterator(native,breaker?breaker->get():nullptr,&status.value);
            if(breaker)breaker->text=U_SUCCESS(status.value)?search->text:hold;
            search->break_handle=address;
            return done();
        }
        if(name=="usearch_setText" || name=="usearch_setPattern"){
            if(!call(1) || !i(2))return reject(U_ILLEGAL_ARGUMENT_ERROR);
            const auto replacement=read_collation_text(memory,call(1),i(2),status.value);
            if(U_FAILURE(status.value))return done();
            if(name=="usearch_setPattern"){
                if(replacement->empty())return reject(U_ILLEGAL_ARGUMENT_ERROR);
                usearch_setPattern(native,replacement->data(),static_cast<std::int32_t>(replacement->size()),&status.value);
                search->pattern=replacement;
                search->pattern_address=call(1);
                if(U_FAILURE(status.value))search->ready=false;
            }else{
                auto* breaker=get_break(search->break_handle);
                const auto hold=breaker?retain_search_text(*breaker,replacement):nullptr;
                const auto length=i(2)<0?-1:static_cast<std::int32_t>(replacement->size());
                usearch_setText(native,replacement->data(),length,&status.value);
                if(breaker)breaker->text=U_SUCCESS(status.value)?replacement:hold;
                search->text=replacement;
                search->text_address=call(1);
                if(U_FAILURE(status.value))search->ready=false;
            }
            return done();
        }
        if(name=="usearch_setAttribute"){
            usearch_setAttribute(native,static_cast<USearchAttribute>(i(1)),static_cast<USearchAttributeValue>(i(2)),&status.value);
            return done();
        }
        if(name=="usearch_setOffset"){
            usearch_setOffset(native,i(1),&status.value);
            return done();
        }
        if(name=="usearch_getMatchedText"){
            const auto capacity=i(2);
            if(capacity<0 || (capacity && !call(1)))return reject(U_ILLEGAL_ARGUMENT_ERROR);
            if(usearch_getMatchedStart(native)==USEARCH_DONE){
                UChar terminal{};
                const auto result=usearch_getMatchedText(native,capacity?&terminal:nullptr,capacity,&status.value);
                if(capacity)write_icu_record(memory,call(1),terminal);
                return done(static_cast<std::uint32_t>(result));
            }
            return finish(icu_output<UChar>(memory,call(1),capacity,status,IcuOutput::TruncatedString,
                [&](auto* output,auto count,auto* error){return usearch_getMatchedText(native,output,count,error);}));
        }
        std::int32_t result=USEARCH_DONE;
        if(name=="usearch_first")result=usearch_first(native,&status.value);
        else if(name=="usearch_last")result=usearch_last(native,&status.value);
        else if(name=="usearch_next")result=usearch_next(native,&status.value);
        else if(name=="usearch_previous")result=usearch_previous(native,&status.value);
        else if(name=="usearch_following")result=usearch_following(native,i(1),&status.value);
        else if(name=="usearch_preceding")result=usearch_preceding(native,i(1),&status.value);
        else throw HostCallFault("ICU 字串搜尋服務未登錄："+std::string(name));
        return done(static_cast<std::uint32_t>(result));
    }catch(const std::bad_alloc&){return reject(U_MEMORY_ALLOCATION_ERROR);}
}
}

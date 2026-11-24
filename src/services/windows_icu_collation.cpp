#include "x64arm/windows_services.hpp"
#include "ansi_codepage.hpp"
#include "icu_guest_abi.hpp"
#include <map>
#include <memory>
#include <unicode/ucol.h>
#include <unicode/ucoleitr.h>
#include <unicode/uldnames.h>
#include <unicode/ures.h>
#include <unicode/usearch.h>

// ICU 排序規則、字串搜尋、資源包與地區顯示名稱（.NET 的 System.Globalization 以 icu.dll 取得這些入口做字串比較、排序鍵與
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
using Text=std::unique_ptr<std::u16string>;
}

struct IcuCollationState {
    struct Collator {IcuHandle<UCollator,ucol_close> native;std::uint64_t rules{};};
    struct Elements {IcuHandle<UCollationElements,ucol_closeElements> native;Text text;};
    struct Search {IcuHandle<UStringSearch,usearch_close> native;Text pattern,text;};
    struct Bundle {IcuHandle<UResourceBundle,ures_close> native;std::vector<std::uint64_t> strings;};
    std::map<std::uint64_t,Collator> collators;
    std::map<std::uint64_t,Elements> elements;
    std::map<std::uint64_t,Search> searches;
    std::map<std::uint64_t,Bundle> bundles;
    std::map<std::uint64_t,IcuHandle<ULocaleDisplayNames,uldn_close>> names;
};

std::span<const WindowsServiceDescriptor> icu_collation_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_COLL_API(name,count,legacy) {"icu.dll",#name,count,0,0,0,true},
#include "icu_collation_services.inc"
#undef ICU_COLL_API
    };
    return entries;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_collation(GuestState& state,GuestMemory& memory,
        const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);
    const auto name=service.symbol;
    const auto i=[&](unsigned n){return signed_argument(call(n));};
    const auto finish=[&](std::uint64_t result){call.return_to(state,result);return std::optional<std::uint32_t>{};};
    const auto finish_int=[&](std::int64_t value){return finish(static_cast<std::uint32_t>(static_cast<std::int32_t>(value)));};
    if(!icu_collation_)icu_collation_=std::make_shared<IcuCollationState>();
    auto& objects=*icu_collation_;
    const auto guest_copy=[&](const void* data,std::size_t bytes)->std::uint64_t {
        const auto address=allocate_heap(memory,std::max<std::size_t>(bytes,1));
        if(address && bytes)memory.write(GuestAddress{address},{static_cast<const std::uint8_t*>(data),bytes});
        return address;};
    const auto release_strings=[&](IcuCollationState::Bundle& bundle){
        for(const auto address:bundle.strings)(void)free_heap(memory,address,state);
        bundle.strings.clear();};
    const auto collator=[&](unsigned n)->UCollator* {const auto found=objects.collators.find(call(n));return found==objects.collators.end()?nullptr:found->second.native.get();};
    const auto elements=[&](unsigned n)->IcuCollationState::Elements* {const auto found=objects.elements.find(call(n));return found==objects.elements.end()?nullptr:&found->second;};
    const auto search=[&](unsigned n)->IcuCollationState::Search* {const auto found=objects.searches.find(call(n));return found==objects.searches.end()?nullptr:&found->second;};
    const auto bundle=[&](unsigned n)->IcuCollationState::Bundle* {const auto found=objects.bundles.find(call(n));return found==objects.bundles.end()?nullptr:&found->second;};
    const auto display=[&](unsigned n)->ULocaleDisplayNames* {const auto found=objects.names.find(call(n));return found==objects.names.end()?nullptr:found->second.get();};
    // 讀客體 UTF-16（長度 -1 為以 NUL 結尾）到穩定緩衝。
    const auto load_text=[&](std::uint64_t address,std::int32_t length,UErrorCode& error)->Text {
        IcuTextInput<UChar> input(memory,address,length,error);
        if(U_FAILURE(error))return {};
        const auto* data=input.get();
        if(!data)return std::make_unique<std::u16string>();
        return std::make_unique<std::u16string>(data,length<0?std::char_traits<UChar>::length(data):static_cast<std::size_t>(length));};
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
            if constexpr(std::is_same_v<std::decay_t<decltype(map)>,decltype(objects.collators)>){if(found->second.rules)(void)free_heap(memory,found->second.rules,state);}
            if constexpr(std::is_same_v<std::decay_t<decltype(map)>,decltype(objects.bundles)>)release_strings(found->second);
            map.erase(found);(void)free_heap(memory,call(0),state);}
        return finish(0);};
    if(name=="ucol_close")return close(objects.collators);
    if(name=="ucol_closeElements")return close(objects.elements);
    if(name=="usearch_close")return close(objects.searches);
    if(name=="ures_close")return close(objects.bundles);
    if(name=="uldn_close")return close(objects.names);
    if(name=="ucol_getStrength"){const auto* c=collator(0);return finish_int(c?ucol_getStrength(c):UCOL_DEFAULT);}
    if(name=="ucol_setStrength"){if(auto* c=collator(0))ucol_setStrength(c,static_cast<UCollationStrength>(i(1)));return finish(0);}
    if(name=="ucol_getMaxVariable"){const auto* c=collator(0);return finish_int(c?ucol_getMaxVariable(c):UCOL_REORDER_CODE_PUNCTUATION);}
    if(name=="ucol_getOffset"){const auto* e=elements(0);return finish_int(e?ucol_getOffset(e->native.get()):0);}
    if(name=="ucol_reset"){if(auto* e=elements(0))ucol_reset(e->native.get());return finish(0);}
    if(name=="ucol_getMaxExpansion"){const auto* e=elements(0);return finish_int(e?ucol_getMaxExpansion(e->native.get(),i(1)):1);}
    if(name=="ucol_getVersion"){
        UVersionInfo version{};if(const auto* c=collator(0))ucol_getVersion(c,version);
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
    if(name=="usearch_getMatchedLength"){const auto* s=search(0);return finish_int(s?usearch_getMatchedLength(s->native.get()):0);}
    if(name=="usearch_getMatchedStart"){const auto* s=search(0);return finish_int(s?usearch_getMatchedStart(s->native.get()):USEARCH_DONE);}
    // 只以 usearch_openFromCollator 建立且不帶斷詞器：與 ICU 相同回傳 NULL。
    if(name=="usearch_getBreakIterator")return finish(0);
    if(name=="usearch_reset"){if(auto* s=search(0))usearch_reset(s->native.get());return finish(0);}
    if(name=="ures_getSize"){const auto* b=bundle(0);return finish_int(b?ures_getSize(b->native.get()):0);}
    if(name=="ures_getType"){const auto* b=bundle(0);return finish_int(b?ures_getType(b->native.get()):URES_NONE);}
    if(name=="ures_getKey"){
        auto* b=bundle(0);const char* key=b?ures_getKey(b->native.get()):nullptr;
        if(!key)return finish(0);
        const auto address=guest_copy(key,std::char_traits<char>::length(key)+1);if(address)b->strings.push_back(address);
        return finish(address);
    }
    if(name=="uldn_getDialectHandling"){const auto* d=display(0);return finish_int(d?uldn_getDialectHandling(d):ULDN_STANDARD_NAMES);}

    // ---- 帶 UErrorCode（最後一個參數）的入口：進入時已失敗就不動作 ----
    IcuStatus status(memory,call(service.arguments-1));
    const auto done=[&](std::uint64_t value){status.commit();return finish(value);};
    const auto done_int=[&](std::int64_t value){status.commit();return finish_int(value);};
    if(U_FAILURE(status.value)){
        if(name=="ucol_next" || name=="ucol_previous")return finish_int(UCOL_NULLORDER);
        if(name.starts_with("usearch_") && name!="usearch_openFromCollator" && name!="usearch_setText" && name!="usearch_setPattern")return finish_int(USEARCH_DONE);
        return finish(0);
    }
    const auto adopt=[&](auto& map,auto object)->std::uint64_t {
        const auto handle=allocate_heap(memory,16);
        if(!handle){status.value=U_MEMORY_ALLOCATION_ERROR;return 0;}
        map.emplace(handle,std::move(object));return handle;};
    const auto missing=[&](std::uint64_t value=0){status.value=U_ILLEGAL_ARGUMENT_ERROR;return done(value);};
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
        return done(native && !U_FAILURE(status.value)?adopt(objects.collators,IcuCollationState::Collator{std::move(native)}):0);
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
        const auto* c=collator(0);if(!c)return missing();
        auto text=load_text(call(1),i(2),status.value);if(U_FAILURE(status.value))return done(0);
        IcuHandle<UCollationElements,ucol_closeElements> native(ucol_openElements(c,text->data(),static_cast<std::int32_t>(text->size()),&status.value));
        return done(native && U_SUCCESS(status.value)?adopt(objects.elements,IcuCollationState::Elements{std::move(native),std::move(text)}):0);
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
    if(name=="usearch_openFromCollator"){
        auto pattern=load_text(call(0),i(1),status.value);auto text=load_text(call(2),i(3),status.value);
        const auto* c=collator(4);
        if(U_FAILURE(status.value))return done(0);
        if(!c)return missing();
        // 斷詞器物件屬於客體端的 ubrk 實作，無法交給宿主搜尋；與 ICU 拒絕不相容參數相同回報錯誤。
        if(call(5)){status.value=U_UNSUPPORTED_ERROR;return done(0);}
        IcuHandle<UStringSearch,usearch_close> native(usearch_openFromCollator(pattern->data(),static_cast<std::int32_t>(pattern->size()),
            text->data(),static_cast<std::int32_t>(text->size()),c,nullptr,&status.value));
        return done(native && U_SUCCESS(status.value)?adopt(objects.searches,IcuCollationState::Search{std::move(native),std::move(pattern),std::move(text)}):0);
    }
    if(name=="usearch_setText" || name=="usearch_setPattern"){
        auto* s=search(0);if(!s)return missing();
        auto text=load_text(call(1),i(2),status.value);if(U_FAILURE(status.value))return done(0);
        if(name=="usearch_setText"){usearch_setText(s->native.get(),text->data(),static_cast<std::int32_t>(text->size()),&status.value);if(U_SUCCESS(status.value))s->text=std::move(text);}
        else {usearch_setPattern(s->native.get(),text->data(),static_cast<std::int32_t>(text->size()),&status.value);if(U_SUCCESS(status.value))s->pattern=std::move(text);}
        return done(0);
    }
    if(name.starts_with("usearch_")){
        auto* s=search(0);if(!s){status.value=U_ILLEGAL_ARGUMENT_ERROR;return done_int(USEARCH_DONE);}
        auto* native=s->native.get();
        if(name=="usearch_first")return done_int(usearch_first(native,&status.value));
        if(name=="usearch_last")return done_int(usearch_last(native,&status.value));
        if(name=="usearch_next")return done_int(usearch_next(native,&status.value));
        if(name=="usearch_previous")return done_int(usearch_previous(native,&status.value));
        if(name=="usearch_following")return done_int(usearch_following(native,i(1),&status.value));
        if(name=="usearch_preceding")return done_int(usearch_preceding(native,i(1),&status.value));
    }
    if(name=="ures_open"){
        const auto package=ascii(call(0)),locale=ascii(call(1));
        IcuHandle<UResourceBundle,ures_close> native(ures_open(package?package->c_str():nullptr,locale?locale->c_str():nullptr,&status.value));
        return done(native && U_SUCCESS(status.value)?adopt(objects.bundles,IcuCollationState::Bundle{std::move(native),{}}):0);
    }
    if(name=="ures_getByKey" || name=="ures_getByIndex"){
        auto* b=bundle(0);if(!b)return missing();
        // fillIn 是本表的 handle 時沿用同一個物件（ICU 原地重填），否則建立新 handle。
        IcuCollationState::Bundle* fill=nullptr;
        if(call(2)){fill=bundle(2);if(!fill)return missing();}
        const auto key=name=="ures_getByKey"?ascii(call(1)):std::nullopt;
        if(name=="ures_getByKey" && !key)return missing();
        auto* native=name=="ures_getByKey"?ures_getByKey(b->native.get(),key->c_str(),fill?fill->native.get():nullptr,&status.value)
                                          :ures_getByIndex(b->native.get(),i(1),fill?fill->native.get():nullptr,&status.value);
        if(fill){release_strings(*fill);return done(native?call(2):0);}
        IcuHandle<UResourceBundle,ures_close> owned(native);
        return done(owned && U_SUCCESS(status.value)?adopt(objects.bundles,IcuCollationState::Bundle{std::move(owned),{}}):0);
    }
    if(name=="ures_getString" || name=="ures_getStringByIndex" || name=="ures_getStringByKey"){
        auto* b=bundle(0);const unsigned length_argument=name=="ures_getString"?1U:2U;
        if(!b){if(call(length_argument))memory.store(GuestAddress{call(length_argument)},32,0);return missing();}
        std::int32_t length=0;const UChar* text=nullptr;
        if(name=="ures_getString")text=ures_getString(b->native.get(),&length,&status.value);
        else if(name=="ures_getStringByIndex")text=ures_getStringByIndex(b->native.get(),i(1),&length,&status.value);
        else {const auto key=ascii(call(1));if(!key)return missing();text=ures_getStringByKey(b->native.get(),key->c_str(),&length,&status.value);}
        if(call(length_argument))memory.store(GuestAddress{call(length_argument)},32,static_cast<std::uint32_t>(U_SUCCESS(status.value)?length:0));
        if(!text || U_FAILURE(status.value))return done(0);
        std::u16string copy(text,static_cast<std::size_t>(std::max(length,0)));copy.push_back(u'\0');
        const auto address=guest_copy(copy.data(),copy.size()*sizeof(UChar));
        if(!address){status.value=U_MEMORY_ALLOCATION_ERROR;return done(0);}
        b->strings.push_back(address);return done(address);
    }
    if(name=="uldn_open"){
        const auto locale=ascii(call(0));
        IcuHandle<ULocaleDisplayNames,uldn_close> native(uldn_open(locale?locale->c_str():nullptr,static_cast<UDialectHandling>(i(1)),&status.value));
        return done(native && U_SUCCESS(status.value)?adopt(objects.names,std::move(native)):0);
    }
    if(name.starts_with("uldn_")){
        const auto* d=display(0);if(!d)return missing();
        const bool pair=name=="uldn_keyValueDisplayName";
        const auto first=ascii(call(1));const auto second=pair?ascii(call(2)):std::nullopt;
        if(!first || (pair && !second))return missing();
        const unsigned output=pair?3U:2U;
        const auto length=icu_output<UChar>(memory,call(output),i(output+1),status,IcuOutput::String,[&](UChar* buffer,std::int32_t size,UErrorCode* error){
            if(name=="uldn_keyDisplayName")return uldn_keyDisplayName(d,first->c_str(),buffer,size,error);
            if(pair)return uldn_keyValueDisplayName(d,first->c_str(),second->c_str(),buffer,size,error);
            if(name=="uldn_languageDisplayName")return uldn_languageDisplayName(d,first->c_str(),buffer,size,error);
            if(name=="uldn_localeDisplayName")return uldn_localeDisplayName(d,first->c_str(),buffer,size,error);
            if(name=="uldn_regionDisplayName")return uldn_regionDisplayName(d,first->c_str(),buffer,size,error);
            return uldn_scriptDisplayName(d,first->c_str(),buffer,size,error);});
        return finish(length);
    }
    throw HostCallFault("ICU 排序規則服務未登錄："+std::string(name));
}
}

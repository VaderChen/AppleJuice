#include "x64arm/windows_services.hpp"
#include "icu_break_state.hpp"
#include "icu_guest_abi.hpp"
#include <unicode/ucasemap.h>
#include <unicode/uloc.h>
#include <unicode/ustring.h>

namespace x64arm {
namespace {
using namespace icu_abi;
using OwnedCase=std::unique_ptr<UCaseMap,decltype(&ucasemap_close)>;
struct CaseObject {
    std::shared_ptr<const void> text;
    OwnedCase map{nullptr,ucasemap_close};
    std::uint64_t break_handle{};
    std::uint64_t locale_storage{};
};

template<class Char>
std::shared_ptr<const std::basic_string<Char>> input_text(GuestMemory& memory,std::uint64_t address,
        std::int32_t length,UErrorCode& status,bool stop_at_null=false){
    if(U_FAILURE(status))return {};
    if(length< -1 || (!address && length)){
        status=U_ILLEGAL_ARGUMENT_ERROR;
        return {};
    }
    if(!address)return std::make_shared<const std::basic_string<Char>>();
    if(length<0){
        if constexpr(sizeof(Char)==1)return std::make_shared<const std::string>(read_ansi(memory,address,INT32_MAX));
        else return std::make_shared<const std::u16string>(read_utf16(memory,GuestAddress{address},INT32_MAX));
    }
    const auto count=static_cast<std::size_t>(length);
    if(stop_at_null){
        auto text=std::make_shared<std::basic_string<Char>>();
        for(std::size_t i=0;i<count;++i){
            const auto unit=static_cast<Char>(memory.load(GuestAddress{address+i*sizeof(Char)},sizeof(Char)*8));
            if(!unit)break;
            text->push_back(unit);
        }
        return text;
    }
    const auto size=count*sizeof(Char);
    if(size)memory.check(GuestAddress{address},size,Access::Read);
    auto text=std::make_shared<std::basic_string<Char>>(count,Char{});
    if(size)memory.read(GuestAddress{address},{reinterpret_cast<std::uint8_t*>(text->data()),size});
    return text;
}

bool overlaps(std::uint64_t first,std::uint64_t first_size,std::uint64_t second,std::uint64_t second_size){
    if(!first_size || !second_size)return false;
    return first<=second?second-first<first_size:first-second<second_size;
}

template<class Char>
void preserve_unwritten(GuestMemory& memory,std::uint64_t address,Char* output,std::int32_t capacity){
    if(capacity>0)memory.read(GuestAddress{address},
        {reinterpret_cast<std::uint8_t*>(output),static_cast<std::size_t>(capacity)*sizeof(Char)});
}

void restore_title_source(UBreakIterator* iterator,const std::u16string& source,UErrorCode* status){
    if(!iterator)return;
    const auto position=ubrk_current(iterator);
    UErrorCode binding=U_ZERO_ERROR;
    // UTF-16 標題函式用區域 UnicodeString 包住來源；改用仍存活的 UTF-16 backing。
    ubrk_setText(iterator,source.data(),static_cast<std::int32_t>(source.size()),&binding);
    if(U_SUCCESS(binding) && position>=0)(void)ubrk_isBoundary(iterator,position);
    if(U_FAILURE(binding) && U_SUCCESS(*status))*status=binding;
}

// 失敗的標題轉換可能已替換 iterator 的文字；維持前後 backing 至下一次成功替換。
std::shared_ptr<const void> retain_both(const std::shared_ptr<const void>& old,
                                        const std::shared_ptr<const void>& next){
    if(!old)return next;
    return std::make_shared<const std::pair<std::shared_ptr<const void>,std::shared_ptr<const void>>>(old,next);
}
}

struct IcuCaseState {
    std::map<std::uint64_t,CaseObject> objects;
};

std::span<const WindowsServiceDescriptor> icu_case_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_CASE_API(name,count) {"icu.dll",#name,count,0,0,0,true},
#define ICU_CASE_RESIDENT ICU_CASE_API
#include "icu_case_services.inc"
#undef ICU_CASE_RESIDENT
#undef ICU_CASE_API
    };
    return entries;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_case(GuestState& state,GuestMemory& memory,
        const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);
    const auto name=service.symbol;
    const auto arg=[&](unsigned i){return signed_argument(call(i));};
    const auto options=[&](unsigned i){return static_cast<std::uint32_t>(call(i));};
    const auto finish=[&](std::uint64_t result){call.return_to(state,result);return std::optional<std::uint32_t>{};};
    if(name=="u_strCaseCompare"){
        IcuStatus status(memory,call(5));
        const auto a=input_text<UChar>(memory,call(0),arg(1),status.value);
        const auto b=input_text<UChar>(memory,call(2),arg(3),status.value);
        const auto result=a && b?u_strCaseCompare(a->data(),static_cast<std::int32_t>(a->size()),
            b->data(),static_cast<std::int32_t>(b->size()),options(4),&status.value):0;
        status.commit();
        return finish(static_cast<std::uint32_t>(result));
    }
    if(name=="u_strcasecmp" || name=="u_strncasecmp" || name=="u_memcasecmp"){
        UErrorCode status=U_ZERO_ERROR;
        const bool terminated=name!="u_memcasecmp";
        const bool bounded=name!="u_strcasecmp";
        const auto length=bounded?arg(2):-1;
        if(bounded && length<=0)return finish(0);
        const auto a=input_text<UChar>(memory,call(0),length,status,terminated);
        const auto b=input_text<UChar>(memory,call(1),length,status,terminated);
        if(!a || !b)return finish(0);
        const auto flags=options(bounded?3U:2U);
        const auto result=terminated?u_strcasecmp(a->c_str(),b->c_str(),flags):u_memcasecmp(a->data(),b->data(),length,flags);
        return finish(static_cast<std::uint32_t>(result));
    }
    if(!name.starts_with("ucasemap_")){
        const bool title=name=="u_strToTitle";
        const bool fold=name=="u_strFoldCase";
        IcuStatus status(memory,call(title?6U:5U));
        const auto source=input_text<UChar>(memory,call(2),arg(3),status.value);
        if(!source){status.commit();return finish(0);}
        const auto locale_at=fold?0:call(title?5U:4U);
        const auto locale=locale_at?read_ansi(memory,locale_at):std::string{};
        auto* iterator=title && call(4) && icu_breaks_?icu_breaks_->find(call(4)):nullptr;
        if(title && call(4) && !iterator){status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();return finish(0);}
        bool called=false;
        const auto previous=iterator?retain_both(iterator->text,source):std::shared_ptr<const void>{};
        const auto length=arg(3);
        const auto result=icu_output<UChar>(memory,call(0),arg(1),status,IcuOutput::TruncatedString,
            [&](auto* output,auto capacity,auto* code){
                preserve_unwritten(memory,call(0),output,capacity);
                called=true;
                if(iterator)iterator->text=previous;
                if(fold)return u_strFoldCase(output,capacity,source->data(),length,options(4),code);
                const auto* tag=locale_at?locale.c_str():icu_default_locale();
                if(title){
                    auto* native=iterator?iterator->get():nullptr;
                    const auto count=u_strToTitle(output,capacity,source->data(),length,native,tag,code);
                    restore_title_source(native,*source,code);
                    return count;
                }
                if(name=="u_strToLower")return u_strToLower(output,capacity,source->data(),length,tag,code);
                return u_strToUpper(output,capacity,source->data(),length,tag,code);
            });
        if(called && iterator && (U_SUCCESS(status.value) || status.value==U_BUFFER_OVERFLOW_ERROR))iterator->text=source;
        return finish(result);
    }

    if(!icu_cases_)icu_cases_=std::make_shared<IcuCaseState>();
    auto& cases=icu_cases_->objects;
    if(name=="ucasemap_open"){
        IcuStatus status(memory,call(2));
        if(U_FAILURE(status.value))return finish(0);
        const auto locale=call(0)?read_ansi(memory,call(0)):std::string{};
        CaseObject opened;
        opened.map.reset(ucasemap_open(call(0)?locale.c_str():icu_default_locale(),options(1),&status.value));
        if(!opened.map || U_FAILURE(status.value)){status.commit();return finish(0);}
        const auto handle=allocate_heap(memory,1);
        if(handle){
            try{cases.emplace(handle,std::move(opened));}
            catch(...){(void)free_heap(memory,handle,state);throw;}
        }else status.value=U_MEMORY_ALLOCATION_ERROR;
        status.commit();
        return finish(handle);
    }
    const auto found=cases.find(call(0));
    auto* object=found==cases.end()?nullptr:&found->second;
    const auto retire=[&](std::uint64_t handle){
        if(handle && icu_breaks_){
            icu_breaks_->objects.erase(handle);
            (void)free_heap(memory,handle,state);
        }
    };
    if(name=="ucasemap_close"){
        if(object){
            const auto handle=object->break_handle;
            const auto locale=object->locale_storage;
            cases.erase(found);
            retire(handle);
            if(locale)(void)free_heap(memory,locale,state);
        }
        return finish(0);
    }
    const auto update_locale=[&]{
        const auto* locale=ucasemap_getLocale(object->map.get());
        memory.write(GuestAddress{object->locale_storage},icu_bytes(locale,std::char_traits<char>::length(locale)+1));
    };
    if(name=="ucasemap_getLocale"){
        if(!object)return finish(0);
        if(!object->locale_storage){
            object->locale_storage=allocate_heap(memory,ULOC_FULLNAME_CAPACITY);
            if(object->locale_storage)update_locale();
        }
        return finish(object->locale_storage);
    }
    if(name=="ucasemap_getOptions")return finish(object?ucasemap_getOptions(object->map.get()):0);
    if(name=="ucasemap_getBreakIterator"){
        if(!object)return finish(0);
        if(object->break_handle)return finish(object->break_handle);
        const auto* iterator=ucasemap_getBreakIterator(object->map.get());
        if(!iterator)return finish(0);
        if(!icu_breaks_)icu_breaks_=std::make_shared<IcuBreakState>();
        const auto handle=allocate_heap(memory,1);
        if(!handle)return finish(0);
        icu_break::BreakObject borrowed;
        borrowed.borrowed=const_cast<UBreakIterator*>(iterator);
        borrowed.case_owner=call(0);
        borrowed.text=object->text;
        try{icu_breaks_->objects.emplace(handle,std::move(borrowed));}
        catch(...){(void)free_heap(memory,handle,state);throw;}
        object->break_handle=handle;
        return finish(handle);
    }
    const bool property=name=="ucasemap_setLocale" || name=="ucasemap_setOptions" || name=="ucasemap_setBreakIterator";
    IcuStatus status(memory,call(property?2U:5U));
    if(U_FAILURE(status.value))return finish(0);
    if(!object){status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();return finish(0);}
    if(name=="ucasemap_setLocale"){
        const auto locale=call(1)?read_ansi(memory,call(1)):std::string{};
        ucasemap_setLocale(object->map.get(),call(1)?locale.c_str():icu_default_locale(),&status.value);
        if(object->locale_storage)update_locale();
    }else if(name=="ucasemap_setOptions"){
        ucasemap_setOptions(object->map.get(),options(1),&status.value);
    }else if(name=="ucasemap_setBreakIterator"){
        const auto next=call(1);
        if(next && next==object->break_handle)return finish(0);
        auto* editor=next && icu_breaks_?icu_breaks_->find(next):nullptr;
        if(next && (!editor || !editor->iterator))status.value=U_ILLEGAL_ARGUMENT_ERROR;
        else{
            ucasemap_setBreakIterator(object->map.get(),editor?editor->get():nullptr,&status.value);
            if(U_SUCCESS(status.value)){
                retire(object->break_handle);
                object->break_handle=next;
                object->text.reset();
                if(editor){
                    editor->borrowed=editor->iterator.release();
                    editor->case_owner=call(0);
                }
            }
        }
    }else{
        const auto transform=[&]<class Char>(auto operation,bool title){
            const auto source=input_text<Char>(memory,call(3),arg(4),status.value);
            if(!source){status.commit();return std::uint32_t{};}
            if(arg(2)<0 || overlaps(call(1),sizeof(Char)*static_cast<std::uint64_t>(arg(2)),
                                    call(3),source->size()*sizeof(Char))){
                status.value=U_ILLEGAL_ARGUMENT_ERROR;
                status.commit();
                return std::uint32_t{};
            }
            auto* iterator=title && object->break_handle?icu_breaks_->find(object->break_handle):nullptr;
            const auto previous=title?retain_both(iterator?iterator->text:object->text,source):std::shared_ptr<const void>{};
            const auto result=icu_output<Char>(memory,call(1),arg(2),status,IcuOutput::TruncatedString,
                [&](auto* output,auto capacity,auto* code){
                    preserve_unwritten(memory,call(1),output,capacity);
                    if(title){if(iterator)iterator->text=previous;else object->text=previous;}
                    const auto count=operation(object->map.get(),output,capacity,source->data(),arg(4),code);
                    if constexpr(std::is_same_v<Char,UChar>){
                        if(title)restore_title_source(const_cast<UBreakIterator*>(ucasemap_getBreakIterator(object->map.get())),
                                                      *source,code);
                    }
                    return count;
                });
            if(title && (U_SUCCESS(status.value) || status.value==U_BUFFER_OVERFLOW_ERROR)){
                if(iterator)iterator->text=source;
                else object->text=source;
            }
            return result;
        };
        if(name=="ucasemap_toTitle")return finish(transform.template operator()<UChar>(ucasemap_toTitle,true));
        if(name=="ucasemap_utf8ToTitle")return finish(transform.template operator()<char>(ucasemap_utf8ToTitle,true));
        if(name=="ucasemap_utf8ToLower")return finish(transform.template operator()<char>(ucasemap_utf8ToLower,false));
        if(name=="ucasemap_utf8ToUpper")return finish(transform.template operator()<char>(ucasemap_utf8ToUpper,false));
        return finish(transform.template operator()<char>(ucasemap_utf8FoldCase,false));
    }
    status.commit();
    return finish(0);
}
}

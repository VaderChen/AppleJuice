#include "x64arm/windows_services.hpp"
#include "icu_break_binary.hpp"
#include "icu_break_projection.hpp"
#include "icu_break_state.hpp"
#include "icu_guest_abi.hpp"
#include "icu_text_guest.hpp"
#include "icu_text_projection_layout.hpp"
#include "service_callback_task.hpp"
#include <unicode/ubrk.h>
#include <unicode/utf16.h>

namespace x64arm {
namespace {
using namespace icu_abi;
using icu_break::BreakObject;
using icu_break::Rules;

template<auto Function>
std::uint32_t query(UBreakIterator* iterator,const WindowsCall& call){
    if(!iterator){
        if constexpr(std::is_same_v<decltype(Function),decltype(&ubrk_isBoundary)>)return 0;
        return static_cast<std::uint32_t>(UBRK_DONE);
    }
    if constexpr(std::is_invocable_v<decltype(Function),UBreakIterator*,std::int32_t>)
        return static_cast<std::uint32_t>(Function(iterator,signed_argument(call(1))));
    else
        return static_cast<std::uint32_t>(Function(iterator));
}

constexpr struct BreakDescriptor {
    WindowsServiceDescriptor service;
    std::uint32_t (*query)(UBreakIterator*,const WindowsCall&);
} descriptors[]{
#define ICU_BREAK_API(name,count) {{"icu.dll",#name,count,0,0,0,true},nullptr},
#define ICU_BREAK_QUERY(name,count) {{"icu.dll",#name,count,0,0,0,true},query<name>},
#include "icu_break_services.inc"
#undef ICU_BREAK_QUERY
#undef ICU_BREAK_API
};

std::shared_ptr<const std::u16string> load_text(GuestMemory& memory,std::uint64_t address,
                                               std::int32_t length,UErrorCode& error){
    if(U_FAILURE(error))return {};
    if(length< -1 || (!address && length)){
        error=U_ILLEGAL_ARGUMENT_ERROR;
        return {};
    }
    if(length<0)return std::make_shared<const std::u16string>(
        read_utf16(memory,GuestAddress{address},INT32_MAX));
    const auto count=static_cast<std::size_t>(length);
    const auto size=count*sizeof(UChar);
    if(size)memory.check(GuestAddress{address},size,Access::Read);
    auto text=std::make_shared<std::u16string>(count,u'\0');
    if(size)memory.read(GuestAddress{address},{reinterpret_cast<std::uint8_t*>(text->data()),size});
    return text;
}

icu_break::SharedProjection load_projection(GuestMemory& memory,std::uint64_t address,UErrorCode& error){
    icu_text::Snapshot header{};
    memory.read(GuestAddress{address},{reinterpret_cast<std::uint8_t*>(&header),sizeof(header)});
    if(header.native_length>INT32_MAX || header.count>header.native_length ||
       bool(header.count)!=bool(header.native_length)){
        error=U_INVALID_FORMAT_ERROR;
        return {};
    }
    const auto size=std::size_t{header.count}*sizeof(icu_text::CodePoint);
    const GuestAddress records{address+sizeof(header)};
    if(size)memory.check(records,size,Access::Read);
    std::vector<icu_text::CodePoint> points(header.count);
    if(size)memory.read(records,{reinterpret_cast<std::uint8_t*>(points.data()),size});
    auto result=std::make_shared<icu_break::Projection>();
    result->units.reserve(header.count);
    result->native_offsets.reserve(std::size_t{header.count}+1);
    std::uint32_t prior=0;
    for(std::size_t i=0;i<points.size();++i){
        const auto [offset,code]=points[i];
        if((i==0?offset!=0:offset<=prior) || offset>=header.native_length || code>0x10ffff){
            error=U_INVALID_FORMAT_ERROR;
            return {};
        }
        const auto length=code<=0xffff?1U:2U;
        if(result->units.size()>std::size_t{INT32_MAX}-length){
            error=U_INDEX_OUTOFBOUNDS_ERROR;
            return {};
        }
        const auto index=static_cast<std::int32_t>(offset);
        result->native_offsets.push_back(index);
        if(length==1)result->units.push_back(static_cast<UChar>(code));
        else{
            result->units.push_back(U16_LEAD(code));
            result->native_offsets.push_back(index);
            result->units.push_back(U16_TRAIL(code));
        }
        prior=offset;
    }
    result->native_offsets.push_back(static_cast<std::int32_t>(header.native_length));
    return result;
}
}

std::span<const WindowsServiceDescriptor> icu_break_service_descriptors(){
    static const auto table=[]{
        std::array<WindowsServiceDescriptor,std::size(descriptors)> entries{};
        for(std::size_t i=0;i<entries.size();++i)entries[i]=descriptors[i].service;
        return entries;
    }();
    return table;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_break(GuestState& state,GuestMemory& memory,
        const WindowsServiceDescriptor& service){
    if(!icu_breaks_)icu_breaks_=std::make_shared<IcuBreakState>();
    const auto name=service.symbol;
    if(name=="ubrk_setUText" || name=="ubrk_refreshUText"){
        return nested_continuation(state,memory,"ICU 文字分段 provider",
            [this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);
                const auto finish=[&]{call.return_to(s,0);return std::optional<std::uint32_t>{};};
                IcuStatus status(m,call(2));
                if(U_FAILURE(status.value))co_return finish();
                const auto handle=call(0);
                if(!icu_breaks_->find(handle) || !call(1)){
                    status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();co_return finish();
                }
                const auto base=icu_text::image_address(x86_);
                const auto& image=runtime_.builtin_images.at(base);
                const auto target=base+image.export_names.at(icu_text::project_export);
                const auto output=co_await service_callback::call_value(target,{call(1),status.address.value});
                struct Release {
                    WindowsServices& self;
                    GuestMemory& memory;
                    GuestState& state;
                    std::uint64_t address;
                    ~Release(){if(address)(void)self.free_heap(memory,address,state);}
                } release{*this,m,s,output};
                status.value=static_cast<UErrorCode>(signed_argument(m.load(status.address,32)));
                if(U_FAILURE(status.value))co_return finish();
                auto* object=icu_breaks_->find(handle);
                if(!object || !output){
                    status.value=object?U_MEMORY_ALLOCATION_ERROR:U_ILLEGAL_ARGUMENT_ERROR;
                    status.commit();co_return finish();
                }
                const auto projection=load_projection(m,output,status.value);
                if(projection){
                    UText local=UTEXT_INITIALIZER;
                    if(icu_break::open_projection(&local,projection,&status.value)){
                        if(service.symbol=="ubrk_refreshUText")ubrk_refreshUText(object->get(),&local,&status.value);
                        else ubrk_setUText(object->get(),&local,&status.value);
                        if(U_SUCCESS(status.value))object->text.reset();
                    }
                    utext_close(&local);
                }
                status.commit();
                co_return finish();
            });
    }

    const WindowsCall call(state,memory,service);
    const auto arg=[&](unsigned i){return signed_argument(call(i));};
    const auto finish=[&](std::uint64_t value){call.return_to(state,value);return std::optional<std::uint32_t>{};};
    if(name=="ubrk_countAvailable")return finish(static_cast<std::uint32_t>(ubrk_countAvailable()));
    if(name=="ubrk_getAvailable"){
        const auto index=arg(0);
        if(index<0 || index>=ubrk_countAvailable())return finish(0);
        return finish(publish_icu_literal(memory,ubrk_getAvailable(index)));
    }
    auto* object=icu_breaks_->find(call(0));
    const auto* descriptor=std::ranges::find(descriptors,name,[](const auto& item){return item.service.symbol;});
    if(descriptor!=std::end(descriptors) && descriptor->query)
        return finish(descriptor->query(object?object->get():nullptr,call));
    if(name=="ubrk_close"){
        if(object && !object->case_owner){
            icu_breaks_->objects.erase(call(0));
            (void)free_heap(memory,call(0),state);
        }
        return finish(0);
    }
    const auto require_object=[&](IcuStatus& status){
        if(!object && U_SUCCESS(status.value)){
            status.value=U_ILLEGAL_ARGUMENT_ERROR;
            status.commit();
        }
        return object && U_SUCCESS(status.value);
    };
    if(name=="ubrk_getLocaleByType"){
        IcuStatus status(memory,call(2));
        if(!require_object(status))return finish(0);
        const auto text=ubrk_getLocaleByType(object->get(),static_cast<ULocDataLocaleType>(arg(1)),&status.value);
        status.commit();
        return finish(publish_icu_literal(memory,text));
    }
    if(name=="ubrk_getRuleStatusVec"){
        IcuStatus status(memory,call(3));
        if(!require_object(status))return finish(0);
        return finish(icu_output<std::int32_t>(memory,call(1),arg(2),status,IcuOutput::TruncatedArray,
            [&](auto* output,auto capacity,auto* code){
                return ubrk_getRuleStatusVec(object->get(),output,capacity,code);
            }));
    }
    if(name=="ubrk_getBinaryRules"){
        IcuStatus status(memory,call(3));
        if(!require_object(status))return finish(0);
        return finish(icu_output<std::uint8_t>(memory,call(1),arg(2),status,IcuOutput::Array,
            [&](auto* output,auto capacity,auto* code){
                std::uint8_t dummy{};
                return ubrk_getBinaryRules(object->get(),call(1)?(capacity?output:&dummy):nullptr,capacity,code);
            }));
    }
    if(name=="ubrk_setText"){
        IcuStatus status(memory,call(3));
        if(!require_object(status))return finish(0);
        const auto text=load_text(memory,call(1),arg(2),status.value);
        if(text){
            ubrk_setText(object->get(),text->data(),static_cast<std::int32_t>(text->size()),&status.value);
            if(U_SUCCESS(status.value))object->text=text;
        }
        status.commit();
        return finish(0);
    }
    const auto install=[&](BreakObject value,IcuStatus& status){
        if(!value.iterator || U_FAILURE(status.value)){
            status.commit();return finish(0);
        }
        const auto handle=allocate_heap(memory,1);
        if(!handle)status.value=U_MEMORY_ALLOCATION_ERROR;
        else{
            try{icu_breaks_->objects.emplace(handle,std::move(value));}
            catch(...){(void)free_heap(memory,handle,state);throw;}
        }
        status.commit();
        return finish(handle);
    };
    if(name=="ubrk_clone" || name=="ubrk_safeClone"){
        const bool safe=name=="ubrk_safeClone";
        IcuStatus status(memory,call(safe?3U:1U));
        if(!require_object(status))return finish(0);
        const auto size_address=safe?call(2):0;
        if(size_address){
            const auto size=memory.load(GuestAddress{size_address},32);
            memory.store(GuestAddress{size_address},32,1);
            if(!size)return finish(0);
        }
        BreakObject copy;
        copy.binary=object->binary;
        copy.text=object->text;
        copy.iterator.reset(ubrk_clone(object->get(),&status.value));
        if(size_address && copy.iterator && U_SUCCESS(status.value))status.value=U_SAFECLONE_ALLOCATED_WARNING;
        return install(std::move(copy),status);
    }

    const bool rules=name=="ubrk_openRules",binary=name=="ubrk_openBinaryRules";
    IcuStatus status(memory,call(rules?5U:4U));
    if(U_FAILURE(status.value))return finish(0);
    BreakObject opened;
    const auto initial_text=call(2)?load_text(memory,call(2),arg(3),status.value):std::shared_ptr<const std::u16string>{};
    opened.text=initial_text;
    if(U_FAILURE(status.value)){status.commit();return finish(0);}
    const auto* text=initial_text?initial_text->data():nullptr;
    const auto text_length=initial_text?static_cast<std::int32_t>(initial_text->size()):0;
    if(rules){
        const auto source=load_text(memory,call(0),arg(1),status.value);
        UParseError parsed{};
        const auto parse_address=call(4);
        if(source){
            if(parse_address)memory.read(GuestAddress{parse_address},
                {reinterpret_cast<std::uint8_t*>(&parsed),sizeof(parsed)});
            opened.iterator.reset(ubrk_openRules(source->data(),static_cast<std::int32_t>(source->size()),
                text,text_length,parse_address?&parsed:nullptr,&status.value));
            if(parse_address)memory.write(GuestAddress{parse_address},icu_bytes(&parsed,1));
        }
    }else if(binary){
        const auto length=arg(1);
        const auto address=call(0);
        icu_break::RBBIDataHeader header{};
        if(!address || length<static_cast<std::int32_t>(sizeof(header)))status.value=U_ILLEGAL_ARGUMENT_ERROR;
        else{
            memory.read(GuestAddress{address},{reinterpret_cast<std::uint8_t*>(&header),sizeof(header)});
            if(header.fLength>static_cast<std::uint32_t>(length))status.value=U_ILLEGAL_ARGUMENT_ERROR;
            else if(header.fLength<sizeof(header))status.value=U_INVALID_FORMAT_ERROR;
            else{
                const std::size_t size=header.fLength;
                memory.check(GuestAddress{address},size,Access::Read);
                opened.binary=std::make_shared<Rules>((size+3)/4);
                auto* data=reinterpret_cast<std::uint8_t*>(opened.binary->data());
                memory.read(GuestAddress{address},{data,size});
                if(icu_break::valid_binary_rules({data,size}))
                    opened.iterator.reset(ubrk_openBinaryRules(data,static_cast<std::int32_t>(size),text,text_length,&status.value));
                else status.value=U_INVALID_FORMAT_ERROR;
            }
        }
    }else{
        const auto locale=call(1)?read_ansi(memory,call(1)):std::string{};
        opened.iterator.reset(ubrk_open(static_cast<UBreakIteratorType>(arg(0)),call(1)?locale.c_str():icu_default_locale(),
                                        text,text_length,&status.value));
    }
    return install(std::move(opened),status);
}
}

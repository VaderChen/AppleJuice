#include "x64arm/windows_services.hpp"
#include "icu_guest_abi.hpp"
#include "icu_message_format.hpp"
#include <cstring>

namespace x64arm {
namespace {
using namespace icu_abi;
using OwnedMessage=std::unique_ptr<UMessageFormat,decltype(&umsg_close)>;

struct Message {
    OwnedMessage native{nullptr,umsg_close};
    std::uint64_t locale{};
};

static_assert(sizeof(UParseError)==72 && offsetof(UParseError,postContext)==40);
}

struct IcuMessageState {
    std::map<std::uint64_t,Message> objects;
    std::uint64_t empty_locale{};
};

std::span<const WindowsServiceDescriptor> icu_message_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_MESSAGE_API(name,count) {"icu.dll",#name,count,0,0,0,true},
#include "icu_message_services.inc"
#undef ICU_MESSAGE_API
    };
    return entries;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_message(GuestState& state,GuestMemory& memory,
        const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);
    const auto name=service.symbol;
    const auto finish=[&](std::uint64_t result=0){
        call.return_to(state,result);
        return std::optional<std::uint32_t>{};
    };
    const auto i=[&](unsigned arg){return signed_argument(call(arg));};
    const auto object=[&](std::uint64_t handle)->Message*{
        if(!icu_messages_)return nullptr;
        const auto found=icu_messages_->objects.find(handle);
        return found==icu_messages_->objects.end()?nullptr:&found->second;
    };
    const auto forget_locale=[&](Message& message){
        if(const auto address=std::exchange(message.locale,0))(void)free_heap(memory,address,state);
    };
    if(name=="umsg_close" || name=="umsg_setLocale"){
        if(auto* message=object(call(0))){
            if(name=="umsg_close"){
                forget_locale(*message);
                icu_messages_->objects.erase(call(0));
                (void)free_heap(memory,call(0),state);
            }else{
                const auto input=call(1)?read_ansi(memory,call(1)):std::string{};
                umsg_setLocale(message->native.get(),call(1)?input.c_str():icu_default_locale());
                forget_locale(*message);
            }
        }
        return finish();
    }

    if(name=="umsg_getLocale"){
        try{
            auto* message=object(call(0));
            if(!icu_messages_)icu_messages_=std::make_shared<IcuMessageState>();
            auto& slot=message?message->locale:icu_messages_->empty_locale;
            if(slot)return finish(slot);
            const auto* locale=umsg_getLocale(message?message->native.get():nullptr);
            const auto size=std::strlen(locale)+1;
            const auto address=allocate_heap(memory,size);
            if(!address)return finish();
            try{memory.write(GuestAddress{address},icu_bytes(locale,size));}
            catch(...){(void)free_heap(memory,address,state);throw;}
            slot=address;
            return finish(slot);
        }catch(const std::bad_alloc&){return finish();}
    }

    const bool one_shot=name.starts_with("u_");
    const bool parsing=name=="umsg_parse" || name=="umsg_vparse" || name.find("parseMessage")!=name.npos;
    const bool formatting=name=="umsg_format" || name=="umsg_vformat" || name.find("formatMessage")!=name.npos;
    const auto failure_result=formatting || name=="umsg_toPattern" || name=="umsg_autoQuoteApostrophe"?UINT32_MAX:0;
    IcuStatus status(memory,call(service.arguments-1));
    const auto initial=status.value;
    const auto done=[&](std::uint64_t result=0){
        if(status.value!=initial)status.commit();
        return finish(result);
    };
    if(U_FAILURE(initial))return finish(failure_result);
    const auto invalid=[&](){status.value=U_ILLEGAL_ARGUMENT_ERROR;return done(failure_result);};
    const auto open=[&](unsigned pattern,unsigned locale,std::uint64_t parse_address){
        const IcuTextInput<UChar> input(memory,call(pattern),i(pattern+1),status.value);
        if(U_FAILURE(status.value))return OwnedMessage(nullptr,umsg_close);
        const auto locale_text=call(locale)?read_ansi(memory,call(locale)):std::string{};
        UParseError error{};
        read_icu_record(memory,parse_address,error);
        OwnedMessage native(umsg_open(input.get(),input.length,call(locale)?locale_text.c_str():icu_default_locale(),
                                     parse_address?&error:nullptr,&status.value),umsg_close);
        write_icu_record(memory,parse_address,error);
        return native;
    };
    try{
        if(name=="umsg_open" || name=="umsg_clone"){
            Message message;
            if(name=="umsg_open")message.native=open(0,2,call(3));
            else{
                const auto* original=object(call(0));
                if(!original)return invalid();
                message.native.reset(static_cast<UMessageFormat*>(umsg_clone(original->native.get(),&status.value)));
            }
            if(U_FAILURE(status.value) || !message.native)return done();
            if(!icu_messages_)icu_messages_=std::make_shared<IcuMessageState>();
            if(status.value!=initial)memory.check(status.address,4,Access::Write);
            const auto handle=allocate_heap(memory,8);
            if(!handle){status.value=U_MEMORY_ALLOCATION_ERROR;return done();}
            try{icu_messages_->objects.emplace(handle,std::move(message));}
            catch(...){(void)free_heap(memory,handle,state);throw;}
            return done(handle);
        }
        if(name=="umsg_autoQuoteApostrophe"){
            const IcuTextInput<UChar> text(memory,call(0),i(1),status.value);
            if(!text.get() || U_FAILURE(status.value) || i(3)<0 || (!call(2) && i(3)))return invalid();
            return finish(icu_output<UChar>(memory,call(2),i(3),status,IcuOutput::TruncatedString,
                [&](auto* output,auto capacity,auto* error){
                    return umsg_autoQuoteApostrophe(text.get(),text.length,output,capacity,error);
                }));
        }

        const bool indirect=name.starts_with("u_v") || name.starts_with("umsg_v");
        OwnedMessage temporary(nullptr,umsg_close);
        UMessageFormat* native=nullptr;
        if(one_shot){
            // u_vparseMessageWithError 的 va_list 在解析診斷之前，格式化版順序相反。
            const auto diagnostics=name.ends_with("WithError")?call(indirect && parsing?6U:5U):0;
            temporary=open(1,0,diagnostics);
            native=temporary.get();
        }else if(const auto* message=object(call(0)))native=message->native.get();
        if(U_FAILURE(status.value))return done(failure_result);
        if(!native)return invalid();

        if(name=="umsg_applyPattern"){
            const IcuTextInput<UChar> input(memory,call(1),i(2),status.value);
            if(U_FAILURE(status.value))return done();
            UParseError error{};
            read_icu_record(memory,call(3),error);
            umsg_applyPattern(native,input.get(),input.length,call(3)?&error:nullptr,&status.value);
            write_icu_record(memory,call(3),error);
            return done();
        }
        if(name=="umsg_toPattern"){
            const auto capacity=i(2);
            if(capacity<0 || (!call(1) && capacity))return invalid();
            return finish(icu_preserving_output<UChar>(memory,call(1),capacity,status,
                [&](auto* output,auto count,auto* error){return umsg_toPattern(native,output,count,error);}));
        }

        const auto io=one_shot?3U:1U;
        unsigned first=service.arguments;
        if(indirect){
            first=service.arguments-2;
            if(name=="u_vparseMessageWithError")--first;
        }
        icu_message::Arguments args(call,memory,first,indirect);
        if(parsing){
            const auto count=one_shot?0:call(3);
            if(!call(io) || (!one_shot && !count))return invalid();
            const IcuTextInput<UChar> text(memory,call(io),i(io+1),status.value);
            if(U_FAILURE(status.value))return done();
            const auto length=text.length<0?u_strlen(text.get()):text.length;
            icu_message::parse(*native,icu::UnicodeString(text.get(),length),args,memory,count,status.value);
            return done();
        }
        if(formatting){
            const auto capacity=i(io+1);
            if(capacity<0 || (!call(io) && capacity))return invalid();
            const auto message=icu_message::format(*native,args,memory,status.value);
            if(U_FAILURE(status.value))return done(failure_result);
            return finish(icu_output<UChar>(memory,call(io),capacity,status,IcuOutput::String,
                [&](auto* output,auto count,auto* error){return message.extract(output,count,*error);}));
        }
        throw HostCallFault("ICU 訊息格式服務未登錄");
    }catch(const std::bad_alloc&){
        status.value=U_MEMORY_ALLOCATION_ERROR;
        return done(failure_result);
    }
}
}

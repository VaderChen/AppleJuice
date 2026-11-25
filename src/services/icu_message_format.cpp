#include "icu_message_format.hpp"
#include "icu_guest_abi.hpp"
#include <unicode/msgfmt.h>

// 這個型別對應固定 ICU 靜態函式庫的 umsg.cpp 橋接介面。
// 沿用其參數型別表，包含巢狀樣式與未使用的參數槽。
U_NAMESPACE_BEGIN
class MessageFormatAdapter {
public:
    static const Formattable::Type* getArgTypeList(const MessageFormat& m,int32_t& count);
    static UBool hasArgTypeConflicts(const MessageFormat& m) {
        return m.hasArgTypeConflicts;
    }
};
U_NAMESPACE_END

namespace x64arm::icu_message {
Arguments::Arguments(const WindowsCall& call,const GuestMemory& memory,unsigned first,bool indirect)
    :call_(call),memory_(memory),address_(indirect?call(first):0),slot_(first),
     x86_(memory.execution_mode()==ExecutionMode::X86),indirect_(indirect){}

std::uint64_t Arguments::take(bool wide,UErrorCode& status){
    if(U_FAILURE(status))return 0;
    const unsigned bytes=x86_ && !wide?4:8;
    if(!indirect_){
        // Win64 的浮點可變參數在前四槽同時出現在通用暫存器。
        const auto low=call_(slot_++);
        return x86_ && wide?low|(call_(slot_++)<<32):low;
    }
    if(!address_ || address_>UINT64_MAX-bytes){status=U_ILLEGAL_ARGUMENT_ERROR;return 0;}
    const auto value=memory_.load(GuestAddress{address_},static_cast<std::uint8_t>(8*bytes));
    address_+=bytes;
    return value;
}

icu::UnicodeString format(const UMessageFormat& message,Arguments& args,const GuestMemory& memory,UErrorCode& status){
    using icu::Formattable;
    icu::UnicodeString result;
    if(U_FAILURE(status))return result;
    const auto& provider=reinterpret_cast<const icu::MessageFormat&>(message);
    if(provider.usesNamedArguments()){status=U_ARGUMENT_TYPE_MISMATCH;return result;}
    std::int32_t count{};
    const auto* types=icu::MessageFormatAdapter::getArgTypeList(provider,count);
    std::vector<Formattable> values(static_cast<std::size_t>(std::max(count,1)));
    for(std::int32_t i=0;i<count && U_SUCCESS(status);++i){
        auto& value=values[static_cast<std::size_t>(i)];
        switch(types[i]){
        case Formattable::kDate:
            value.setDate(std::bit_cast<double>(args.take(true,status)));
            break;
        case Formattable::kDouble:
            value.setDouble(std::bit_cast<double>(args.take(true,status)));
            break;
        case Formattable::kLong:
            value.setLong(icu_abi::signed_argument(args.take(false,status)));
            break;
        case Formattable::kInt64:
            value.setInt64(std::bit_cast<std::int64_t>(args.take(true,status)));
            break;
        case Formattable::kString:{
            const auto address=args.take(false,status);
            if(!address){status=U_ILLEGAL_ARGUMENT_ERROR;break;}
            const auto text=read_utf16(memory,GuestAddress{address},INT32_MAX);
            value.setString(icu::UnicodeString(text.data(),static_cast<std::int32_t>(text.size())));
            break;
        }
        case Formattable::kArray:
        case Formattable::kObject:
            (void)args.take(false,status);
            break;
        default:
            status=U_ARGUMENT_TYPE_MISMATCH;
            break;
        }
    }
    icu::FieldPosition position(icu::FieldPosition::DONT_CARE);
    if(U_SUCCESS(status))provider.format(values.data(),count,result,position,status);
    if(U_SUCCESS(status) && result.isBogus())status=U_MEMORY_ALLOCATION_ERROR;
    return result;
}

void parse(const UMessageFormat& message,const icu::UnicodeString& text,Arguments& args,GuestMemory& memory,
           std::uint64_t count_address,UErrorCode& status){
    using icu::Formattable;
    if(U_FAILURE(status))return;
    const auto& provider=reinterpret_cast<const icu::MessageFormat&>(message);
    if(provider.usesNamedArguments()){status=U_ARGUMENT_TYPE_MISMATCH;return;}
    std::int32_t count{};
    std::unique_ptr<Formattable[]> values(provider.parse(text,count,status));
    icu_abi::write_icu_record(memory,count_address,count);
    if(U_FAILURE(status) || !values)return;
    auto read_status=U_ZERO_ERROR;
    for(std::int32_t i=0;i<count;++i){
        const auto address=args.take(false,read_status);
        if(!address){status=U_ILLEGAL_ARGUMENT_ERROR;continue;}
        const auto& value=values[static_cast<std::size_t>(i)];
        switch(value.getType()){
        case Formattable::kDate:
            icu_abi::write_icu_record(memory,address,value.getDate());
            break;
        case Formattable::kDouble:
            icu_abi::write_icu_record(memory,address,value.getDouble());
            break;
        case Formattable::kLong:
            icu_abi::write_icu_record(memory,address,value.getLong());
            break;
        case Formattable::kInt64:
            icu_abi::write_icu_record(memory,address,value.getInt64());
            break;
        case Formattable::kString:{
            const auto& string=value.getString();
            const auto length=static_cast<std::size_t>(string.length());
            memory.check(GuestAddress{address},(length+1)*sizeof(UChar),Access::Write);
            if(length)memory.write(GuestAddress{address},icu_abi::icu_bytes(string.getBuffer(),length));
            memory.store(GuestAddress{address+length*sizeof(UChar)},16,0);
            break;
        }
        default:
            status=U_ARGUMENT_TYPE_MISMATCH;
            break;
        }
    }
}
}

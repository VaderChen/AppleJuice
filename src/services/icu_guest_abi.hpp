#pragma once
#include "x64arm/windows_abi.hpp"
#include "x64arm/unicode.hpp"
#include <unicode/utypes.h>
#include <algorithm>
#include <bit>
#include <span>
#include <vector>

namespace x64arm {
void windows_floating_return(GuestState&,bool,double,bool);
}

namespace x64arm::icu_abi {
inline std::int32_t signed_argument(std::uint64_t value){
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));
}

inline double icu_double_argument(const GuestState& state,const WindowsCall& call,bool x86,unsigned index){
    if(x86)return std::bit_cast<double>(call(index));
    std::uint64_t bits{};
    for(unsigned byte=0;byte<8;++byte)bits|=std::uint64_t{state.xmm[index][byte]}<<(8*byte);
    return std::bit_cast<double>(bits);
}

// ICU 的 UErrorCode 與有符號長度在兩種 Windows ABI 都是 32 位元。
struct IcuStatus {
    GuestMemory& memory;
    GuestAddress address;
    UErrorCode value;
    IcuStatus(GuestMemory& guest,std::uint64_t pointer)
        : memory(guest),address{pointer},
          value(pointer?static_cast<UErrorCode>(signed_argument(memory.load(address,32))):U_ILLEGAL_ARGUMENT_ERROR){}
    void commit()const {if(address.value)memory.store(address,32,static_cast<std::uint32_t>(value));}
};

template<class Char>
class IcuTextInput {
public:
    IcuTextInput(const GuestMemory& memory,std::uint64_t address,std::int32_t size,UErrorCode& status):length(size){
        if(U_FAILURE(status))return;
        if(address?size< -1:size!=0){status=U_ILLEGAL_ARGUMENT_ERROR;return;}
        present_=address!=0;
        if(size<0){
            if constexpr(sizeof(Char)==1)text_=read_ansi(memory,address,INT32_MAX);
            else text_=read_utf16(memory,GuestAddress{address},INT32_MAX);
        }else if(size){
            const auto bytes=static_cast<std::size_t>(size)*sizeof(Char);
            memory.check(GuestAddress{address},bytes,Access::Read);
            text_.resize(static_cast<std::size_t>(size));
            memory.read(GuestAddress{address},{reinterpret_cast<std::uint8_t*>(text_.data()),bytes});
        }
    }
    Char* get(){return present_?text_.data():nullptr;}
    const Char* get()const{return present_?text_.c_str():nullptr;}
    std::int32_t length;
private:
    std::basic_string<Char> text_;
    bool present_{};
};

template<class Element>
std::span<const std::uint8_t> icu_bytes(const Element* data,std::size_t size){
    static_assert(std::endian::native==std::endian::little);
    return {reinterpret_cast<const std::uint8_t*>(data),size*sizeof(Element)};
}

template<class T>
void read_icu_record(const GuestMemory& memory,std::uint64_t address,T& result){
    if(address)memory.read(GuestAddress{address},{reinterpret_cast<std::uint8_t*>(&result),sizeof(result)});
}

template<class T>
void write_icu_record(GuestMemory& memory,std::uint64_t address,const T& result){
    if(address)memory.write(GuestAddress{address},icu_bytes(&result,1));
}

enum class IcuOutput {String, TruncatedString, Array, TruncatedArray};

template<class Element,class Operation>
std::uint32_t icu_output(GuestMemory& memory,std::uint64_t address,std::int32_t capacity,IcuStatus& status,
                         IcuOutput kind,Operation operation){
    if(U_FAILURE(status.value))return 0;
    if(capacity<0 || (!address && capacity)){
        status.value=U_ILLEGAL_ARGUMENT_ERROR;
        status.commit();return 0;
    }
    const bool string=kind==IcuOutput::String || kind==IcuOutput::TruncatedString;
    const bool truncated=kind==IcuOutput::TruncatedString || kind==IcuOutput::TruncatedArray;
    const auto required=[&](std::int32_t length){
        return length<0?0:static_cast<std::uint64_t>(length)+(string?1U:0U);
    };
    const auto initial=status.value;
    std::vector<Element> output(static_cast<std::size_t>(std::min(capacity,256)));
    auto length=operation(output.data(),static_cast<std::int32_t>(output.size()),&status.value);
    const auto wanted=std::min<std::uint64_t>(static_cast<std::uint32_t>(capacity),required(length));
    if(wanted>output.size()){
        output.resize(static_cast<std::size_t>(wanted));
        status.value=initial;
        length=operation(output.data(),static_cast<std::int32_t>(output.size()),&status.value);
    }
    if(U_SUCCESS(status.value) || (truncated && status.value==U_BUFFER_OVERFLOW_ERROR)){
        const auto copied=std::min<std::uint64_t>(output.size(),required(length));
        if(copied)memory.write(GuestAddress{address},icu_bytes(output.data(),static_cast<std::size_t>(copied)));
    }
    status.commit();
    return static_cast<std::uint32_t>(length);
}

template<class Element,class Operation>
std::uint32_t icu_preserving_output(GuestMemory& memory,std::uint64_t address,std::int32_t capacity,
        IcuStatus& status,Operation operation,IcuOutput kind=IcuOutput::TruncatedString){
    return icu_output<Element>(memory,address,capacity,status,kind,[&](Element* output,std::int32_t size,UErrorCode* error){
        // 部分 ICU 輸出只覆寫前綴，或在容量不足時保留原始內容。
        if(size)memory.read(GuestAddress{address},
            {reinterpret_cast<std::uint8_t*>(output),static_cast<std::size_t>(size)*sizeof(Element)});
        return operation(output,size,error);
    });
}

}

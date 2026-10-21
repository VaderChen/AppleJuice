#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include "crt_multibyte_state.hpp"

namespace x64arm::crt_multibyte {
enum class StringStatus { Terminated, Limit, Invalid };
struct StringResult {
    StringStatus status{StringStatus::Limit};
    std::size_t source_units{},output_units{};
    State state{};
};

// Reader 以來源單位索引讀取；Writer 以目的單位索引寫入。遇到容量界線時，
// 不消耗未能完整輸出的字元。查詢模式由呼叫者提供不寫入的 Writer。
template<class Reader,class Writer>
StringResult decode_string(State state,std::uint32_t page,std::size_t maximum_bytes,
                           std::size_t limit,Reader read,Writer write){
    StringResult result;result.state=state;
    while(result.output_units<limit){
        std::array<std::uint8_t,8> input{};State trial=state;Result character;
        if(!maximum_bytes || maximum_bytes>input.size())return {StringStatus::Invalid,result.source_units,result.output_units,{}};
        for(std::size_t i=0;i<maximum_bytes;++i){
            input[i]=read(result.source_units+i);trial=state;
            character=decode(trial,std::span(input).first(i+1),page);
            if(character.status!=CrtDecodedCharacter::Status::Partial)break;
        }
        if(character.status!=CrtDecodedCharacter::Status::Complete || character.character>0x10ffff
           || (character.character>=0xd800 && character.character<=0xdfff))
            return {StringStatus::Invalid,result.source_units,result.output_units,{}};
        if(!character.character){write(result.output_units,0);result.status=StringStatus::Terminated;result.state=trial;return result;}
        const auto units=character.character>0xffff?2U:1U;
        if(units>limit-result.output_units)return result;
        if(units==1)write(result.output_units,static_cast<char16_t>(character.character));
        else {
            const auto value=character.character-0x10000;
            write(result.output_units,static_cast<char16_t>(0xd800+(value>>10)));
            write(result.output_units+1,static_cast<char16_t>(0xdc00+(value&0x3ff)));
        }
        result.output_units+=units;result.source_units+=character.consumed;state=trial;result.state=state;
    }return result;
}

template<class Reader,class Writer>
StringResult encode_string(State state,std::uint32_t page,std::size_t limit,Reader read,Writer write){
    StringResult result;result.state=state;
    if(!initial(state))return {StringStatus::Invalid,0,0,{}};
    while(result.output_units<limit){
        std::array<char16_t,2> input{read(result.source_units),0};std::size_t units=1;
        if(input[0]>=0xd800 && input[0]<=0xdbff){
            input[1]=read(result.source_units+1);units=2;
            if(input[1]<0xdc00 || input[1]>0xdfff)return {StringStatus::Invalid,result.source_units,result.output_units,{}};
        }else if(input[0]>=0xdc00 && input[0]<=0xdfff)return {StringStatus::Invalid,result.source_units,result.output_units,{}};
        const auto bytes=windows_crt_encode(std::u16string_view(input.data(),units),page);
        if(!bytes || bytes->empty())return {StringStatus::Invalid,result.source_units,result.output_units,{}};
        if(bytes->size()>limit-result.output_units)return result;
        for(std::size_t i=0;i<bytes->size();++i)write(result.output_units+i,(*bytes)[i]);
        if(!input[0]){result.status=StringStatus::Terminated;result.state={};return result;}
        result.source_units+=units;result.output_units+=bytes->size();
    }return result;
}
}

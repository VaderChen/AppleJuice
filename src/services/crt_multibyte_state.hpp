#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <span>
#include "windows_locale_data.hpp"
namespace x64arm::crt_multibyte {
using State=std::array<std::uint8_t,8>;
inline bool initial(const State& state){return std::all_of(state.begin(),state.end(),[](auto byte){return byte==0;});}
struct Result{CrtDecodedCharacter::Status status{CrtDecodedCharacter::Status::Invalid};std::uint32_t character{};std::size_t consumed{};};
// restartable CRT 與 STL 共用 byte prefix；回傳 consumed 只計本次輸入。
inline Result decode(State& state,std::span<const std::uint8_t> input,std::uint32_t page){
    const auto prefix=state[7]==0x6d?state[6]:0U;
    if(prefix>6 || (state[7] && state[7]!=0x6d) || (!state[7] && !initial(state))){state={};return {};}
    if(input.empty())return {CrtDecodedCharacter::Status::Partial};
    std::array<std::uint8_t,8> bytes{};std::copy_n(state.begin(),prefix,bytes.begin());
    for(std::size_t consumed=0;consumed<input.size() && prefix+consumed<6;){
        bytes[prefix+consumed]=input[consumed];++consumed;
        const auto result=windows_crt_decode(std::span(bytes).first(prefix+consumed),page);
        if(result.status==CrtDecodedCharacter::Status::Invalid){state={};return {};}
        if(result.status==CrtDecodedCharacter::Status::Complete){
            if(result.bytes<prefix || result.bytes>prefix+consumed){state={};return {};}
            state={};return {result.status,result.character,result.bytes-prefix};
        }
        if(consumed==input.size()){
            state={};std::copy_n(bytes.begin(),prefix+consumed,state.begin());state[6]=static_cast<std::uint8_t>(prefix+consumed);state[7]=0x6d;
            return {result.status,0,consumed};
        }
    }state={};return {};
}
}

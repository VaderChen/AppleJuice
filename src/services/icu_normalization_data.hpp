#pragma once

#include "icu_trie_binary.hpp"
#include <norm2allmodes.h>
#include <ucmndata.h>
#include <algorithm>
#include <array>
#include <memory>
#include <string_view>
#include <vector>

namespace x64arm::icu_normalization {
using Bytes=std::span<const std::uint8_t>;
using NormImpl=icu::Normalizer2Impl;

inline Bytes data_payload(Bytes bytes,std::string_view format,std::uint8_t version){
    if(bytes.size()<sizeof(DataHeader))return {};
    DataHeader header{};
    std::memcpy(&header,bytes.data(),sizeof(header));
    const auto length=header.dataHeader.headerSize;
    if(header.dataHeader.magic1!=0xda || header.dataHeader.magic2!=0x27 ||
       length<sizeof(DataHeader) || length>bytes.size() || (length&3) ||
       header.info.size<sizeof(UDataInfo) || header.info.size>length-sizeof(MappedData) ||
       header.info.isBigEndian!=U_IS_BIG_ENDIAN || header.info.charsetFamily!=U_CHARSET_FAMILY ||
       header.info.sizeofUChar!=sizeof(UChar) || header.info.formatVersion[0]!=version ||
       std::string_view(reinterpret_cast<const char*>(header.info.dataFormat),4)!=format)return {};
    return bytes.subspan(length);
}

inline std::uint32_t word32(Bytes bytes,std::size_t offset){
    std::uint32_t result{};
    std::memcpy(&result,bytes.data()+offset,sizeof(result));
    return result;
}

inline Bytes package_entry(Bytes bytes,std::string_view name,UErrorCode& status){
    const auto data=data_payload(bytes,"CmnD",1);
    if(data.size()<4){status=U_INVALID_FORMAT_ERROR;return {};}
    const auto count=word32(data,0);
    if(count>(data.size()-4)/8){status=U_INVALID_FORMAT_ERROR;return {};}
    const auto names_begin=4+std::size_t{count}*8;
    const auto names_end=count?word32(data,8):data.size();
    if(names_end<names_begin || names_end>data.size()){status=U_INVALID_FORMAT_ERROR;return {};}
    for(unsigned i=0;i<count;++i){
        const auto position=4+std::size_t{i}*8;
        const auto name_offset=word32(data,position);
        const auto data_offset=word32(data,position+4);
        const auto end=i+1<count?word32(data,position+12):data.size();
        if(name_offset<names_begin || name_offset>=names_end || data_offset<names_end ||
           (data_offset&3) || data_offset>=end || end>data.size()){
            status=U_INVALID_FORMAT_ERROR;return {};
        }
        const auto* start=reinterpret_cast<const char*>(data.data()+name_offset);
        const auto* terminator=static_cast<const char*>(std::memchr(start,0,names_end-name_offset));
        if(!terminator){status=U_INVALID_FORMAT_ERROR;return {};}
        if(std::string_view(start,static_cast<std::size_t>(terminator-start))==name)
            return data.subspan(data_offset,end-data_offset);
    }
    status=U_FILE_ACCESS_ERROR;
    return {};
}

struct NormalizationIndexes {
    std::array<std::int32_t,NormImpl::IX_COUNT> values{};
    using Index=std::size_t;
    std::size_t operator[](Index index)const{return static_cast<std::size_t>(values[index]);}

    bool valid(Bytes data) const {
        if(data.size()<sizeof(values))return false;
        const auto all=std::span(values);
        if(std::any_of(all.begin(),all.end(),[](auto value){return value<0;}))return false;
        if(!std::is_sorted(all.begin(),all.begin()+8) ||
           (*this)[NormImpl::IX_NORM_TRIE_OFFSET]<sizeof(values) ||
           (*this)[NormImpl::IX_TOTAL_SIZE]>data.size() ||
           (*this)[NormImpl::IX_RESERVED3_OFFSET]-(*this)[NormImpl::IX_SMALL_FCD_OFFSET]<256 ||
           ((*this)[NormImpl::IX_NORM_TRIE_OFFSET]&3) ||
           (((*this)[NormImpl::IX_EXTRA_DATA_OFFSET]|(*this)[NormImpl::IX_SMALL_FCD_OFFSET])&1))return false;
        const std::array thresholds{(*this)[NormImpl::IX_MIN_YES_NO],(*this)[NormImpl::IX_MIN_YES_NO_MAPPINGS_ONLY],
            (*this)[NormImpl::IX_MIN_NO_NO],(*this)[NormImpl::IX_MIN_NO_NO_COMP_BOUNDARY_BEFORE],
            (*this)[NormImpl::IX_MIN_NO_NO_COMP_NO_MAYBE_CC],(*this)[NormImpl::IX_MIN_NO_NO_EMPTY],
            (*this)[NormImpl::IX_LIMIT_NO_NO],(*this)[NormImpl::IX_MIN_MAYBE_NO],
            (*this)[NormImpl::IX_MIN_MAYBE_NO_COMBINES_FWD],(*this)[NormImpl::IX_MIN_MAYBE_YES]};
        return std::is_sorted(thresholds.begin(),thresholds.end()) && thresholds.front()>NormImpl::JAMO_L &&
            thresholds.back()<=NormImpl::MIN_NORMAL_MAYBE_YES && !(thresholds[7]&7) &&
            values[NormImpl::IX_MIN_DECOMP_NO_CP]<=0xffff && values[NormImpl::IX_MIN_COMP_NO_MAYBE_CP]<=0xffff &&
            values[NormImpl::IX_MIN_LCCC_CP]<=0xffff;
    }
};

inline bool valid_records(const NormalizationIndexes& ix,const UCPTrie& trie,std::span<const std::uint16_t> extra){
    const auto yes_no=ix[NormImpl::IX_MIN_YES_NO];
    const auto mappings_only=ix[NormImpl::IX_MIN_YES_NO_MAPPINGS_ONLY];
    const auto limit_no=ix[NormImpl::IX_LIMIT_NO_NO];
    const auto maybe_no=ix[NormImpl::IX_MIN_MAYBE_NO];
    const auto maybe_fwd=ix[NormImpl::IX_MIN_MAYBE_NO_COMBINES_FWD];
    const auto maybe_yes=ix[NormImpl::IX_MIN_MAYBE_YES];
    const auto center=static_cast<std::int32_t>(maybe_no>>NormImpl::DELTA_SHIFT)-NormImpl::MAX_DELTA-1;
    const auto data_index=[&](std::uint32_t value){
        return (value>=maybe_no?value-maybe_no+limit_no:value)>>NormImpl::OFFSET_SHIFT;
    };
    const auto mapping_end=[&](std::size_t begin)->std::size_t {
        if(begin>=extra.size())return 0;
        const auto unit=extra[begin];
        const auto length=static_cast<std::size_t>(unit&NormImpl::MAPPING_LENGTH_MASK);
        const auto cc=(unit&NormImpl::MAPPING_HAS_CCC_LCCC_WORD)?1U:0U;
        const auto end=begin+1+length;
        if(end>extra.size() || begin<cc)return 0;
        if(unit&NormImpl::MAPPING_HAS_RAW_MAPPING){
            if(begin<=cc)return 0;
            const auto raw=begin-cc-1;
            const auto marker=extra[raw];
            if(marker<=NormImpl::MAPPING_LENGTH_MASK?raw<marker:length<2)return 0;
        }
        return end;
    };
    const auto valid_list=[&](std::size_t start){
        for(auto offset=start;offset+1<extra.size();){
            const auto first=extra[offset];
            const auto trail=first&NormImpl::COMP_1_TRAIL_MASK;
            const bool triple=(first&NormImpl::COMP_1_TRIPLE)!=0;
            if(triple && offset+2>=extra.size())return false;
            const auto result=triple?
                (std::uint32_t(extra[offset+1]&(trail>=NormImpl::COMP_1_TRAIL_LIMIT?0x3f:0xffff))<<16)|extra[offset+2]:
                extra[offset+1];
            if(result>0x21ffff)return false;
            if(result&1){
                const auto target=ucptrie_get(&trie,static_cast<UChar32>(result>>1));
                const bool forward=(target>yes_no && target<mappings_only) || (target>=maybe_fwd && target<maybe_yes);
                if(!forward)return false;
            }
            if(first&NormImpl::COMP_1_LAST_TUPLE)return true;
            offset+=triple?3:2;
        }
        return false;
    };
    // Jamo L 的保留項目是空列表標記；Hangul 組合由 ICU 的演算法處理。
    if(extra.size()<2 || extra[1]!=0xffff || ucptrie_get(&trie,-1)!=NormImpl::INERT)return false;
    std::array<bool,65536> checked{};
    for(UChar32 cp=0;cp<=0x10ffff;++cp){
        const auto value=ucptrie_get(&trie,cp);
        if(value>=limit_no && value<maybe_no){
            // 差值只會指向已正規化且 combining class 為零的有效碼點。
            const auto target=cp+static_cast<std::int32_t>(value>>NormImpl::DELTA_SHIFT)-center;
            if(static_cast<std::uint32_t>(target)>0x10ffff ||
               ucptrie_get(&trie,target)>=ix[NormImpl::IX_MIN_NO_NO])return false;
            continue;
        }
        if(checked[value])continue;
        checked[value]=true;
        if(value<=NormImpl::JAMO_L || value>=NormImpl::MIN_NORMAL_MAYBE_YES)continue;
        auto position=data_index(value);
        if((value>=yes_no && value<limit_no) || (value>=maybe_no && value<maybe_yes)){
            position=mapping_end(position);
            if(!position)return false;
        }
        if(value<yes_no || (value>yes_no && value<mappings_only) || value>=maybe_fwd){
            if(!valid_list(position))return false;
        }
    }
    return true;
}

class NormalizationData final : public NormImpl {
public:
    std::vector<std::uint8_t> bytes;
    std::unique_ptr<UCPTrie,decltype(&ucptrie_close)> trie{nullptr,ucptrie_close};
};

inline std::shared_ptr<const icu::Norm2AllModes> load(std::vector<std::uint8_t> file,UErrorCode& status){
    auto owned=std::make_unique<NormalizationData>();
    if(!owned){status=U_MEMORY_ALLOCATION_ERROR;return {};}
    owned->bytes=std::move(file);
    const auto data=data_payload(owned->bytes,"Nrm2",5);
    NormalizationIndexes ix;
    if(data.size()<sizeof(ix.values)){status=U_INVALID_FORMAT_ERROR;return {};}
    std::memcpy(ix.values.data(),data.data(),sizeof(ix.values));
    if(!ix.valid(data)){status=U_INVALID_FORMAT_ERROR;return {};}
    const auto trie=ix[NormImpl::IX_NORM_TRIE_OFFSET],extra=ix[NormImpl::IX_EXTRA_DATA_OFFSET],
        fcd=ix[NormImpl::IX_SMALL_FCD_OFFSET];
    const auto trie_bytes=data.subspan(trie,extra-trie);
    if(!icu_trie::valid_binary(trie_bytes,UCPTRIE_TYPE_FAST,UCPTRIE_VALUE_BITS_16)){
        status=U_INVALID_FORMAT_ERROR;return {};
    }
    owned->trie.reset(ucptrie_openFromBinary(UCPTRIE_TYPE_FAST,UCPTRIE_VALUE_BITS_16,trie_bytes.data(),
        static_cast<std::int32_t>(trie_bytes.size()),nullptr,&status));
    if(!owned->trie || U_FAILURE(status))return {};
    const auto* mapping=reinterpret_cast<const std::uint16_t*>(data.data()+extra);
    if(!valid_records(ix,*owned->trie,{mapping,(fcd-extra)/2})){
        status=U_INVALID_FORMAT_ERROR;return {};
    }
    owned->init(ix.values.data(),owned->trie.get(),mapping,data.data()+fcd);
    return std::shared_ptr<const icu::Norm2AllModes>(icu::Norm2AllModes::createInstance(owned.release(),status));
}
}

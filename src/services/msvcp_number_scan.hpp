#pragma once
#include "msvcp_numeric.hpp"
#include <array>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <locale.h>
#if defined(__APPLE__)
#include <xlocale.h>
#endif
#include <new>
#include <string>
#include <utility>

namespace x64arm::msvcp_detail {
// VC10／VC14 operator>> 共用的 num_get 欄位：逐字元餵入，回傳 false 的字元留在 streambuf。
// 整數依 basefield（0 為自動判定）收集，浮點接受十進位與 0x 前綴；group 不符時保留已轉換的值並回報失敗。
// 溢位存放型別上下限並失敗（C++ [facet.num.get.virtuals]），欄位無數字時存 0 並失敗。
class NumberField {
public:
    struct Result {std::uint64_t bits{};bool failed{};};
    void configure(NumberType type,std::uint32_t flags,NumericPunctuation punctuation,unsigned pointer_bytes){
        type_=type;pointer_bytes_=pointer_bytes;punctuation_=std::move(punctuation);
        names_=type==NumberType::Boolean && (flags&0x4000U);
        floating_=type==NumberType::Floating32 || type==NumberType::Floating64;
        grouped_=!punctuation_.grouping.empty() && punctuation_.thousands;
        const auto basefield=flags&0xe00U;
        base_=type==NumberType::Pointer || basefield==0x800U?16U:basefield==0x400U?8U:basefield==0?0U:10U;
    }
    std::uint8_t store_bits()const{
        switch(type_){
        case NumberType::Boolean:return 8;
        case NumberType::Signed16:case NumberType::Unsigned16:return 16;
        case NumberType::Signed32:case NumberType::Unsigned32:case NumberType::Floating32:return 32;
        case NumberType::Pointer:return static_cast<std::uint8_t>(pointer_bytes_*8);
        default:return 64;
        }
    }
    bool accept(char16_t c){return names_?accept_name(c,false):floating_?accept_float(c):accept_integer(c);}
    Result conclude(bool end_of_input){
        if(names_){
            if(end_of_input)(void)accept_name(0,true);
            return answer_>=0?Result{static_cast<std::uint64_t>(answer_),false}:Result{0,true};
        }
        auto result=floating_?floating():integer();
        if(bad_ || !groups_valid())result.failed=true;
        return result;
    }
private:
    enum class Stage {Sign,AfterSign,Prefix,Digits,Fraction,ExponentSign,Exponent};
    static constexpr std::size_t significant_limit=768;
    NumberType type_{NumberType::Signed32};
    unsigned pointer_bytes_{8},base_{10};
    NumericPunctuation punctuation_;
    bool names_{},floating_{},grouped_{},negative_{},seen_{},bad_{},overflow_{},hex_{},sticky_{},exponent_negative_{};
    Stage stage_{Stage::Sign};
    std::uint64_t magnitude_{};
    std::string groups_=std::string(1,'\0'),digits_;
    long long scale_{},exponent_{};
    std::size_t column_{};
    std::array<bool,2> closed_{};
    int answer_{-2};
    static unsigned digit_value(char16_t c){
        if(c>=u'0' && c<=u'9')return static_cast<unsigned>(c-u'0');
        if(c>=u'a' && c<=u'f')return static_cast<unsigned>(c-u'a')+10;
        if(c>=u'A' && c<=u'F')return static_cast<unsigned>(c-u'A')+10;
        return 99;
    }
    bool separator(char16_t c)const{return grouped_ && c==punctuation_.thousands;}
    void count_digit(){if(groups_.back()<CHAR_MAX)++groups_.back();}
    bool accept_separator(char16_t c){
        if(!separator(c))return false;
        if(!groups_.back())bad_=true;else groups_.push_back('\0');
        return true;
    }
    // 自最右群比對 grouping（最後一個值重複使用）；最左群可較短，CHAR_MAX 或 0 表示不再限制。
    bool groups_valid()const{
        if(groups_.size()<2)return true;
        if(!groups_.back())return false;
        auto group=groups_.size();std::size_t rule=0;
        while(group){
            const auto expected=punctuation_.grouping[rule];
            if(expected==CHAR_MAX || expected<=0)break;
            --group;
            if(group?expected!=groups_[group]:expected<groups_[group])return false;
            if(rule+1<punctuation_.grouping.size() && punctuation_.grouping[rule+1]>0)++rule;
        }
        return true;
    }
    bool accept_sign(char16_t c){
        stage_=Stage::AfterSign;
        if(c==u'+')return true;
        if(c==u'-'){negative_=true;return true;}
        return false;
    }
    bool accept_integer(char16_t c){
        for(;;)switch(stage_){
        case Stage::Sign:
            if(separator(c)){bad_=true;return true;}
            if(accept_sign(c))return true;
            break;
        case Stage::AfterSign:
            if(separator(c)){bad_=true;return true;}
            stage_=Stage::Digits;
            if(c==u'0'){seen_=true;groups_[0]=1;stage_=Stage::Prefix;return true;}
            break;
        case Stage::Prefix:
            stage_=Stage::Digits;
            if((c==u'x' || c==u'X') && (base_==0 || base_==16)){base_=16;seen_=false;groups_[0]=0;return true;}
            if(!base_)base_=8;
            break;
        default:{
            const auto radix=base_?base_:10U;const auto digit=digit_value(c);
            if(digit>=radix)return accept_separator(c);
            seen_=true;count_digit();
            if(magnitude_>(UINT64_MAX-digit)/radix)overflow_=true;else magnitude_=magnitude_*radix+digit;
            return true;
        }}
    }
    Result integer()const{
        if(!seen_)return {0,true};
        const bool boolean=type_==NumberType::Boolean;
        const bool signed_value=boolean || type_==NumberType::Signed16 || type_==NumberType::Signed32 || type_==NumberType::Signed64;
        const unsigned width=boolean?32U:type_==NumberType::Pointer?pointer_bytes_*8:static_cast<unsigned>(store_bits());
        if(signed_value){
            const auto maximum=(UINT64_C(1)<<(width-1))-1,lowest=maximum+1;
            const bool over=overflow_ || magnitude_>(negative_?lowest:maximum);
            const auto value=over?(negative_?UINT64_C(0)-lowest:maximum):negative_?UINT64_C(0)-magnitude_:magnitude_;
            if(boolean)return {value!=0,over || (value!=0 && value!=1)};
            return {value,over};
        }
        const auto maximum=width==64?UINT64_MAX:(UINT64_C(1)<<width)-1;
        if(overflow_ || magnitude_>maximum)return {maximum,true};
        return {(negative_?UINT64_C(0)-magnitude_:magnitude_)&maximum,false};
    }
    bool accept_exponent_mark(char16_t c){
        if(!seen_ || !(hex_?c==u'p' || c==u'P':c==u'e' || c==u'E'))return false;
        seen_=false;stage_=Stage::ExponentSign;return true;
    }
    bool accept_float(char16_t c){
        for(;;)switch(stage_){
        case Stage::Sign:
            if(separator(c)){bad_=true;return true;}
            if(accept_sign(c))return true;
            break;
        case Stage::AfterSign:
            if(separator(c)){bad_=true;return true;}
            stage_=Stage::Digits;
            if(c==u'0'){seen_=true;groups_[0]=1;stage_=Stage::Prefix;return true;}
            break;
        case Stage::Prefix:
            stage_=Stage::Digits;
            if(c==u'x' || c==u'X'){hex_=true;seen_=false;groups_[0]=0;return true;}
            break;
        case Stage::Digits:{
            const auto digit=digit_value(c);
            if(digit<(hex_?16U:10U)){
                seen_=true;count_digit();
                if(digits_.size()>=significant_limit){++scale_;sticky_=sticky_ || digit;}
                else if(!digits_.empty() || digit)digits_.push_back(static_cast<char>(c));
                return true;
            }
            if(accept_separator(c))return true;
            if(c==punctuation_.decimal){stage_=Stage::Fraction;return true;}
            return accept_exponent_mark(c);
        }
        case Stage::Fraction:{
            const auto digit=digit_value(c);
            if(digit>=(hex_?16U:10U))return accept_exponent_mark(c);
            seen_=true;
            if(digits_.empty() && !digit)--scale_;
            else if(digits_.size()>=significant_limit)sticky_=sticky_ || digit;
            else{digits_.push_back(static_cast<char>(c));--scale_;}
            return true;
        }
        case Stage::ExponentSign:
            stage_=Stage::Exponent;
            if(c==u'+')return true;
            if(c==u'-'){exponent_negative_=true;return true;}
            break;
        default:
            if(c<u'0' || c>u'9')return false;
            seen_=true;if(exponent_<1000000000)exponent_=exponent_*10+(c-u'0');
            return true;
        }
    }
    // 由宿主 C locale 轉換，不受宿主目前 locale 影響；上下溢以結果判定，不依賴各平台的 errno 差異。
    Result floating()const{
        if(!seen_)return {0,true};
        std::string text;if(negative_)text.push_back('-');if(hex_)text+="0x";
        text+=digits_.empty()?std::string("0"):digits_;
        auto scale=scale_;if(sticky_){text.push_back('1');--scale;}
        auto exponent=(exponent_negative_?-exponent_:exponent_)+(hex_?scale*4:scale);
        exponent=std::clamp<long long>(exponent,-1000000,1000000);
        text.push_back(hex_?'p':'e');text+=std::to_string(exponent);
        static const auto locale=newlocale(LC_NUMERIC_MASK,"C",nullptr);if(!locale)throw std::bad_alloc();
        const bool nonzero=digits_.find_first_not_of('0')!=digits_.npos;
        const auto saved=errno;Result result;
        if(type_==NumberType::Floating32){
            auto value=strtof_l(text.c_str(),nullptr,locale);
            result.failed=std::isinf(value) || (value==0 && nonzero);
            if(std::isinf(value))value=std::copysign(std::numeric_limits<float>::max(),value);
            result.bits=std::bit_cast<std::uint32_t>(value);
        }else{
            auto value=strtod_l(text.c_str(),nullptr,locale);
            result.failed=std::isinf(value) || (value==0 && nonzero);
            if(std::isinf(value))value=std::copysign(std::numeric_limits<double>::max(),value);
            result.bits=std::bit_cast<std::uint64_t>(value);
        }
        errno=saved;return result;
    }
    // _Getloctxt 的逐欄比對：完整名稱成為候選，仍有較長名稱可比時繼續；比對失敗的字元留在 streambuf。
    bool accept_name(char16_t c,bool end){
        const std::array<const std::u16string*,2> fields{&punctuation_.false_name,&punctuation_.true_name};
        bool prefix=false;
        for(int field=0;field<2;++field){
            auto& closed=closed_[static_cast<std::size_t>(field)];if(closed)continue;
            const auto& text=*fields[static_cast<std::size_t>(field)];
            if(column_>=text.size()){closed=true;answer_=field;}
            else if(end || text[column_]!=c)closed=true;
            else prefix=true;
        }
        if(!prefix || end)return false;
        ++column_;answer_=-1;return true;
    }
};
}

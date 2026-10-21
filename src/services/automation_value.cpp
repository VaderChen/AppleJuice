#include "automation_value.hpp"
#include <openssl/bn.h>
#include <algorithm>
#include <bit>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <locale.h>

namespace x64arm::automation {
std::uint64_t get(const Value& v,unsigned at,unsigned bytes){std::uint64_t result=0;for(unsigned i=0;i<bytes;++i)result|=std::uint64_t(v[at+i])<<(8*i);return result;}
void put(Value& v,unsigned at,unsigned bytes,std::uint64_t n){for(unsigned i=0;i<bytes;++i)v[at+i]=static_cast<std::uint8_t>(n>>(8*i));}
bool valid_type(std::uint16_t t,bool refs){
    if(t&0x9000U || (!refs && (t&0x6000U)))return false;
    const auto b=t&0xfffU;if(b==15 || b>23)return false;
    if((t&0x6000U) && b<=1)return false;
    return b!=12 || (t&0x6000U);
}
namespace {
#if defined(__APPLE__)
locale_t numeric_locale(){static locale_t value=newlocale(LC_NUMERIC_MASK,"C",nullptr);if(!value)throw std::bad_alloc();return value;}
#endif
using Big=std::unique_ptr<BIGNUM,decltype(&BN_free)>;
using Context=std::unique_ptr<BN_CTX,decltype(&BN_CTX_free)>;
void checked(int result){if(!result)throw std::bad_alloc();}
Big big(){Big result(BN_new(),BN_free);if(!result)throw std::bad_alloc();return result;}
Big integer(std::uint64_t n){auto result=big();std::array<unsigned char,8> bytes{};for(unsigned i=0;i<8;++i)bytes[7-i]=static_cast<unsigned char>(n>>(i*8));if(!BN_bin2bn(bytes.data(),8,result.get()))throw std::bad_alloc();return result;}
Context context(){Context result(BN_CTX_new(),BN_CTX_free);if(!result)throw std::bad_alloc();return result;}
Big power(unsigned scale){auto result=integer(1);for(unsigned i=0;i<scale;++i)checked(BN_mul_word(result.get(),10));return result;}
std::string decimal_text(const BIGNUM* n){char* raw=BN_bn2dec(n);if(!raw)throw std::bad_alloc();std::unique_ptr<char,void(*)(char*)> owned(raw,[](char* p){OPENSSL_free(p);});return raw;}
struct Number {Big numerator=integer(0),denominator=integer(1);bool negative{};};
Number from_decimal(std::string_view s){
    Number result;std::size_t i=0;if(i<s.size() && (s[i]=='-' || s[i]=='+'))result.negative=s[i++]=='-';
    bool dot=false,any=false;int scale=0;
    for(;i<s.size() && s[i]!='e' && s[i]!='E';++i){if(s[i]=='.' && !dot){dot=true;continue;}if(s[i]<'0' || s[i]>'9')throw std::invalid_argument("數值格式不合法");any=true;checked(BN_mul_word(result.numerator.get(),10));checked(BN_add_word(result.numerator.get(),static_cast<unsigned>(s[i]-'0')));if(dot)++scale;}
    if(!any)throw std::invalid_argument("數值為空");
    if(i<s.size()){++i;bool minus=false;if(i<s.size() && (s[i]=='-' || s[i]=='+'))minus=s[i++]=='-';if(i==s.size())throw std::invalid_argument("指數為空");int exponent=0;for(;i<s.size();++i){if(s[i]<'0' || s[i]>'9')throw std::invalid_argument("指數不合法");exponent=std::min(100000,exponent*10+s[i]-'0');}scale+=minus?exponent:-exponent;}
    if(std::abs(scale)>100000)throw std::range_error("數值指數超過範圍");
    if(scale>=0)result.denominator=power(static_cast<unsigned>(scale));
    else {const auto p=power(static_cast<unsigned>(-scale));auto ctx=context();checked(BN_mul(result.numerator.get(),result.numerator.get(),p.get(),ctx.get()));}
    if(BN_is_zero(result.numerator.get()))result.negative=false;return result;
}
Number number(const Value& v,std::string_view text={}){
    const auto type=get(v,0,2);if(type==8)return from_decimal(text);Number n;auto raw=get(v,8,8);unsigned width=0;bool signed_value=false;
    switch(type){
    case 0:return n;
    case 2:width=16;signed_value=true;break;case 3:case 22:width=32;signed_value=true;break;
    case 16:width=8;signed_value=true;break;case 17:width=8;break;case 18:width=16;break;case 19:case 23:width=32;break;
    case 20:case 6:width=64;signed_value=true;break;case 21:width=64;break;
    case 11:width=16;signed_value=true;break;
    case 14:{
        const unsigned scale=v[2],sign=v[3];if(scale>28 || (sign!=0 && sign!=128))throw std::invalid_argument("DECIMAL 格式不合法");
        std::array<unsigned char,12> bytes{};for(unsigned i=0;i<4;++i)bytes[3-i]=v[4+i];for(unsigned i=0;i<8;++i)bytes[11-i]=v[8+i];
        if(!BN_bin2bn(bytes.data(),12,n.numerator.get()))throw std::bad_alloc();n.denominator=power(scale);n.negative=sign && !BN_is_zero(n.numerator.get());return n;
    }
    case 4:case 5:case 7:{
        const bool single=type==4;const unsigned mantissa=single?23:52,exponentbits=single?8:11;
        if(single)raw&=UINT32_MAX;const auto exponent=(raw>>mantissa)&((UINT64_C(1)<<exponentbits)-1);const auto fraction=raw&((UINT64_C(1)<<mantissa)-1);
        if(exponent==((UINT64_C(1)<<exponentbits)-1))throw std::range_error("非有限浮點值");
        n.negative=(raw>>(mantissa+exponentbits))!=0;
        const auto coefficient=fraction|(exponent?UINT64_C(1)<<mantissa:0);
        n.numerator=integer(coefficient);int shift=static_cast<int>(exponent?exponent:1)-(single?127:1023)-static_cast<int>(mantissa);
        if(shift>=0)checked(BN_lshift(n.numerator.get(),n.numerator.get(),shift));else checked(BN_lshift(n.denominator.get(),n.denominator.get(),-shift));
        if(!coefficient)n.negative=false;return n;
    }
    default:throw std::invalid_argument("無法轉為數值的型別");
    }
    if(width<64)raw&=(UINT64_C(1)<<width)-1;
    n.negative=signed_value && (raw&(UINT64_C(1)<<(width-1)));
    if(n.negative){raw=UINT64_C(0)-raw;if(width<64)raw&=(UINT64_C(1)<<width)-1;}
    n.numerator=integer(raw);if(type==6)n.denominator=power(4);return n;
}
// 對精確有理數採用 Automation 的 ties-to-even，避免宿主浮點環境影響 CY／DECIMAL。
Big rounded(const Number& n,unsigned scale){
    auto p=power(scale),scaled=big(),q=big(),r=big();auto ctx=context();
    checked(BN_mul(scaled.get(),n.numerator.get(),p.get(),ctx.get()));checked(BN_div(q.get(),r.get(),scaled.get(),n.denominator.get(),ctx.get()));
    checked(BN_lshift1(r.get(),r.get()));const auto cmp=BN_cmp(r.get(),n.denominator.get());if(cmp>0 || (cmp==0 && BN_is_odd(q.get())))checked(BN_add_word(q.get(),1));return q;
}
std::uint64_t u64(const BIGNUM* n){std::array<unsigned char,8> bytes{};if(BN_bn2binpad(n,bytes.data(),8)!=8)throw std::range_error("64 位元溢位");std::uint64_t value=0;for(auto c:bytes)value=(value<<8)|c;return value;}
std::string fixed(const BIGNUM* n,unsigned scale,bool negative){auto s=decimal_text(n);if(scale){if(s.size()<=scale)s.insert(0,scale+1-s.size(),'0');s.insert(s.size()-scale,1,'.');while(s.back()=='0')s.pop_back();if(s.back()=='.')s.pop_back();}if(negative && !BN_is_zero(n))s.insert(s.begin(),'-');return s;}
std::string floating_text(const Number& n){
    // 先產生足夠精度的十進位，再由原生 strtod 完成一次 IEEE-754 捨入。
    const auto numerator=decimal_text(n.numerator.get()),denominator=decimal_text(n.denominator.get());
    const int exponent=static_cast<int>(numerator.size())-static_cast<int>(denominator.size());
    const unsigned scale=static_cast<unsigned>(std::max(0,1100-exponent));const auto coefficient=rounded(n,scale);return fixed(coefficient.get(),scale,n.negative);
}
double floating(const Number& n){
    const auto s=floating_text(n);char* end=nullptr;
#if defined(__APPLE__)
    const double result=strtod_l(s.c_str(),&end,numeric_locale());
#else
    const double result=std::strtod(s.c_str(),&end);
#endif
    if(!end || *end || !std::isfinite(result))throw std::range_error("浮點溢位");return result;
}
float single(const Number& n){
    const auto s=floating_text(n);char* end=nullptr;
#if defined(__APPLE__)
    const float result=strtof_l(s.c_str(),&end,numeric_locale());
#else
    const float result=std::strtof(s.c_str(),&end);
#endif
    if(!end || *end || !std::isfinite(result))throw std::range_error("單精度浮點溢位");return result;
}
}
std::uint32_t number_convert(const Value& source,std::string_view decimal,std::uint16_t target,Value& output){
    try{
        auto n=number(source,decimal);output={};put(output,0,2,target);unsigned bits=0;bool unsign=false;unsigned scale=0;
        switch(target){case 2:bits=16;break;case 3:case 22:bits=32;break;case 16:bits=8;break;case 17:bits=8;unsign=true;break;case 18:bits=16;unsign=true;break;case 19:case 23:bits=32;unsign=true;break;case 20:bits=64;break;case 21:bits=64;unsign=true;break;case 6:bits=64;scale=4;break;
        case 11:put(output,8,2,BN_is_zero(n.numerator.get())?0:0xffff);return 0;
        case 4:case 5:case 7:{
            const auto from=get(source,0,2);const bool minus_zero=BN_is_zero(n.numerator.get()) && ((from==4 && (get(source,8,4)>>31)) || ((from==5 || from==7) && (get(source,8,8)>>63)) || (from==8 && !decimal.empty() && decimal.front()=='-'));
            if(target==4){const auto f=single(n);put(output,8,4,minus_zero?UINT64_C(1)<<31:std::bit_cast<std::uint32_t>(f));}
            else {const auto d=floating(n);if(target==7 && (d<=-657435.0 || d>=2958466.0))return overflow;put(output,8,8,minus_zero?UINT64_C(1)<<63:std::bit_cast<std::uint64_t>(d));}return 0;
        }
        case 14:{
            Big coefficient(nullptr,BN_free);unsigned s=28;for(;;){coefficient=rounded(n,s);if(BN_num_bits(coefficient.get())<=96)break;if(!s)return overflow;--s;}
            while(s && !BN_is_zero(coefficient.get()) && BN_mod_word(coefficient.get(),10)==0){BN_div_word(coefficient.get(),10);--s;}if(BN_is_zero(coefficient.get()))s=0;
            std::array<unsigned char,12> bytes{};if(BN_bn2binpad(coefficient.get(),bytes.data(),12)!=12)return overflow;
            output[2]=static_cast<std::uint8_t>(s);output[3]=n.negative && !BN_is_zero(coefficient.get())?128:0;for(unsigned i=0;i<4;++i)output[4+i]=bytes[3-i];for(unsigned i=0;i<8;++i)output[8+i]=bytes[11-i];return 0;
        }
        default:return mismatch;
        }
        if(unsign && get(source,0,2)==11 && n.negative){put(output,8,bits/8,bits==64?UINT64_MAX:(UINT64_C(1)<<bits)-1);return 0;}
        const auto coefficient=rounded(n,scale);auto bound=integer(1);checked(BN_lshift(bound.get(),bound.get(),static_cast<int>(unsign?bits:bits-1)));
        if(unsign || !n.negative)checked(BN_sub_word(bound.get(),1));if(BN_cmp(coefficient.get(),bound.get())>0 || (unsign && n.negative && !BN_is_zero(coefficient.get())))return overflow;
        const auto magnitude=u64(coefficient.get());put(output,8,bits/8,n.negative?UINT64_C(0)-magnitude:magnitude);return 0;
    }catch(const std::invalid_argument&){return mismatch;}catch(const std::range_error&){return overflow;}catch(const std::bad_alloc&){return nomemory;}
}
std::uint32_t number_compare(const Value& a,const Value& b,std::string_view left_text,std::string_view right_text){
    try{const auto left=number(a,left_text),right=number(b,right_text);if(left.negative!=right.negative)return left.negative?0:2;auto x=big(),y=big();auto ctx=context();checked(BN_mul(x.get(),left.numerator.get(),right.denominator.get(),ctx.get()));checked(BN_mul(y.get(),right.numerator.get(),left.denominator.get(),ctx.get()));int c=BN_cmp(x.get(),y.get());if(left.negative)c=-c;return c<0?0:c>0?2:1;}catch(const std::invalid_argument&){return mismatch;}catch(const std::range_error&){return overflow;}catch(const std::bad_alloc&){return nomemory;}
}
std::string number_string(const Value& v){
    const auto t=get(v,0,2);if(t==4 || t==5){const auto d=t==4?static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(get(v,8,4)))):std::bit_cast<double>(get(v,8,8));if(!std::isfinite(d))throw std::range_error("非有限浮點值");std::array<char,64> buffer{};
#if defined(__APPLE__)
        const int length=snprintf_l(buffer.data(),buffer.size(),numeric_locale(),"%.*g",t==4?7:15,d);
#else
        const int length=std::snprintf(buffer.data(),buffer.size(),"%.*g",t==4?7:15,d);
#endif
        if(length<=0 || static_cast<std::size_t>(length)>=buffer.size())throw std::range_error("數值格式化失敗");return std::string(buffer.data(),static_cast<std::size_t>(length));}
    const auto n=number(v);const unsigned scale=t==6?4U:t==14?v[2]:0U;const auto coefficient=rounded(n,scale);return fixed(coefficient.get(),scale,n.negative);
}
std::optional<std::array<unsigned,8>> time_fields(double value){
    using namespace std::chrono;if(!std::isfinite(value) || value<=-657435.0 || value>=2958466.0)return {};
    const auto integral=std::trunc(value);auto seconds=static_cast<unsigned>(std::floor(std::abs(value-integral)*86400.0+0.5000009));auto date=sys_days{year{1899}/12/30}+days{static_cast<int>(integral)};if(seconds>=86400){date+=days{1};seconds=0;}
    const year_month_day ymd{date};const int y=int(ymd.year());if(y<100 || y>9999)return {};return std::array<unsigned,8>{static_cast<unsigned>(y),unsigned(ymd.month()),weekday{date}.c_encoding(),unsigned(ymd.day()),seconds/3600,(seconds/60)%60,seconds%60,0};
}
std::optional<double> date_value(std::array<unsigned,8> fields){
    using namespace std::chrono;auto y=fields[0];if(y<100)y+=y<30?2000:1900;if(y>9999 || !fields[1] || fields[1]>12 || fields[3]>31 || fields[4]>23 || fields[5]>59 || fields[6]>59)return {};
    const auto date=sys_days{year{static_cast<int>(y)}/month{fields[1]}/1}+days{static_cast<int>(fields[3])-1};const year_month_day adjusted{date};if(int(adjusted.year())<100 || int(adjusted.year())>9999)return {};
    const auto count=(date-sys_days{year{1899}/12/30}).count();const auto fraction=static_cast<double>(fields[4]*3600+fields[5]*60+fields[6])/86400.0;return static_cast<double>(count)+(count<0?-fraction:fraction);
}
std::u16string bool_string(bool b,std::uint32_t lcid,bool local){
    if(local)switch(lcid&0x3ffU){case 7:return b?u"Wahr":u"Falsch";case 12:return b?u"Vrai":u"Faux";case 10:return b?u"Verdadero":u"Falso";case 16:return b?u"Vero":u"Falso";case 22:return b?u"Verdadeiro":u"Falso";case 19:return b?u"Waar":u"Onwaar";case 29:return b?u"Sant":u"Falskt";default:break;}return b?u"True":u"False";
}
}

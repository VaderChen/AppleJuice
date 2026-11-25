#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "host_complex_math.h"
#include "msvcp_math.hpp"
#include "msvcp90_string_exports.hpp"
#include "crt_bounded_length.hpp"
#include "windows_math_environment.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// 逐一運算的 IEEE 結果須與 MSVC 的 SSE2／x87 逐步運算一致：不得把乘加收縮成 FMA。
#pragma clang fp contract(off)

namespace x64arm {
namespace {
// msvcp90（msvcp80 經家族對應共用）的 <complex>、_Ctraits、char_traits、allocator 與 _String_val 匯出。
// 這些都是模板實例化的純值運算，以宿主 ARM 原生實作；名稱依型別與 ABI 在初始化時展開一次。
enum class Op : std::uint8_t {
    Equal,NotEqual,Multiply,Subtract,Add,Divide,Negate,Plus,Fabs,Abs,Arg,Imag,Norm,Real,
    Conj,Cos,Cosh,Exp,Log,Log10,Sin,Sinh,Sqrt,Tan,Tanh,Polar,PolarMagnitude,Pow,
    Construct,ConstructFrom,Assign,AssignScalar,MultiplyAssign,AddAssign,SubtractAssign,DivideAssign,DefaultConstruct,
    GetReal,GetImag,SetReal,SetImag,
    TraitsCosh,TraitsSinh,TraitsExp,TraitsInfinity,TraitsNan,TraitsIsinf,TraitsIsnan,TraitsLibm,
    ReturnThis,
    CopySecure,MoveSecure,AssignFill,AssignFillRef,AssignOne,Compare,Copy,Move,Length,Find,Eof,Eq,Lt,EqInt,NotEof,ToChar,ToInt,
    Address,Allocate,Deallocate,ConstructElement,Destroy,MaxSize};
// 二元運算元形狀：哪一側是純量。
enum class Shape : std::uint8_t {None,ScalarComplex,ComplexComplex,ComplexScalar,ComplexInt};
struct Binding {
    std::string name;Op op{};Shape shape{};
    std::uint8_t size{};     // 浮點元素 4／8 byte；字元元素 1／2 byte
    std::uint8_t other{};    // 轉換建構的來源元素大小
    bool member{};           // thiscall：Win32 的 this 在 ECX，Win64 計入第一個參數
    unsigned arguments{};std::uint32_t wide{};
    unsigned architectures;
    std::string_view libm;   // _Ctraits 轉交的 CRT 數學函式
};
struct FloatType {const char* letter;std::uint8_t size;const char* structure;};
// MSVC 的 long double 與 double 相同。
constexpr FloatType float_types[]{{"M",4,"_C_float_complex"},{"N",8,"_C_double_complex"},{"O",8,"_C_ldouble_complex"}};
struct CharType {const char* letter;std::uint8_t size;const char* integer;};
constexpr CharType char_types[]{{"D",1,"H"},{"_W",2,"G"},{"G",2,"G"}};

// %T／%U／%C／%I 代入型別；%m %k %i 為非 const／const／protected 成員，%R %A 為 const／非 const 參考，
// %P %Q 為指標，%n 為 size_t，%1 為第二個參數 size_t 的回溯（Win32 的 I 是單字元型別，不進回溯表）。
std::string expand(std::string_view pattern,bool x86,std::string_view t,std::string_view u={},std::string_view c={},std::string_view i={}){
    std::string out;
    for(std::size_t k=0;k<pattern.size();++k){
        if(pattern[k]!='%' || k+1==pattern.size()){out+=pattern[k];continue;}
        switch(pattern[++k]){
        case 'T':out+=t;break;case 'U':out+=u;break;case 'C':out+=c;break;case 'I':out+=i;break;
        case 'm':out+=x86?"QAE":"QEAA";break;case 'k':out+=x86?"QBE":"QEBA";break;case 'i':out+=x86?"IAE":"IEAA";break;
        case 'R':out+=x86?"AB":"AEB";break;case 'A':out+=x86?"AA":"AEA";break;
        case 'P':out+=x86?"PA":"PEA";break;case 'Q':out+=x86?"PB":"PEB";break;
        case 'n':out+=x86?"I":"_K";break;case '1':out+=x86?"I":"1";break;
        default:throw std::logic_error("msvcp90 名稱樣板含未知代號");
        }
    }
    return out;
}

using Named=std::pair<const char*,Op>;
const std::vector<Binding>& bindings(){
    static const auto value=[]{
        std::vector<Binding> out;std::unordered_map<std::string,std::size_t> seen;
        for(const bool x86:{true,false}){
            // Win32 的 double／long double 值參數占兩個 4-byte 槽（wide）；無指標的靜態函式兩種 ABI 同名，只登錄一次。
            const auto add=[&](std::string name,Op op,unsigned arguments,bool member,std::uint8_t size,Shape shape=Shape::None,
                               std::uint32_t wide=0,std::uint8_t other=0,std::string_view libm={}){
                const unsigned arch=x86?1U:2U;
                const auto [slot,inserted]=seen.emplace(name,out.size());
                if(!inserted){out[slot->second].architectures|=arch;return;}
                out.push_back({std::move(name),op,shape,size,other,member,arguments+(member && !x86?1U:0U),x86?wide:0U,arch,libm});
            };
            for(std::size_t index=0;index<std::size(float_types);++index){
                const auto& type=float_types[index];const std::string_view t=type.letter;const auto s=type.size;
                const auto e=[&](const std::string& pattern,std::string_view u={},std::string_view c={}){return expand(pattern,x86,t,u,c);};
                const std::uint32_t one=s==8?1U:0U,two=s==8?3U:0U;
                // 自由函式（cdecl）：回傳 complex 的函式第一個參數是呼叫端配置的回傳緩衝區。
                const std::string first="%RV?$complex@%T@0@",result="YA?AV?$complex@%T@0@";
                for(const auto& [code,op]:std::initializer_list<Named>{{"?8",Op::Equal},{"?9",Op::NotEqual}}){
                    const std::string head=std::string("??$")+code+"%T@std@@YA_N";
                    add(e(head+"%R%T"+first+"@Z"),op,2,false,s,Shape::ScalarComplex);
                    add(e(head+first+"0@Z"),op,2,false,s,Shape::ComplexComplex);
                    add(e(head+first+"%R%T@Z"),op,2,false,s,Shape::ComplexScalar);
                }
                for(const auto& [code,op]:std::initializer_list<Named>{{"?D",Op::Multiply},{"?G",Op::Subtract},{"?H",Op::Add},{"?K",Op::Divide},{"pow@",Op::Pow}}){
                    const std::string head=std::string("??$")+code+"%T@std@@"+result;
                    add(e(head+"%R%T%RV10@@Z"),op,3,false,s,Shape::ScalarComplex);
                    add(e(head+"%RV10@0@Z"),op,3,false,s,Shape::ComplexComplex);
                    add(e(head+"%RV10@%R%T@Z"),op,3,false,s,Shape::ComplexScalar);
                    if(op==Op::Pow)add(e(head+"%RV10@H@Z"),op,3,false,s,Shape::ComplexInt);
                }
                add(e("??$?G%T@std@@"+result+"%RV10@@Z"),Op::Negate,2,false,s);
                add(e("??$?H%T@std@@"+result+"%RV10@@Z"),Op::Plus,2,false,s);
                add(e("??$_Fabs@%T@std@@YA%T"+first+"%PH@Z"),Op::Fabs,2,false,s);
                for(const auto& [name,op]:std::initializer_list<Named>{{"abs",Op::Abs},{"arg",Op::Arg},{"imag",Op::Imag},{"norm",Op::Norm},{"real",Op::Real}})
                    add(e(std::string("??$")+name+"@%T@std@@YA%T"+first+"@Z"),op,1,false,s);
                for(const auto& [name,op]:std::initializer_list<Named>{{"conj",Op::Conj},{"cos",Op::Cos},{"cosh",Op::Cosh},{"exp",Op::Exp},{"log",Op::Log},
                        {"log10",Op::Log10},{"sin",Op::Sin},{"sinh",Op::Sinh},{"sqrt",Op::Sqrt},{"tan",Op::Tan},{"tanh",Op::Tanh}})
                    add(e(std::string("??$")+name+"@%T@std@@"+result+"%RV10@@Z"),op,2,false,s);
                add(e("??$polar@%T@std@@"+result+"%R%T0@Z"),Op::Polar,3,false,s);
                add(e("??$polar@%T@std@@"+result+"%R%T@Z"),Op::PolarMagnitude,2,false,s);
                // complex<T> 成員。
                const std::string complex="?$complex@%T@std@@";
                add(e("??0"+complex+"%m@%R%T0@Z"),Op::Construct,2,true,s);
                for(std::size_t from=index;from<std::size(float_types);++from)
                    add(e("??0"+complex+"%m@%RU%C@@@Z",{},float_types[from].structure),Op::ConstructFrom,1,true,s,Shape::None,0,float_types[from].size);
                for(std::size_t from=0;from<std::size(float_types);++from)if(from!=index)
                    add(e("??0"+complex+"%m@%RV?$complex@%U@1@@Z",float_types[from].letter),Op::ConstructFrom,1,true,s,Shape::None,0,float_types[from].size);
                add(e("??4"+complex+"%m%AV01@%R%T@Z"),Op::AssignScalar,1,true,s);
                add(e("??4"+complex+"%m%AV01@%RV01@@Z"),Op::Assign,1,true,s);
                for(const auto& [code,op]:std::initializer_list<Named>{{"??X",Op::MultiplyAssign},{"??Y",Op::AddAssign},{"??Z",Op::SubtractAssign},{"??_0",Op::DivideAssign}}){
                    // VC6 同時匯出模板及非模板自由函式，左側物件是第一個 cdecl 參數。
                    const auto suffix="@std@@YA%AV?$complex@%T@0@%AV10@%RV10@@Z";
                    add(e(std::string("??$")+std::string(code).substr(1)+"%T%T"+suffix),op,2,false,s,Shape::ComplexComplex);
                    add(e(std::string(code)+"std@@YA%AV?$complex@%T@0@%AV10@%RV10@@Z"),op,2,false,s,Shape::ComplexComplex);
                    add(e(code+complex+"%m%AV01@%R%T@Z"),op,1,true,s,Shape::ComplexScalar);
                    add(e(code+complex+"%m%AV01@%RV01@@Z"),op,1,true,s,Shape::ComplexComplex);
                }
                add(e("??_F"+complex+"%mXXZ"),Op::DefaultConstruct,0,true,s);
                // _Complex_base<T,C 結構>：與 complex<T> 同一個 {實部, 虛部} 版面。
                const std::string base="?$_Complex_base@%TU%C@@@std@@";
                add(e("??0"+base+"%m@%R%T0@Z",{},type.structure),Op::Construct,2,true,s);
                add(e("??4"+base+"%m%AV01@%RV01@@Z",{},type.structure),Op::Assign,1,true,s);
                add(e("?imag@"+base+"%k%TXZ",{},type.structure),Op::GetImag,0,true,s);
                add(e("?imag@"+base+"%m%T%R%T@Z",{},type.structure),Op::SetImag,1,true,s);
                add(e("?real@"+base+"%k%TXZ",{},type.structure),Op::GetReal,0,true,s);
                add(e("?real@"+base+"%m%T%R%T@Z",{},type.structure),Op::SetReal,1,true,s);
                // _Ctraits<T>：值參數的靜態函式；libm 函式轉交 CRT 的同名服務以共用 errno／浮點狀態契約。
                const std::string traits="?$_Ctraits@%T@std@@";
                add(e("??4"+traits+"%m%AV01@%RV01@@Z"),Op::ReturnThis,1,true,s);
                add(e("?_Cosh@"+traits+"SA%T%T%T@Z"),Op::TraitsCosh,2,false,s,Shape::None,two);
                add(e("?_Sinh@"+traits+"SA%T%T%T@Z"),Op::TraitsSinh,2,false,s,Shape::None,two);
                // _Exp(T* x, T y, short eoff)：*x＝y·e^*x·2^eoff，回傳 FP 分類碼。
                add(e("?_Exp@"+traits+"SAF%P%T%TF@Z"),Op::TraitsExp,3,false,s,Shape::None,s==8?2U:0U);
                add(e("?_Infv@"+traits+"SA%T%T@Z"),Op::TraitsInfinity,1,false,s,Shape::None,one);
                add(e("?_Nanv@"+traits+"SA%T%T@Z"),Op::TraitsNan,1,false,s,Shape::None,one);
                add(e("?_Isinf@"+traits+"SA_N%T@Z"),Op::TraitsIsinf,1,false,s,Shape::None,one);
                add(e("?_Isnan@"+traits+"SA_N%T@Z"),Op::TraitsIsnan,1,false,s,Shape::None,one);
                for(const auto& [name,single]:std::initializer_list<std::pair<const char*,const char*>>{{"atan2","atan2f"},{"pow","powf"}})
                    add(e(std::string("?")+name+"@"+traits+"SA%T%T%T@Z"),Op::TraitsLibm,2,false,s,Shape::None,two,0,s==4?single:name);
                for(const auto& [name,single]:std::initializer_list<std::pair<const char*,const char*>>{{"cos","cosf"},{"exp","expf"},{"log","logf"},{"sin","sinf"},{"sqrt","sqrtf"},{"tan","tanf"}})
                    add(e(std::string("?")+name+"@"+traits+"SA%T%T@Z"),Op::TraitsLibm,1,false,s,Shape::None,one,0,s==4?single:name);
                add(e("?ldexp@"+traits+"SA%T%TH@Z"),Op::TraitsLibm,2,false,s,Shape::None,one,0,s==4?"ldexpf":"ldexp");
            }
            for(const auto& type:char_types){
                const auto u=type.size;
                const auto e=[&](const std::string& pattern){return expand(pattern,x86,type.letter,{},{},type.integer);};
                // char_traits<E>：全部是 cdecl 靜態函式。
                const std::string traits="@?$char_traits@%T@std@@";
                add(e("?_Copy_s"+traits+"SA%P%T%P%T%n%Q%T%1@Z"),Op::CopySecure,4,false,u);
                add(e("?_Move_s"+traits+"SA%P%T%P%T%n%Q%T%1@Z"),Op::MoveSecure,4,false,u);
                add(e("?assign"+traits+"SA%P%T%P%T%n%T@Z"),Op::AssignFill,3,false,u);
                if(std::string_view(type.letter)!="_W")
                    add(e("?assign"+traits+"SA%P%T%P%T%n%R%T@Z"),Op::AssignFillRef,3,false,u);
                add(e("?assign"+traits+"SAX%A%T%R%T@Z"),Op::AssignOne,2,false,u);
                add(e("?compare"+traits+"SAH%Q%T0%n@Z"),Op::Compare,3,false,u);
                add(e("?copy"+traits+"SA%P%T%P%T%Q%T%n@Z"),Op::Copy,3,false,u);
                add(e("?eof"+traits+"SA%IXZ"),Op::Eof,0,false,u);
                add(e("?eq"+traits+"SA_N%R%T0@Z"),Op::Eq,2,false,u);
                add(e("?eq_int_type"+traits+"SA_N%R%I0@Z"),Op::EqInt,2,false,u);
                add(e("?find"+traits+"SA%Q%T%Q%T%n%R%T@Z"),Op::Find,3,false,u);
                add(e("?length"+traits+"SA%n%Q%T@Z"),Op::Length,1,false,u);
                add(e("?lt"+traits+"SA_N%R%T0@Z"),Op::Lt,2,false,u);
                add(e("?move"+traits+"SA%P%T%P%T%Q%T%n@Z"),Op::Move,3,false,u);
                add(e("?not_eof"+traits+"SA%I%R%I@Z"),Op::NotEof,1,false,u);
                add(e("?to_char_type"+traits+"SA%T%R%I@Z"),Op::ToChar,1,false,u);
                add(e("?to_int_type"+traits+"SA%I%R%T@Z"),Op::ToInt,1,false,u);
                // allocator<E>：無狀態；配置與 operator new／delete 共用 CRT heap。
                const std::string allocator="?$allocator@%T@std@@";
                add(e("??0"+allocator+"%m@%RV01@@Z"),Op::ReturnThis,1,true,u);
                add(e("??0"+allocator+"%m@XZ"),Op::ReturnThis,0,true,u);
                add(e("??4"+allocator+"%m%AV01@%RV01@@Z"),Op::ReturnThis,1,true,u);
                add(e("?address@"+allocator+"%k%P%T%A%T@Z"),Op::Address,1,true,u);
                add(e("?address@"+allocator+"%k%Q%T%R%T@Z"),Op::Address,1,true,u);
                add(e("?allocate@"+allocator+"%m%P%T%n@Z"),Op::Allocate,1,true,u);
                add(e("?allocate@"+allocator+"%m%P%T%n%QX@Z"),Op::Allocate,2,true,u);
                add(e("?construct@"+allocator+"%mX%P%T%R%T@Z"),Op::ConstructElement,2,true,u);
                add(e("?deallocate@"+allocator+"%mX%P%T%n@Z"),Op::Deallocate,2,true,u);
                add(e("?destroy@"+allocator+"%mX%P%T@Z"),Op::Destroy,1,true,u);
                add(e("?max_size@"+allocator+"%k%nXZ"),Op::MaxSize,0,true,u);
                // _String_val<E,allocator<E>>：VC9 字串基底只含空的 allocator（版面補齊到指標大小），建構與指派不改寫資料。
                const std::string string_value="?$_String_val@%TV?$allocator@%T@std@@@std@@";
                add(e("??0"+string_value+"%i@V?$allocator@%T@1@@Z"),Op::ReturnThis,1,true,u);
                add(e("??0"+string_value+"%m@%RV01@@Z"),Op::ReturnThis,1,true,u);
                add(e("??4"+string_value+"%m%AV01@%RV01@@Z"),Op::ReturnThis,1,true,u);
                add(e("??_F"+string_value+"%mXXZ"),Op::ReturnThis,0,true,u);
            }
            const auto e=[&](const std::string& pattern){return expand(pattern,x86,"X");};
            add(e("??0?$allocator@X@std@@%m@%RV01@@Z"),Op::ReturnThis,1,true,0);
            add(e("??0?$allocator@X@std@@%m@XZ"),Op::ReturnThis,0,true,0);
            add(e("??4?$allocator@X@std@@%m%AV01@%RV01@@Z"),Op::ReturnThis,1,true,0);
        }
        return out;
    }();
    return value;
}
const auto& binding_index(){
    static const auto index=[]{std::unordered_map<std::string_view,const Binding*> out;for(const auto& item:bindings())out.emplace(item.name,&item);return out;}();
    return index;
}
const Binding& binding(std::string_view symbol){
    const auto& index=binding_index();
    const auto found=index.find(symbol);
    if(found==index.end())throw HostCallFault("未知的 msvcp90 數值服務");
    return *found->second;
}

template<class T> using Raw=std::conditional_t<sizeof(T)==4,std::uint32_t,std::uint64_t>;
template<class T> constexpr auto value_bits=static_cast<std::uint8_t>(sizeof(T)*8);
template<class T> T load_value(GuestMemory& memory,std::uint64_t address){return std::bit_cast<T>(static_cast<Raw<T>>(memory.load(GuestAddress{address},value_bits<T>)));}
template<class T> void store_value(GuestMemory& memory,std::uint64_t address,T value){memory.store(GuestAddress{address},value_bits<T>,std::bit_cast<Raw<T>>(value));}
template<class T> std::complex<T> load_complex(GuestMemory& memory,std::uint64_t address){
    memory.check(GuestAddress{address},2*sizeof(T),Access::Read);
    return {load_value<T>(memory,address),load_value<T>(memory,address+sizeof(T))};
}
template<class T> void store_complex(GuestMemory& memory,std::uint64_t address,std::complex<T> value){
    memory.check(GuestAddress{address},2*sizeof(T),Access::Write);
    store_value(memory,address,value.real());store_value(memory,address+sizeof(T),value.imag());
}
// std::complex 只當 {實部, 虛部} 容器；乘除不用 C 的 Annex G 輔助函式，依 STL 的逐步公式計算。
template<class T> std::complex<T> multiply(std::complex<T> x,std::complex<T> y){
    return {x.real()*y.real()-x.imag()*y.imag(),x.real()*y.imag()+x.imag()*y.real()};
}
template<class T> std::complex<T> scale(std::complex<T> x,T y){return {x.real()*y,x.imag()*y};}
// Smith 除法：以較大的除數分量正規化，避免中間值上溢；除數為零或 NaN 時兩個分量都是 NaN。
template<class T> std::complex<T> divide(std::complex<T> x,std::complex<T> y){
    constexpr auto nan=std::numeric_limits<T>::quiet_NaN();
    if(std::isnan(y.real()) || std::isnan(y.imag()))return {nan,nan};
    if(std::fabs(y.imag())<std::fabs(y.real())){
        const T ratio=y.imag()/y.real(),denominator=y.real()+ratio*y.imag();
        if(std::isnan(denominator) || denominator==T{})return {nan,nan};
        return {(x.real()+x.imag()*ratio)/denominator,(x.imag()-x.real()*ratio)/denominator};
    }
    if(y.imag()==T{})return {nan,nan};
    const T ratio=y.real()/y.imag(),denominator=y.imag()+ratio*y.real();
    if(std::isnan(denominator) || denominator==T{})return {nan,nan};
    return {(x.real()*ratio+x.imag())/denominator,(x.imag()*ratio-x.real())/denominator};
}
template<class T> std::complex<T> power(std::complex<T> base,std::int32_t exponent){
    // 平方乘法；負指數取倒數。
    auto count=exponent<0?0U-static_cast<std::uint32_t>(exponent):static_cast<std::uint32_t>(exponent);
    std::complex<T> result{T{1},T{}};
    for(;;){
        if(count&1U)result=multiply(result,base);
        if(!(count>>=1))break;
        base=multiply(base,base);
    }
    return exponent<0?divide(std::complex<T>{T{1},T{}},result):result;
}
template<class T> std::complex<T> exp_log(std::complex<T> factor,std::complex<T> z){
    return host_complex(X64armComplexExp,multiply(factor,host_complex(X64armComplexLog,z)));
}
// 實數底為正、虛部為零時走實數 pow，讓實數結果不帶 exp(log) 的捨入誤差。
template<class T> std::complex<T> pow_complex_real(std::complex<T> x,T y){
    if(x.imag()==T{} && T{}<x.real())return {std::pow(x.real(),y),T{}};
    return exp_log(std::complex<T>{y,T{}},x);
}
template<class T> std::complex<T> pow_real_complex(T x,std::complex<T> y){
    if(y.imag()==T{})return {std::pow(x,y.real()),T{}};
    if(T{}<x)return host_complex(X64armComplexExp,scale(y,std::log(x)));
    return exp_log(y,std::complex<T>{x,T{}});
}
template<class T> std::complex<T> pow_complex(std::complex<T> x,std::complex<T> y){
    if(y.imag()==T{})return pow_complex_real(x,y.real());
    if(x.imag()==T{})return pow_real_complex(x.real(),y);
    return exp_log(y,x);
}
// _Fabs：回傳值×2^exponent＝|z|；有限非零時 exponent 為 ±2，先縮放避免中間值上溢或下溢。
template<class T> T scaled_magnitude(std::complex<T> z,std::int32_t& exponent){
    exponent=0;T a=z.real(),b=z.imag();
    if(std::isinf(a) || std::isinf(b))return std::numeric_limits<T>::infinity();
    if(std::isnan(a))return a;
    if(std::isnan(b))return b;
    a=std::fabs(a);b=std::fabs(b);
    if(a<b)std::swap(a,b);
    if(a==T{})return a;
    const bool large=T{1}<=a;exponent=large?2:-2;
    const T factor=large?static_cast<T>(0.25):T{4};
    return std::hypot(a*factor,b*factor);
}
X64armComplexKind complex_kind(Op op){
    switch(op){
    case Op::Cos:return X64armComplexCos;case Op::Cosh:return X64armComplexCosh;case Op::Exp:return X64armComplexExp;
    case Op::Log:return X64armComplexLog;case Op::Log10:return X64armComplexLog10;case Op::Sin:return X64armComplexSin;
    case Op::Sinh:return X64armComplexSinh;case Op::Sqrt:return X64armComplexSqrt;case Op::Tan:return X64armComplexTan;
    case Op::Tanh:return X64armComplexTanh;default:throw std::logic_error("不是 complex 超越函式");
    }
}
}

bool msvcp_traits::wrong_traits_abi(std::string_view symbol,bool x86){
    const auto& index=binding_index();
    const auto found=index.find(symbol);
    if(found==index.end())return false;
    const auto& b=*found->second;
    return !(b.architectures&(x86?1U:2U));
}

std::vector<std::pair<std::string_view,std::string>> msvcp_traits::resident_traits_routines(bool x86){
    std::vector<std::pair<std::string_view,std::string>> out;
    for(const auto& b:bindings()){
        if(!(b.architectures&(x86?1U:2U)))continue;
        std::string_view component;
        switch(b.op){
        case Op::Real:component="real";break;
        case Op::Imag:component="imag";break;
        case Op::GetReal:component="get_real";break;
        case Op::GetImag:component="get_imag";break;
        case Op::SetReal:component="set_real";break;
        case Op::SetImag:component="set_imag";break;
        case Op::Construct:component="construct";break;
        case Op::Assign:component="assign";break;
        case Op::AssignScalar:component="assign_scalar";break;
        case Op::DefaultConstruct:component="zero";break;
        default:break;
        }
        if(!component.empty()){
            out.emplace_back(b.name,"guest_complex_"+std::string(b.size==4?"F_":"D_")+std::string(component));
            continue;
        }
        if(b.op==Op::ReturnThis && b.name.find("?$_Ctraits@")!=b.name.npos){
            out.emplace_back(b.name,"guest_numeric_assign");
            continue;
        }
        if(b.op>=Op::TraitsInfinity && b.op<=Op::TraitsIsnan){
            const auto method=b.name.substr(2,b.name.find('@')-2);
            out.emplace_back(b.name,"guest_ctraits_"+std::string(b.size==4?"F_":"D_")+method);
            continue;
        }
        if(b.op<Op::AssignFill || b.op>Op::ToInt)continue;
        auto operation=b.name.substr(1,b.name.find('@')-1);
        if(b.op==Op::AssignFill)operation="fill";
        if(b.op==Op::AssignFillRef)operation="fill_ref";
        out.emplace_back(b.name,"guest_traits_"+std::string(b.size==1?"D_":"W_")+operation);
    }
    return out;
}

std::span<const WindowsServiceDescriptor> msvcp_values_service_descriptors(){
    // thiscall 成員由被呼叫端清理堆疊；自由與靜態函式是 cdecl。
    static const auto table=[]{
        std::vector<WindowsServiceDescriptor> out;out.reserve(bindings().size());
        for(const auto& item:bindings()){
            const bool free_assign=!item.member && item.op>=Op::MultiplyAssign && item.op<=Op::DivideAssign;
            const auto family=item.op==Op::AssignFillRef || free_assign?"msvcp60.dll":"msvcp90.dll";
            out.push_back({family,item.name,item.arguments,item.wide,0,0,!item.member});
        }
        return out;
    }();
    return table;
}

std::optional<std::uint32_t> WindowsServices::invoke_msvcp_values(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const auto& b=binding(service.symbol);
    const WindowsCall arg(state,memory,service);
    const auto self=b.member?(x86_?state.reg(Gpr::Rcx)&UINT32_MAX:arg(0)):0;
    const auto a=[&](unsigned index){return arg(index+(b.member && !x86_?1U:0U));};
    const auto done=[&](std::uint64_t value)->std::optional<std::uint32_t>{arg.return_to(state,value);return std::nullopt;};
    const auto floating=[&](double value,bool single)->std::optional<std::uint32_t>{windows_floating_return(state,x86_,value,single);return done(0);};
    if(b.op==Op::ReturnThis)return done(self);
    if(b.op==Op::TraitsLibm){
        // 參數版面（cdecl、值參數）與 CRT 的 cos／powf／ldexp 等完全相同：直接交給同一個 CRT 數學服務。
        static const auto crt=[]{
            std::unordered_map<std::string_view,const WindowsServiceDescriptor*> out;
            for(const auto& descriptor:runtime_service_descriptors())if(descriptor.family=="crt")out.try_emplace(descriptor.symbol,&descriptor);
            return out;
        }();
        const auto found=crt.find(b.libm);
        if(found==crt.end())throw std::logic_error("_Ctraits 缺少對應的 CRT 數學服務");
        return invoke_math(state,memory,*found->second);
    }
    const std::uint64_t size_limit=x86_?UINT32_MAX:UINT64_MAX;
    if(b.op>=Op::CopySecure){
        const unsigned unit=b.size;const auto bits=static_cast<std::uint8_t>(unit*8);
        const bool narrow=unit==1;
        // int_type：char 為 int（EOF＝-1），wchar_t／unsigned short 為 unsigned short（WEOF＝0xFFFF）。
        const std::uint64_t eof=narrow?UINT32_MAX:0xffffU,mask=narrow?0xffU:0xffffU;
        const auto element=[&](std::uint64_t address){return memory.load(GuestAddress{address},bits);};
        const auto integer=[&](std::uint64_t address){return memory.load(GuestAddress{address},narrow?32:16);};
        const auto at=[&](std::uint64_t base,std::uint64_t index){
            if(index>(size_limit-base)/unit)throw HostCallFault("char_traits 位址溢位");
            return base+index*unit;
        };
        const auto bytes=[&](std::uint64_t count){if(count>size_limit/unit)throw HostCallFault("char_traits 位元組數溢位");return static_cast<std::size_t>(count*unit);};
        switch(b.op){
        case Op::CopySecure:case Op::MoveSecure:{
            const auto destination=a(0);
            if(const auto error=secure_memory_copy(memory,destination,a(1),a(2),a(3),unit,b.op==Op::MoveSecure)){
                memory.store(GuestAddress{runtime_.errno_cell},32,error);
                if(const auto handler=current_invalid_parameter_handler()){start_callbacks(state,memory,{{handler,{0,0,0,0,0}}},Continuation::Return,destination);return std::nullopt;}
                return 0xc0000417U;
            }
            return done(destination);
        }
        case Op::AssignFill:case Op::AssignFillRef:{
            const auto first=a(0),count=a(1);
            if(!count)return done(first);
            const auto value=b.op==Op::AssignFillRef?element(a(2)):a(2)&mask;
            const auto total=bytes(count);
            if(narrow){memory.fill(GuestAddress{first},static_cast<std::uint8_t>(value),total);return done(first);}
            std::array<std::uint8_t,1024> block;
            for(std::size_t i=0;i<block.size();i+=2){block[i]=static_cast<std::uint8_t>(value);block[i+1]=static_cast<std::uint8_t>(value>>8);}
            memory.check(GuestAddress{first},total,Access::Write);
            for(std::size_t i=0;i<total;){
                const auto length=std::min(block.size(),total-i);
                memory.write(GuestAddress{first+i},std::span(block).first(length));i+=length;
            }
            return done(first);
        }
        case Op::AssignOne:memory.store(GuestAddress{a(0)},bits,element(a(1)));return done(0);
        case Op::Compare:{
            // char 依 memcmp（無號位元組），寬字元依 wmemcmp（無號 16-bit）。
            const auto left=a(0),right=a(1),count=a(2);
            for(std::uint64_t i=0;i<count;++i){
                const auto x=element(at(left,i)),y=element(at(right,i));
                if(x!=y)return done(x<y?UINT32_MAX:1);
            }
            return done(0);
        }
        case Op::Copy:case Op::Move:{
            const auto destination=a(0),source=a(1),count=a(2);
            if(count)memory.move(GuestAddress{destination},GuestAddress{source},bytes(count));
            return done(destination);
        }
        case Op::Length:return done(crt_bounded_length(memory,a(0),size_limit/unit,unit));
        case Op::Find:{
            const auto first=a(0),count=a(1);if(!count)return done(0);
            const auto wanted=element(a(2));
            for(std::uint64_t i=0;i<count;++i)if(const auto address=at(first,i);element(address)==wanted)return done(address);
            return done(0);
        }
        case Op::Eof:return done(eof);
        case Op::Eq:return done(element(a(0))==element(a(1)));
        case Op::Lt:{
            // VC9 的 char_traits<char>::lt 以 char（有號）比較；寬字元為無號。
            const auto x=element(a(0)),y=element(a(1));
            return done(narrow?static_cast<std::int8_t>(x)<static_cast<std::int8_t>(y):x<y);
        }
        case Op::EqInt:return done(integer(a(0))==integer(a(1)));
        case Op::NotEof:{const auto value=integer(a(0));return done(value!=eof?value:0);}
        case Op::ToChar:return done(integer(a(0))&mask);
        case Op::ToInt:return done(element(a(0)));
        case Op::Address:return done(a(0));
        case Op::Allocate:{
            // 與 operator new 同一個 CRT heap，0 個元素仍回傳唯一有效位址；超過 size_t 或配置失敗丟出 bad_alloc。
            const auto count=a(0);
            if(count>size_limit/unit){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return std::nullopt;}
            const auto size=std::max<std::uint64_t>(count*unit,1);
            auto pointer=guest_heap_allocate(memory,size);if(!pointer)pointer=allocate_heap(memory,size);
            if(!pointer){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return std::nullopt;}
            return done(pointer);
        }
        case Op::Deallocate:if(!free_heap(memory,a(0),state))throw HostCallFault("allocator::deallocate 收到非配置起始地址");return done(0);
        case Op::ConstructElement:{const auto pointer=a(0);if(pointer)memory.store(GuestAddress{pointer},bits,element(a(1)));return done(0);}
        case Op::Destroy:return done(0);
        case Op::MaxSize:return done(std::max<std::uint64_t>(size_limit/unit,1));
        default:throw std::logic_error("未知的 msvcp90 字元服務");
        }
    }
    const auto evaluate=[&]<class T>()->std::optional<std::uint32_t>{
        using C=std::complex<T>;constexpr bool single=sizeof(T)==4;
        const auto value=[&](std::uint64_t address){return load_value<T>(memory,address);};
        const auto pair=[&](std::uint64_t address){return load_complex<T>(memory,address);};
        const auto operand=[&](unsigned index,bool scalar)->C{return scalar?C{value(a(index)),T{}}:pair(a(index));};
        const auto real_result=[&](T result){return floating(static_cast<double>(result),single);};
        // 值參數：Win32 在堆疊（double 占兩槽），Win64 在與參數序相同的 XMM。
        const auto by_value=[&](unsigned index)->T{
            if(x86_)return std::bit_cast<T>(static_cast<Raw<T>>(arg(index)));
            Raw<T> raw=0;for(unsigned i=0;i<sizeof(T);++i)raw|=static_cast<Raw<T>>(Raw<T>{state.xmm[index][i]}<<(i*8));
            return std::bit_cast<T>(raw);
        };
        switch(b.op){
        case Op::Construct:{const C z{value(a(0)),value(a(1))};store_complex(memory,self,z);return done(self);}
        case Op::ConstructFrom:{
            const auto source=a(0);
            const C z=b.other==4?C{static_cast<T>(load_value<float>(memory,source)),static_cast<T>(load_value<float>(memory,source+4))}
                                :C{static_cast<T>(load_value<double>(memory,source)),static_cast<T>(load_value<double>(memory,source+8))};
            store_complex(memory,self,z);return done(self);
        }
        case Op::Assign:store_complex(memory,self,pair(a(0)));return done(self);
        case Op::AssignScalar:store_complex(memory,self,C{value(a(0)),T{}});return done(self);
        case Op::DefaultConstruct:store_complex(memory,self,C{});return done(self);
        case Op::GetReal:return real_result(value(self));
        case Op::GetImag:return real_result(value(self+sizeof(T)));
        case Op::SetReal:case Op::SetImag:{const auto v=value(a(0));store_value(memory,self+(b.op==Op::SetImag?sizeof(T):0U),v);return real_result(v);}
        case Op::TraitsInfinity:return real_result(std::numeric_limits<T>::infinity());
        case Op::TraitsNan:return real_result(std::numeric_limits<T>::quiet_NaN());
        case Op::TraitsExp:{
            const auto pointer=a(0);auto x=value(pointer);
            const auto code=msvcp_detail::scaled_exp(x,by_value(1),static_cast<int>(static_cast<std::int16_t>(a(2))));
            store_value(memory,pointer,x);return done(static_cast<std::uint64_t>(static_cast<std::int64_t>(code)));
        }
        case Op::TraitsIsinf:return done(std::isinf(by_value(0)));
        case Op::TraitsIsnan:return done(std::isnan(by_value(0)));
        case Op::TraitsCosh:case Op::TraitsSinh:{
            // y·cosh(x)／y·sinh(x)，與 msvcp100 起的 _Cosh／_Sinh 共用分段縮放；大引數上下溢時 errno＝ERANGE。
            bool range_error=false;const auto x=by_value(0),y=by_value(1);
            const auto result=b.op==Op::TraitsCosh?msvcp_detail::scaled_cosh(x,y,range_error):msvcp_detail::scaled_sinh(x,y,range_error);
            if(range_error)memory.store(GuestAddress{runtime_.errno_cell},32,34);
            return real_result(result);
        }
        case Op::Equal:case Op::NotEqual:{
            const auto left=operand(0,b.shape==Shape::ScalarComplex),right=operand(1,b.shape==Shape::ComplexScalar);
            return done((left.real()==right.real() && left.imag()==right.imag())==(b.op==Op::Equal));
        }
        case Op::Negate:case Op::Plus:case Op::Conj:{
            const auto z=pair(a(1));const auto output=a(0);
            store_complex(memory,output,b.op==Op::Negate?C{-z.real(),-z.imag()}:b.op==Op::Conj?C{z.real(),-z.imag()}:z);
            return done(output);
        }
        case Op::Real:case Op::Imag:{const auto z=pair(a(0));return real_result(b.op==Op::Real?z.real():z.imag());}
        default:break;
        }
        // 先讀取全部輸入再寫回，保留輸出與輸入重疊時的語義。
        const bool assigns=b.op>=Op::MultiplyAssign && b.op<=Op::DivideAssign;
        const bool scalar_result=b.op==Op::Fabs || b.op==Op::Abs || b.op==Op::Arg || b.op==Op::Norm;
        const auto output=assigns && b.member?self:scalar_result?0:a(0);
        C left{},right{};std::int32_t exponent=0;std::uint64_t exponent_output=0;
        const bool right_scalar=b.shape==Shape::ComplexScalar;
        if(assigns){left=pair(output);right=operand(b.member?0:1,right_scalar);}
        else if(b.op==Op::Fabs){left=pair(a(0));exponent_output=a(1);}
        else if(scalar_result)left=pair(a(0));
        else if(b.op==Op::Polar)left={value(a(1)),value(a(2))};
        else if(b.op==Op::PolarMagnitude)left={value(a(1)),T{}};
        else if(b.shape==Shape::None)left=pair(a(1));
        else {
            left=operand(1,b.shape==Shape::ScalarComplex);
            if(b.shape==Shape::ComplexInt)exponent=static_cast<std::int32_t>(static_cast<std::uint32_t>(a(2)));
            else right=operand(2,right_scalar);
        }
        // 純算術只提交浮點狀態；呼叫 CRT 數學函式的運算另依例外設定 errno。
        const bool sets_errno=!assigns && b.op!=Op::Multiply && b.op!=Op::Add && b.op!=Op::Subtract && b.op!=Op::Divide && b.op!=Op::Fabs && b.op!=Op::Norm;
        C result{};T magnitude{};std::int32_t scale_exponent=0;
        {
            WindowsMathEnvironment environment(x86_?static_cast<std::uint32_t>((state.x87_control>>10)&3U)<<13:state.mxcsr);
            switch(b.op){
            case Op::Multiply:case Op::MultiplyAssign:result=right_scalar?scale(left,right.real()):multiply(left,right);break;
            case Op::Add:case Op::AddAssign:result=right_scalar?C{left.real()+right.real(),left.imag()}:C{left.real()+right.real(),left.imag()+right.imag()};break;
            case Op::Subtract:case Op::SubtractAssign:result=right_scalar?C{left.real()-right.real(),left.imag()}:C{left.real()-right.real(),left.imag()-right.imag()};break;
            case Op::Divide:case Op::DivideAssign:result=right_scalar?C{left.real()/right.real(),left.imag()/right.real()}:divide(left,right);break;
            case Op::Fabs:magnitude=scaled_magnitude(left,scale_exponent);break;
            case Op::Abs:magnitude=std::hypot(left.real(),left.imag());break;
            case Op::Arg:magnitude=std::atan2(left.imag(),left.real());break;
            case Op::Norm:magnitude=left.real()*left.real()+left.imag()*left.imag();break;
            case Op::Polar:result={left.real()*std::cos(left.imag()),left.real()*std::sin(left.imag())};break;
            case Op::PolarMagnitude:result=left;break;
            case Op::Pow:
                result=b.shape==Shape::ComplexInt?power(left,exponent):b.shape==Shape::ScalarComplex?pow_real_complex(left.real(),right):
                       right_scalar?pow_complex_real(left,right.real()):pow_complex(left,right);
                break;
            default:result=host_complex(complex_kind(b.op),left);break;
            }
            windows_commit_math_environment(state,memory,sets_errno?runtime_.errno_cell:0,x86_,environment);
        }
        if(b.op==Op::Fabs)memory.store(GuestAddress{exponent_output},32,static_cast<std::uint32_t>(scale_exponent));
        if(scalar_result)return real_result(magnitude);
        store_complex(memory,output,result);return done(output);
    };
    return b.size==4?evaluate.template operator()<float>():evaluate.template operator()<double>();
}
}

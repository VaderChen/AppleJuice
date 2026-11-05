#pragma once
#include "x64arm/windows_abi.hpp"
#include "msvcp_numeric_values.h"
#include <array>
#include <map>
#include <string>
#include <vector>

namespace x64arm::msvcp_numeric {
struct Type {std::string_view code;unsigned bits;bool floating,sign;unsigned digits,digits10;std::array<std::uint64_t,8> values;};
#define TYPE(C,T,B,F,S,D,D10,MIN,MAX,EPS,ROUND,INF,NAN,SNAN,DENORM) {#C,B,F,S,D,D10,{MIN,MAX,EPS,ROUND,INF,NAN,SNAN,DENORM}},
inline constexpr Type types[]{MSVCP_NUMERIC_TYPES(TYPE)};
#undef TYPE
inline constexpr std::string_view operations[]{"min","max","epsilon","round_error","infinity","quiet_NaN","signaling_NaN","denorm_min"};
struct Function {std::string name,native;unsigned type,operation;bool assignment,x86;};
inline const auto& functions(){
    static const auto values=[] {
        std::vector<Function> out;
        for(unsigned t=0;t<std::size(types);++t){const auto code=std::string(types[t].code),stem="?$numeric_limits@"+code+"@std@@";
            for(unsigned op=0;op<std::size(operations);++op)out.push_back({"?"+std::string(operations[op])+"@"+stem+"SA"+code+"XZ","guest_numeric_"+code+"_"+std::string(operations[op]),t,op,false,false});
            for(const bool x86:{true,false})out.push_back({"??4"+stem+(x86?"QAEAAV01@ABV01@@Z":"QEAAAEAV01@AEBV01@@Z"),"guest_numeric_assign",t,0,true,x86});
        }
        for(const bool x86:{true,false})out.push_back({x86?"??4_Num_base@std@@QAEAAV01@ABV01@@Z":"??4_Num_base@std@@QEAAAEAV01@AEBV01@@Z","guest_numeric_assign",0,0,true,x86});
        return out;
    }();return values;
}
inline const Function* function(std::string_view name){
    static const auto index=[] {std::map<std::string_view,const Function*> out;for(const auto& f:functions())out.emplace(f.name,&f);return out;}();
    const auto it=index.find(name);return it==index.end()?nullptr:it->second;
}
inline auto descriptors(){std::vector<WindowsServiceDescriptor> out;for(const auto& f:functions())out.push_back({"msvcp90.dll",f.name,f.assignment?(f.x86?1U:2U):0U,0,0,0,!f.assignment});return out;}
struct Constant {std::string name;std::uint64_t value;unsigned bytes;};
inline const std::vector<Constant>& constants(){
    static const auto values=[] {
        std::vector<Constant> out;
        const auto add=[&](std::string name,std::uint64_t value,unsigned bytes=4){out.push_back({std::move(name),value,bytes});};
        for(const auto& t:types){const auto stem="@?$numeric_limits@"+std::string(t.code)+"@std@@";
            add("?digits"+stem+"2HB",t.digits);add("?digits10"+stem+"2HB",t.digits10);
            if(!t.floating)add("?is_signed"+stem+"2_NB",t.sign,1);
            if(t.code=="_N")add("?is_modulo"+stem+"2_NB",0,1);
            if(t.floating){const bool single=t.bits==32;
                add("?max_exponent"+stem+"2HB",single?128:1024);add("?max_exponent10"+stem+"2HB",single?38:308);
                add("?min_exponent"+stem+"2HB",static_cast<std::uint32_t>(single?-125:-1021));add("?min_exponent10"+stem+"2HB",static_cast<std::uint32_t>(single?-37:-307));
                const auto prefix=t.code=="M"?"_F":t.code=="N"?"_":"_L";
                for(const auto [name,op]:std::array<std::pair<std::string_view,unsigned>,5>{{{"Inf",4},{"Nan",5},{"Snan",6},{"Denorm",7},{"Eps",2}}})add(prefix+std::string(name),t.values[op],t.bits/8);
            }
        }
        add("_LZero",0,8);add("_Hugeval",0x7ff0000000000000ULL,8);
        // VC9 公開基底常數；不以宿主 C++ 版本的 numeric_limits 改變客體 ABI。
        for(const auto group:{0,1,2}){
            const auto stem=group==0?"@_Num_base@std@@":group==1?"@_Num_int_base@std@@":"@_Num_float_base@std@@";
            for(const auto name:{"digits","digits10","max_exponent","max_exponent10","min_exponent","min_exponent10","radix"})
                if(group==0 || std::string_view(name)=="radix")add("?"+std::string(name)+stem+"2HB",group?2:0);
            for(const auto name:{"has_denorm_loss","has_infinity","has_quiet_NaN","has_signaling_NaN","is_bounded","is_exact","is_iec559","is_integer","is_modulo","is_signed","is_specialized","tinyness_before","traps"}){
                const std::string_view n=name;const bool integer=n=="is_bounded" || n=="is_exact" || n=="is_integer" || n=="is_modulo" || n=="is_specialized";
                if(group==1 && !integer)continue;
                add("?"+std::string(name)+stem+"2_NB",group==1 || (group==2 && n!="is_exact" && n!="is_integer" && n!="is_modulo"),1);
            }
            if(group!=1){add("?has_denorm"+std::string(stem)+"2W4float_denorm_style@2@B",group==2);add("?round_style"+std::string(stem)+"2W4float_round_style@2@B",group==2);}
        }
        return out;
    }();return values;
}
}

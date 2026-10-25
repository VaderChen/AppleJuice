// 與 Automation 轉型共用精確有理數、捨入及 Windows VARIANT 編碼。
std::optional<Operator> operator_kind(std::string_view name){
    constexpr std::array<std::string_view,11> names{
        "VarAdd","VarSub","VarMul","VarDiv","VarIdiv","VarMod",
        "VarNeg","VarNot","VarAnd","VarOr","VarXor"};
    const auto at=std::find(names.begin(),names.end(),name);
    if(at==names.end())return {};return static_cast<Operator>(1+(at-names.begin()));
}
bool unary_operator(Operator op){return op==Operator::Neg || op==Operator::Not;}
bool logical_operator(Operator op){return op==Operator::Not || op==Operator::And || op==Operator::Or || op==Operator::Xor;}
std::uint32_t operator_types(Operator op,std::uint16_t left,std::uint16_t right){
    const bool restricted=!logical_operator(op) && op!=Operator::Idiv && op!=Operator::Mod;
    for(const auto t:{left,unary_operator(op)?std::uint16_t{}:right}){
        if(t&0x9000U || (t&0x4000U))return badtype;
        if(t&0x2000U)return mismatch;
        if(t==10 || t==12 || t==13 || t==36)return mismatch;
        if(!valid_type(t,false))return badtype;
        if(restricted && (t==16 || t==18 || t==19 || t==21 || t==22 || t==23))return badtype;
    }
    // Automation 的 INT 與 I8 混合位元／整數除法沒有可用的共同型別。
    if(!restricted && !unary_operator(op) && ((left==20 && right==22) || (left==22 && right==20)))return mismatch;
    return 0;
}
namespace {
bool small_integer(unsigned t){return t==0 || t==2 || t==11 || t==17;}
unsigned integer_result(unsigned left,unsigned right,bool logical){
    if(left==20 || right==20)return 20;
    if(!small_integer(left) || !small_integer(right))return 3;
    if(logical && left==11 && right==11)return 11;
    if(left==17 && right==17)return 17;
    return 2;
}
unsigned unary_integer(unsigned t){return t==0?2:t==2 || t==11 || t==17 || t==20?t:3;}
unsigned arithmetic_result(Operator op,unsigned left,unsigned right){
    const auto has=[&](unsigned t){return left==t || right==t;};
    if(has(14))return 14;
    if(op==Operator::Div){
        if(has(4) && (small_integer(left) || left==4) && (small_integer(right) || right==4))return 4;
        return 5;
    }
    if(op==Operator::Mul){
        if(has(5) || has(7) || has(8))return 5;
        if(has(4))return has(3) || has(20) || has(6)?5:4;
        if(has(6))return 6;
    }else{
        if(has(7))return op==Operator::Sub && left==7 && right==7?5:7;
        if(has(6))return 6;
        if(has(5) || has(8))return 5;
        if(has(4))return has(3) || has(20)?5:4;
    }
    if(has(20))return 20;if(has(3))return 3;
    if(has(2) || has(11) || (left==0 && right==0))return 2;
    return 17;
}
unsigned promote_result(unsigned t){
    return t==17?2:t==2?3:(t==3 || t==20 || t==4 || t==7)?5:t;
}
std::uint32_t coerce_operand(const Value& source,std::string_view text,unsigned type,Value& out){
    if(get(source,0,2)==type){out=source;return 0;}
    return number_convert(source,text,static_cast<std::uint16_t>(type),out);
}
std::uint64_t integer_bits(const Value& v){
    const auto t=get(v,0,2),raw=get(v,8,8);
    if(t==17)return raw&255;
    if(t==2 || t==11)return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int16_t>(raw)));
    if(t==3)return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(raw)));
    return raw;
}
void write_integer(Value& out,unsigned type,std::uint64_t bits){
    out={};put(out,0,2,type);put(out,8,type==17?1:type==2 || type==11?2:type==20?8:4,bits);
}
bool negative_float(const Value& v){
    const auto type=get(v,0,2);return type==4?(get(v,8,4)>>31)!=0:(type==5 || type==7) && (get(v,8,8)>>63)!=0;
}
}
std::uint32_t number_operation(Operator op,const Value& left,const Value& right,Value& output,
                               std::string_view left_text,std::string_view right_text){
    try{
        const auto l=static_cast<std::uint16_t>(get(left,0,2)),r=static_cast<std::uint16_t>(get(right,0,2));
        if(const auto status=operator_types(op,l,r))return status;
        if(l==9 || (!unary_operator(op) && r==9))return mismatch;
        const bool logical=logical_operator(op);
        if(l==1 || (!unary_operator(op) && r==1)){
            output={};put(output,0,2,1);
            if((op!=Operator::And && op!=Operator::Or) || (l==1 && r==1))return 0;
            const auto& source=l==1?right:left;const auto text=l==1?right_text:left_text;
            Value operand;const auto type=unary_integer(static_cast<unsigned>(get(source,0,2)));
            if(const auto status=coerce_operand(source,text,type,operand))return status;
            const auto value=integer_bits(operand);
            if((op==Operator::And && !value) || (op==Operator::Or && value))output=operand;
            return 0;
        }
        if(logical){
            const unsigned target=op==Operator::Not?unary_integer(l):integer_result(l,r,true);
            Value a{},b{};if(const auto status=coerce_operand(left,left_text,target,a))return status;
            if(op!=Operator::Not)if(const auto status=coerce_operand(right,right_text,target,b))return status;
            const auto x=integer_bits(a),y=integer_bits(b);
            write_integer(output,target,op==Operator::Not?~x:op==Operator::And?x&y:op==Operator::Or?x|y:x^y);return 0;
        }
        if(op==Operator::Idiv || op==Operator::Mod){
            const auto target=integer_result(l,r,false);Value a{},b{};
            if(const auto status=coerce_operand(left,left_text,target,a))return status;
            if(const auto status=coerce_operand(right,right_text,target,b))return status;
            auto x=number(a),y=number(b);if(BN_is_zero(y.numerator.get()))return divzero;
            Number n;auto remainder=big();auto ctx=context();
            checked(BN_div(n.numerator.get(),remainder.get(),x.numerator.get(),y.numerator.get(),ctx.get()));
            if(op==Operator::Mod)n.numerator=std::move(remainder);
            n.negative=op==Operator::Mod?x.negative:x.negative!=y.negative;
            return encode_number(n,target,output);
        }
        if(op==Operator::Neg){
            // 浮點數只反轉符號；DECIMAL 保留原 scale 與負零，不重做轉型。
            if(l==4 || l==5 || l==7){output=left;const auto bytes=l==4?4U:8U;put(output,8,bytes,get(left,8,bytes)^(UINT64_C(1)<<(bytes*8-1)));return 0;}
            auto n=number(left,left_text);
            if(l==14){output=left;output[3]^=128;return 0;}
            n.negative=!n.negative && !BN_is_zero(n.numerator.get());
            unsigned target=l==0 || l==11 || l==17?2:l==8?5:l;
            for(;;){const auto status=encode_number(n,target,output);const auto next=promote_result(target);if(status!=overflow || next==target)return status;target=next;}
        }
        auto target=arithmetic_result(op,l,r);
        // 小整數先在 I8、R4／DATE 先在 R8 做運算，溢位後再提升輸出型別。
        // CY／DECIMAL 則必須先依該型別捨入兩個運算元。
        const auto work_type=op==Operator::Div?target:target==6 || target==14?target:target==4 || target==5 || target==7?5:20;
        Value a{},b{};if(const auto status=coerce_operand(left,left_text,work_type,a))return status;
        if(const auto status=coerce_operand(right,right_text,work_type,b))return status;
        auto x=number(a),y=number(b);Number n;auto ctx=context();
        unsigned decimal_scale=0;
        if(target==14)decimal_scale=op==Operator::Mul?std::min(28U,unsigned(a[2])+b[2]):op==Operator::Div?0U:std::max(unsigned(a[2]),unsigned(b[2]));
        bool negative_zero=false;
        if(op==Operator::Add || op==Operator::Sub){
            auto first=big(),second=big();
            checked(BN_mul(first.get(),x.numerator.get(),y.denominator.get(),ctx.get()));
            checked(BN_mul(second.get(),y.numerator.get(),x.denominator.get(),ctx.get()));
            BN_set_negative(first.get(),x.negative);BN_set_negative(second.get(),y.negative);
            checked(op==Operator::Add?BN_add(n.numerator.get(),first.get(),second.get()):BN_sub(n.numerator.get(),first.get(),second.get()));
            n.negative=BN_is_negative(n.numerator.get());BN_set_negative(n.numerator.get(),0);
            checked(BN_mul(n.denominator.get(),x.denominator.get(),y.denominator.get(),ctx.get()));
            negative_zero=negative_float(a) && (op==Operator::Sub?!negative_float(b):negative_float(b));
        }else if(op==Operator::Mul){
            checked(BN_mul(n.numerator.get(),x.numerator.get(),y.numerator.get(),ctx.get()));
            checked(BN_mul(n.denominator.get(),x.denominator.get(),y.denominator.get(),ctx.get()));
            n.negative=x.negative!=y.negative;negative_zero=negative_float(a)!=negative_float(b);
        }else if(op==Operator::Div){
            if(BN_is_zero(y.numerator.get()))return target!=14 && BN_is_zero(x.numerator.get())?overflow:divzero;
            checked(BN_mul(n.numerator.get(),x.numerator.get(),y.denominator.get(),ctx.get()));
            checked(BN_mul(n.denominator.get(),x.denominator.get(),y.numerator.get(),ctx.get()));
            n.negative=x.negative!=y.negative;negative_zero=negative_float(a)!=negative_float(b);
        }else return invalid;
        negative_zero=negative_zero && BN_is_zero(n.numerator.get());
        for(;;){
            const auto status=encode_number(n,target,output,negative_zero,false,decimal_scale);
            const auto next=promote_result(target);
            if(status!=overflow || next==target || op==Operator::Div)return status;
            target=next;
        }
    }catch(const std::invalid_argument&){return mismatch;}catch(const std::range_error&){return overflow;}catch(const std::bad_alloc&){return nomemory;}
}

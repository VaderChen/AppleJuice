#include "x64arm/windows_services.hpp"
#include "service_callback_task.hpp"
#include "automation_value.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <deque>
#include <string>

// oleaut32 純量轉換與 CY／DECIMAL 運算家族。
// Var<T>From<S> 全部交給 VariantChangeType 共用的 number_convert（精確有理數、ties-to-even、溢位與 VARIANT_TRUE 規則），
// 只在此處理 Windows 的參數傳遞：x86 的 8 位元組值占兩個堆疊槽、x64 的 FLOAT／DOUBLE／DATE 由 XMM 傳入、DECIMAL 以指標傳入。
// FromDisp 與 VARIANT 層的 VarCat／VarPow／VarEqv／VarImp 先經既有 VariantChangeTypeEx／VarXor 等入口取得值，不重寫轉型分派。
namespace x64arm {
namespace {
using namespace automation;
constexpr std::string_view scalar_names[]{"UI1","I2","I4","I8","R4","R8","Cy","Date","Bool","I1","UI2","UI4","UI8","Dec"};
bool wide_scalar(std::string_view name){const auto [type,bytes]=scalar_type(name);return bytes==8;}
// 名稱型別：Var<T>From<S>；回傳 {T, S}，S 為 "Disp" 時 type 為 9。
std::pair<std::string_view,std::string_view> conversion_parts(std::string_view name){
    const auto at=name.find("From");if(!name.starts_with("Var") || at==std::string_view::npos)return {};
    return {name.substr(3,at-3),name.substr(at+4)};
}
}
std::span<const WindowsServiceDescriptor> oleaut32_conversion_service_descriptors(){
    static const auto table=[]{
        static std::deque<std::string> names;std::vector<WindowsServiceDescriptor> out;
        const auto add=[&](std::string name,unsigned arguments,std::uint32_t wide=0){names.push_back(std::move(name));out.push_back({"oleaut32.dll",names.back(),arguments,wide});};
        for(const auto target:scalar_names){
            for(const auto source:scalar_names){
                // Windows 沒有 VarI8FromI4／VarUI8FromI4 匯出（標頭以巨集轉到其他入口）。
                if(source==target || (source=="I4" && (target=="I8" || target=="UI8")))continue;
                add("Var"+std::string(target)+"From"+std::string(source),2,wide_scalar(source)?1U:0U);
            }
            add("Var"+std::string(target)+"FromDisp",3);
        }
        add("VarBstrFromDisp",4);
        for(const auto name:{"VarCyAbs","VarCyFix","VarCyInt","VarCyNeg"})add(name,2,1);
        for(const auto name:{"VarCyAdd","VarCySub","VarCyMul"})add(name,3,3);
        add("VarCyMulI4",3,1);add("VarCyMulI8",3,3);add("VarCyCmp",2,3);add("VarCyCmpR8",2,3);add("VarCyRound",3,1);
        for(const auto name:{"VarDecAbs","VarDecFix","VarDecInt","VarDecNeg","VarDecCmp"})add(name,2);
        for(const auto name:{"VarDecAdd","VarDecSub","VarDecMul","VarDecDiv","VarDecRound"})add(name,3);
        add("VarDecCmpR8",2,2);add("VarR4CmpR8",2,2);add("VarR8Pow",3,3);add("VarR8Round",3,1);
        for(const auto name:{"VarAbs","VarFix","VarInt"})add(name,2);
        for(const auto name:{"VarRound","VarEqv","VarImp","VarCat","VarPow"})add(name,3);
        return out;
    }();
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_oleaut32_conversion(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const auto name=service.symbol;const unsigned unit=x86_?4U:8U,variant_size=x86_?16U:24U;const auto bits=static_cast<std::uint8_t>(unit*8);
    // 純量參數：DECIMAL 是指標；x64 的浮點參數依位置在 XMM0～XMM3。
    const auto scalar_value=[this](const WindowsCall& call,const GuestState& s,GuestMemory& m,unsigned index,std::uint16_t type,unsigned bytes){
        Value v{};
        if(type==14){const auto pointer=call(index);if(!pointer)throw std::invalid_argument("DECIMAL 指標為空");m.read(GuestAddress{pointer},std::span{v}.first(16));put(v,0,2,14);return v;}
        auto raw=call(index);
        if(!x86_ && (type==4 || type==5 || type==7) && index<4){raw=0;for(unsigned i=0;i<bytes;++i)raw|=std::uint64_t(s.xmm[index][i])<<(i*8);}
        put(v,0,2,type);put(v,8,bytes,raw);return v;
    };
    // DECIMAL 輸出的 wReserved 不帶 VARIANT 的 vt；其他型別只寫自身大小。
    const auto store_scalar=[](GuestMemory& m,std::uint64_t pointer,Value v,std::uint16_t type,unsigned bytes){
        if(type==14){put(v,0,2,0);m.write(GuestAddress{pointer},std::span{v}.first(16));}
        else m.write(GuestAddress{pointer},std::span{v}.subspan(8,bytes));
    };
    const auto [target_name,source_name]=conversion_parts(name);
    const bool conversion=!target_name.empty() && (scalar_type(target_name).first || target_name=="Bstr") && (scalar_type(source_name).first || source_name=="Disp");
    if(conversion && source_name=="Disp"){
        // 經 VariantChangeTypeEx 取 DISPID_VALUE 並轉型，與 VARIANT 轉型共用 IDispatch 呼叫、遞迴深度與錯誤碼。
        const auto [type,bytes]=target_name=="Bstr"?std::pair<std::uint16_t,unsigned>{8,unit}:scalar_type(target_name);
        return nested_continuation(state,memory,"VarFromDisp",[this,service,type,bytes,variant_size,bits](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);const bool text=type==8;const auto output=call(text?3:2);
            const auto finish=[&](std::uint32_t status){call.return_to(s,status);return std::optional<std::uint32_t>{};};
            if(!output)co_return finish(invalid);
            GuestScratch temporary(*this,m,s);const auto block=temporary.allocate(variant_size*2);if(!block)co_return finish(nomemory);
            m.store(GuestAddress{block+variant_size},16,9);m.store(GuestAddress{block+variant_size+8},bits,call(0));
            const auto flags=text?static_cast<std::uint32_t>(call(2))&0xffU:0U;
            const auto status=co_await service_callback::call(resolve("oleaut32.dll","VariantChangeTypeEx").value,{block,block+variant_size,call(1),flags,type});
            if(!status){
                if(type==14){Value v{};m.read(GuestAddress{block},std::span{v}.first(16));put(v,0,2,0);m.write(GuestAddress{output},std::span{v}.first(16));}
                else {std::vector<std::uint8_t> value(bytes);m.read(GuestAddress{block+8},value);m.write(GuestAddress{output},value);}
            }
            co_return finish(status);
        });
    }
    if(name=="VarCat" || name=="VarPow" || name=="VarEqv" || name=="VarImp" ||
       ((name=="VarAbs" || name=="VarFix" || name=="VarInt" || name=="VarRound") && [&]{
            // 數值 VARIANT 直接在宿主端取整；字串、IDispatch 與 BYREF 才經轉型續行。
            const WindowsCall call(state,memory,service);if(!call(0))return false;
            const auto vt=memory.load(GuestAddress{call(0)},16);return vt==8 || vt==9 || (vt&0x4000U);}())){
        return nested_continuation(state,memory,"VariantOperation",[this,service,variant_size,bits](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);const auto name=service.symbol;
            const auto finish=[&](std::uint32_t status){call.return_to(s,status);return std::optional<std::uint32_t>{};};
            const auto entry=[this](std::string_view symbol){return resolve("oleaut32.dll",symbol).value;};
            const bool binary=name!="VarAbs" && name!="VarFix" && name!="VarInt" && name!="VarRound";
            const auto output=call(binary || name=="VarRound"?2:1);
            if(!call(0) || (binary && !call(1)) || !output)co_return finish(invalid);
            GuestScratch temporary(*this,m,s);const auto block=temporary.allocate(variant_size*3+16+variant_size);if(!block)co_return finish(nomemory);
            // 暫存 VARIANT 一律經 VariantClear 釋放，與既有 Clear／SafeArray／COM 釋放路徑一致。
            std::uint32_t status=0;const auto clear=[&]()->service_callback::Task<int>{
                for(unsigned i=0;i<3;++i)if(m.load(GuestAddress{block+i*variant_size},16))(void)co_await service_callback::call(entry("VariantClear"),{block+i*variant_size});
                co_return 0;};
            if(name=="VarEqv" || name=="VarImp"){
                // Eqv＝Not(Xor)；Imp＝Or(Not 左, 右)。NULL 規則由既有 VarXor／VarNot／VarOr 處理。
                if(name=="VarEqv"){status=co_await service_callback::call(entry("VarXor"),{call(0),call(1),block});if(!status)status=co_await service_callback::call(entry("VarNot"),{block,output});}
                else {status=co_await service_callback::call(entry("VarNot"),{call(0),block});if(!status)status=co_await service_callback::call(entry("VarOr"),{block,call(1),output});}
                (void)co_await clear();co_return finish(status);
            }
            const auto vt=[&](std::uint64_t variant){return m.load(GuestAddress{variant},16);};
            if(name=="VarCat"){
                // 兩邊皆 NULL 才回 NULL；NULL／EMPTY 視為空字串，其他型別以 VARIANT_ALPHABOOL 轉成 BSTR 後串接。
                const auto left=vt(call(0)),right=vt(call(1));
                if(left==1 && right==1){m.store(GuestAddress{output},16,1);co_return finish(0);}
                std::array<std::uint64_t,2> strings{};
                for(unsigned side=0;side<2;++side){
                    const auto variant=call(side);const auto t=side?right:left;
                    if(t==0 || t==1)continue;
                    if(t==8){strings[side]=m.load(GuestAddress{variant+8},bits);continue;}
                    status=co_await service_callback::call(entry("VariantChangeTypeEx"),{block+side*variant_size,variant,0x400,2,8});
                    if(status){(void)co_await clear();m.store(GuestAddress{output},16,0);co_return finish(status);}
                    strings[side]=m.load(GuestAddress{block+side*variant_size+8},bits);
                }
                status=co_await service_callback::call(entry("VarBstrCat"),{strings[0],strings[1],block+2*variant_size+8});
                if(!status){
                    // 結果 BSTR 的擁有權移交輸出 VARIANT，暫存槽清回 VT_EMPTY 以免被釋放。
                    std::vector<std::uint8_t> result(variant_size,0);result[0]=8;const auto pointer=m.load(GuestAddress{block+2*variant_size+8},bits);
                    for(unsigned i=0;i<bits/8U;++i)result[8+i]=static_cast<std::uint8_t>(pointer>>(i*8));
                    m.write(GuestAddress{output},result);m.store(GuestAddress{block+2*variant_size+8},bits,0);
                }
                else m.store(GuestAddress{output},16,0);
                (void)co_await clear();co_return finish(status);
            }
            if(name=="VarPow"){
                // 任一邊 NULL 回 NULL；其餘先轉 R8（字串依使用者地區設定解析、IDispatch 取預設值）再計算。
                if(vt(call(0))==1 || vt(call(1))==1){m.store(GuestAddress{output},16,1);co_return finish(0);}
                for(unsigned side=0;side<2 && !status;++side)status=co_await service_callback::call(entry("VariantChangeTypeEx"),{block+side*variant_size,call(side),0x400,0,5});
                if(!status){
                    const auto x=std::bit_cast<double>(m.load(GuestAddress{block+8},64)),y=std::bit_cast<double>(m.load(GuestAddress{block+variant_size+8},64));
                    m.write(GuestAddress{output},std::vector<std::uint8_t>(variant_size,0));m.store(GuestAddress{output},16,5);m.store(GuestAddress{output+8},64,std::bit_cast<std::uint64_t>(std::pow(x,y)));
                }else m.store(GuestAddress{output},16,0);
                (void)co_await clear();co_return finish(status);
            }
            // VarAbs／VarFix／VarInt／VarRound：BYREF 先解參照，IDispatch 取 DISPID_VALUE（保留原型別），字串再依使用者地區設定轉 R8。
            auto current=call(0);
            if(vt(current)&0x4000U){status=co_await service_callback::call(entry("VariantCopyInd"),{block,current});current=block;}
            if(!status && vt(current)==9){
                const auto object=m.load(GuestAddress{current+8},bits);
                if(!object)status=mismatch;
                else {
                    // Invoke(this, DISPID_VALUE, IID_NULL, lcid, DISPATCH_PROPERTYGET, 空 DISPPARAMS, 結果, NULL, NULL)。
                    const auto iid=block+3*variant_size,parameters=iid+16;
                    const auto invoke=m.load(GuestAddress{m.load(GuestAddress{object},bits)+6*(bits/8U)},bits);
                    status=co_await service_callback::call(invoke,{object,0,iid,0x400,2,parameters,block+variant_size,0,0});
                    current=block+variant_size;
                }
            }
            if(!status && vt(current)==8){status=co_await service_callback::call(entry("VariantChangeTypeEx"),{block+2*variant_size,current,0x400,0,5});current=block+2*variant_size;}
            if(!status){
                const auto value=[&]{Value v{};m.read(GuestAddress{current},std::span{v}.first(variant_size));return v;}();
                const auto kind=name=="VarAbs"?Rounding::Abs:name=="VarFix"?Rounding::Fix:name=="VarInt"?Rounding::Int:Rounding::Round;
                const auto vt_value=static_cast<std::uint16_t>(get(value,0,2));Value out{};
                if(vt_value==1)put(out,0,2,1);
                else if(const auto type_status=operator_types(Operator::Neg,vt_value))status=type_status;
                else if(vt_value==9 || vt_value==13)status=mismatch;
                else status=number_rounding(kind,value,static_cast<std::int32_t>(static_cast<std::uint32_t>(call(1))),out);
                if(!status)m.write(GuestAddress{output},std::span{out}.first(variant_size));else m.store(GuestAddress{output},16,0);
            }else m.store(GuestAddress{output},16,0);
            (void)co_await clear();co_return finish(status);
        });
    }
    const WindowsCall arg(state,memory,service);
    const auto done=[&](std::uint32_t status){arg.return_to(state,status);return std::optional<std::uint32_t>{};};
    try{
        if(conversion){
            const auto [target,target_bytes]=scalar_type(target_name);const auto [source,source_bytes]=scalar_type(source_name);
            if(!arg(1))return done(invalid);
            Value out{};const auto status=number_convert(scalar_value(arg,state,memory,0,source,source_bytes),{},target,out);
            if(!status)store_scalar(memory,arg(1),out,target,target_bytes);return done(status);
        }
        if(name=="VarAbs" || name=="VarFix" || name=="VarInt" || name=="VarRound"){
            const bool round=name=="VarRound";const auto output=arg(round?2:1);if(!arg(0) || !output)return done(invalid);
            Value value{};memory.read(GuestAddress{arg(0)},std::span{value}.first(variant_size));
            const auto vt=static_cast<std::uint16_t>(get(value,0,2));Value out{};
            const auto fail=[&](std::uint32_t status){memory.store(GuestAddress{output},16,0);return done(status);};
            if(vt==1){put(out,0,2,1);memory.write(GuestAddress{output},std::span{out}.first(variant_size));return done(0);}
            if(const auto status=operator_types(Operator::Neg,vt))return fail(status);
            const auto kind=name=="VarAbs"?Rounding::Abs:name=="VarFix"?Rounding::Fix:name=="VarInt"?Rounding::Int:Rounding::Round;
            const auto status=number_rounding(kind,value,round?static_cast<std::int32_t>(static_cast<std::uint32_t>(arg(1))):0,out);
            if(status)return fail(status);memory.write(GuestAddress{output},std::span{out}.first(variant_size));return done(0);
        }
        // CY／DECIMAL／R4／R8 的純量運算：以 VARIANT 值包裝後交給 Automation 共用的運算、比較與取整。
        const bool cy=name.starts_with("VarCy"),dec=name.starts_with("VarDec");
        const auto operand=[&](unsigned index,std::uint16_t type){return scalar_value(arg,state,memory,index,type,type==14?16U:type==4 || type==3?4U:8U);};
        const std::uint16_t own=cy?6:dec?14:5;const unsigned own_bytes=cy?8:dec?16:8;
        if(name.ends_with("CmpR8") || name=="VarCyCmp" || name=="VarDecCmp"){
            const auto left=operand(0,name=="VarR4CmpR8"?4:own),right=operand(1,name.ends_with("CmpR8")?5:own);
            return done(number_compare(left,right));
        }
        const auto operation=[&](Operator op,std::uint16_t right_type)->std::optional<std::uint32_t>{
            const auto output=arg(2);if(!output)return done(invalid);
            Value out{};const auto status=number_operation(op,operand(0,own),operand(1,right_type),out);
            if(status)return done(status);
            if(get(out,0,2)!=own){Value converted{};if(const auto again=number_convert(out,{},own,converted))return done(again);out=converted;}
            store_scalar(memory,output,out,own,own_bytes);return done(0);
        };
        if(name.ends_with("Add"))return operation(Operator::Add,own);
        if(name.ends_with("Sub"))return operation(Operator::Sub,own);
        if(name.ends_with("Mul"))return operation(Operator::Mul,own);
        if(name=="VarDecDiv")return operation(Operator::Div,own);
        if(name=="VarCyMulI4")return operation(Operator::Mul,3);
        if(name=="VarCyMulI8")return operation(Operator::Mul,20);
        if(name=="VarR8Pow"){
            if(!arg(2))return done(invalid);
            const auto x=std::bit_cast<double>(get(operand(0,5),8,8)),y=std::bit_cast<double>(get(operand(1,5),8,8));
            memory.store(GuestAddress{arg(2)},64,std::bit_cast<std::uint64_t>(std::pow(x,y)));return done(0);
        }
        const bool round=name.ends_with("Round");const auto output=arg(round?2:1);if(!output)return done(invalid);
        const auto source=operand(0,own);Value out{};std::uint32_t status=0;
        if(name.ends_with("Neg"))status=number_operation(Operator::Neg,source,{},out);
        else {
            const auto kind=name.ends_with("Abs")?Rounding::Abs:name.ends_with("Fix")?Rounding::Fix:name.ends_with("Int")?Rounding::Int:Rounding::Round;
            status=number_rounding(kind,source,round?static_cast<std::int32_t>(static_cast<std::uint32_t>(arg(1))):0,out);
        }
        if(!status)store_scalar(memory,output,out,own,own_bytes);return done(status);
    }catch(const std::invalid_argument&){return done(invalid);}catch(const MemoryFault&){return done(invalid);}catch(const std::bad_alloc&){return done(nomemory);}
}
}

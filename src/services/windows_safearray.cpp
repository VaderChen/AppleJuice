#include "x64arm/windows_services.hpp"
#include <algorithm>
#include <functional>
#include <set>

namespace x64arm {
struct SafeArrayState { struct Allocation {unsigned dimensions{};std::uint64_t data{};};std::map<std::uint64_t,Allocation> arrays; };
std::span<const WindowsServiceDescriptor> safearray_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"oleaut32.dll","SafeArrayCreate",3},{"oleaut32.dll","SafeArrayCreateEx",4},{"oleaut32.dll","SafeArrayCreateVector",3},{"oleaut32.dll","SafeArrayCreateVectorEx",4},
        {"oleaut32.dll","SafeArrayAllocDescriptor",2},{"oleaut32.dll","SafeArrayAllocDescriptorEx",3},{"oleaut32.dll","SafeArrayAllocData",1},
        {"oleaut32.dll","SafeArrayDestroy",1},{"oleaut32.dll","SafeArrayDestroyDescriptor",1},{"oleaut32.dll","SafeArrayDestroyData",1},
        {"oleaut32.dll","SafeArrayGetDim",1},{"oleaut32.dll","SafeArrayGetElemsize",1},{"oleaut32.dll","SafeArrayGetVartype",2},
        {"oleaut32.dll","SafeArrayGetLBound",3},{"oleaut32.dll","SafeArrayGetUBound",3},
        {"oleaut32.dll","SafeArrayLock",1},{"oleaut32.dll","SafeArrayUnlock",1},{"oleaut32.dll","SafeArrayAccessData",2},{"oleaut32.dll","SafeArrayUnaccessData",1},
        {"oleaut32.dll","SafeArrayGetElement",3},{"oleaut32.dll","SafeArrayPutElement",3},{"oleaut32.dll","SafeArrayPtrOfIndex",3},
        {"oleaut32.dll","SafeArrayCopy",2},{"oleaut32.dll","SafeArrayCopyData",2},{"oleaut32.dll","SafeArrayRedim",2},
        {"oleaut32.dll","SafeArraySetIID",2},{"oleaut32.dll","SafeArrayGetIID",2}
    };return table;
}
namespace {
constexpr std::uint32_t invalid=0x80070057U,oom=0x8007000eU,locked=0x8002000dU,badindex=0x8002000bU,badtype=0x80020008U,notimpl=0x80004001U;
struct ArrayError {std::uint32_t status;};
struct ArrayView {std::uint64_t address{},data{},count{};unsigned dims{},flags{},size{},locks{},type{};std::vector<std::pair<std::uint32_t,std::int32_t>> bounds;};
// 所有位址與描述元皆為客體格式；宿主僅追蹤自己配置的儲存區。
class ArrayOperations {
public:
    GuestMemory& memory;SafeArrayState& state;unsigned unit,bits,header,variant_size;
    std::function<std::uint64_t(std::uint64_t)> allocate;
    std::function<void(std::uint64_t)> release;
    std::function<bool(std::uint64_t)> owned;
    std::function<void(std::uint64_t)> executable;
    std::vector<GuestCallback> callbacks;
    std::vector<std::uint64_t> provisional;
    bool committed{};
    unsigned validation_callbacks{};
    ArrayOperations(GuestMemory& m,SafeArrayState& s,bool x86):memory(m),state(s),unit(x86?4:8),bits(unit*8),header(x86?16:24),variant_size(x86?16:24){}
    ~ArrayOperations(){if(!committed)for(auto i=provisional.rbegin();i!=provisional.rend();++i){state.arrays.erase(*i+16);release(*i);}}
    std::uint64_t load(std::uint64_t p,unsigned b)const{return memory.load(GuestAddress{p},static_cast<std::uint8_t>(b));}
    void store(std::uint64_t p,unsigned b,std::uint64_t v){memory.store(GuestAddress{p},static_cast<std::uint8_t>(b),v);}
    std::uint64_t alloc(std::uint64_t size){if(size>WindowsServices::output_limit)throw ArrayError{oom};const auto p=allocate(std::max(UINT64_C(1),size));if(!p)throw ArrayError{oom};try{provisional.push_back(p);}catch(...){release(p);throw;}return p;}
    void free(std::uint64_t p){if(!p)return;std::erase(provisional,p);release(p);}
    void copy_bytes(std::uint64_t dst,std::uint64_t src,std::uint64_t size){if(!size)return;std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));memory.read(GuestAddress{src},bytes);memory.write(GuestAddress{dst},bytes);}
    unsigned element_size(unsigned type)const{switch(type){case 16:case 17:return 1;case 2:case 11:case 18:return 2;case 3:case 4:case 10:case 19:case 22:case 23:return 4;case 5:case 6:case 7:case 20:case 21:return 8;case 8:case 9:case 13:return unit;case 12:return variant_size;case 14:return 16;default:throw ArrayError{badtype};}}
    ArrayView view(std::uint64_t p,bool need_data=true){
        if(!p)throw ArrayError{invalid};ArrayView a;a.address=p;a.dims=static_cast<unsigned>(load(p,16));a.flags=static_cast<unsigned>(load(p+2,16));a.size=static_cast<unsigned>(load(p+4,32));a.locks=static_cast<unsigned>(load(p+8,32));a.data=load(p+(unit==4?12:16),bits);
        if(!a.dims || a.dims>64)throw ArrayError{invalid};if(const auto i=state.arrays.find(p);i!=state.arrays.end() && a.dims!=i->second.dimensions)throw ArrayError{invalid};
        if(a.flags&0x20)throw ArrayError{notimpl};if(a.flags&0x100)a.type=8;else if(a.flags&0x200)a.type=13;else if(a.flags&0x400)a.type=9;else if(a.flags&0x800)a.type=12;else if(a.flags&0x80)a.type=static_cast<unsigned>(load(p-4,32));
        if(a.type && a.size!=element_size(a.type))throw ArrayError{invalid};a.count=1;
        for(unsigned i=0;i<a.dims;++i){const auto count=static_cast<std::uint32_t>(load(p+header+i*8,32));const auto lower=static_cast<std::int32_t>(load(p+header+i*8+4,32));if(count && static_cast<std::int64_t>(lower)+count-1>INT32_MAX)throw ArrayError{invalid};a.bounds.emplace_back(count,lower);if(count && a.count>WindowsServices::output_limit/count)throw ArrayError{oom};a.count*=count;}
        if(a.size && a.count>WindowsServices::output_limit/a.size)throw ArrayError{oom};if(need_data && a.count && (!a.size || !a.data))throw ArrayError{invalid};if(need_data && a.count)memory.check(GuestAddress{a.data},a.count*a.size,Access::Read);return a;
    }
    void own_array(const ArrayView& a){const auto i=state.arrays.find(a.address);if(i==state.arrays.end() || i->second.data!=a.data)throw ArrayError{invalid};}
    void add_callback(std::uint64_t object,unsigned method){if(!object)return;if(callbacks.size()>=65536)throw ArrayError{oom};const auto target=load(load(object,bits)+method*unit,bits);executable(target);callbacks.push_back({target,{object}});}
    std::uint64_t bstr_copy(std::uint64_t p){if(!p)return 0;const auto length=load(p-4,32);if(length>WindowsServices::output_limit-10)throw ArrayError{oom};const auto target=alloc(length+10);store(target,32,0x52545342);store(target+4,32,length);copy_bytes(target+8,p,length);return target+8;}
    void bstr_validate(std::uint64_t p,bool clearing){if(!p)return;const auto length=load(p-4,32);if(length>WindowsServices::output_limit-10)throw ArrayError{oom};memory.check(GuestAddress{p},length,Access::Read);if(clearing && (!owned(p-8) || load(p-8,32)!=0x52545342))throw ArrayError{invalid};}
    void validate_value(unsigned type,std::uint64_t p,bool clearing,unsigned depth,std::set<std::uint64_t>& chain){
        if(depth>32)throw ArrayError{invalid};if(type==12){const auto vt=static_cast<unsigned>(load(p,16));if(vt&0x9000)throw ArrayError{badtype};const auto base=vt&0xfff;if(base==15 || (base>23 && base!=36) || ((vt&0x6000) && base<=1))throw ArrayError{badtype};if(vt&0x4000)return;if(vt&0x2000){validate_array(load(p+8,bits),clearing,depth+1,chain);return;}if(base==0 || base==1)return;if(base==12 || base==36)throw ArrayError{notimpl};(void)element_size(base);validate_value(base,p+(base==14?0:8),clearing,depth+1,chain);return;}
        if(type==8)bstr_validate(load(p,bits),clearing);
        if(type==9 || type==13){const auto object=load(p,bits);if(object){if(++validation_callbacks>65536)throw ArrayError{oom};callbacks.reserve(validation_callbacks);executable(load(load(object,bits)+(clearing?2:1)*unit,bits));}}
    }
    void validate_array(std::uint64_t p,bool clearing,unsigned depth,std::set<std::uint64_t>& chain){if(!p)return;if(depth>32 || !chain.insert(p).second)throw ArrayError{invalid};const auto a=view(p);if(clearing){if(a.locks)throw ArrayError{locked};own_array(a);}for(std::uint64_t i=0;i<a.count;++i)validate_value(a.type,a.data+i*a.size,clearing,depth+1,chain);chain.erase(p);}
    void validate(std::uint64_t p,bool clearing){std::set<std::uint64_t> chain;validate_array(p,clearing,0,chain);}
    void validate_element(unsigned type,std::uint64_t p,bool clearing){std::set<std::uint64_t> chain;validate_value(type,p,clearing,0,chain);}
    std::uint64_t descriptor(unsigned type,unsigned dims){
        if(!dims || dims>64 || state.arrays.size()>=65536)throw ArrayError{invalid};const auto size=type?element_size(type):0;const auto p=alloc(16+header+8*dims)+16;unsigned flags=type?0x80:0;
        if(type==8)flags|=0x100;if(type==12)flags|=0x800;if(type==9 || type==13)flags=(type==9?0x400:0x200)|0x40;
        store(p,16,dims);store(p+2,16,flags);store(p+4,32,size);if(flags&0x80)store(p-4,32,type);
        if(flags&0x40){const std::array<std::uint8_t,16> iid{0,static_cast<std::uint8_t>(type==9?4:0),2,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};auto actual=iid;if(type==13)actual[2]=0;memory.write(GuestAddress{p-16},actual);}
        state.arrays.emplace(p,SafeArrayState::Allocation{dims,0});return p;
    }
    void allocate_data(std::uint64_t p){const auto a=view(p,false);own_array(a);if(a.data || !a.size)throw ArrayError{invalid};const auto data=alloc(a.count*a.size);store(p+(unit==4?12:16),bits,data);state.arrays.at(p).data=data;}
    void copy_value(unsigned type,unsigned size,std::uint64_t dst,std::uint64_t src,unsigned depth=0){
        if(depth>32)throw ArrayError{invalid};copy_bytes(dst,src,size);
        if(type==12){const auto vt=static_cast<unsigned>(load(src,16));if(vt&0x4000)return;if(vt&0x2000){store(dst+8,bits,clone(load(src+8,bits),depth+1));return;}const auto base=vt&0xfff;if(base==0 || base==1 || base==14)return;copy_value(base,element_size(base),dst+8,src+8,depth+1);}
        else if(type==8)store(dst,bits,bstr_copy(load(src,bits)));
        else if(type==9 || type==13)add_callback(load(src,bits),1);
    }
    void clear_value(unsigned type,std::uint64_t p,unsigned depth=0){
        if(depth>32)throw ArrayError{invalid};if(type==12){const auto vt=static_cast<unsigned>(load(p,16));if(!(vt&0x4000)){if(vt&0x2000)destroy(load(p+8,bits));else if((vt&0xfff)!=14)clear_value(vt&0xfff,p+8,depth+1);}store(p,16,0);}
        else if(type==8){const auto value=load(p,bits);if(value)free(value-8);store(p,bits,0);}
        else if(type==9 || type==13){add_callback(load(p,bits),2);store(p,bits,0);}
    }
    std::uint64_t clone(std::uint64_t p,unsigned depth=0){if(!p)return 0;if(depth>32)throw ArrayError{invalid};const auto a=view(p);const auto target=descriptor(a.type,a.dims);copy_bytes(target,p,header+8*a.dims);store(target+2,16,a.flags&~0x2017U);store(target+8,32,0);store(target+(unit==4?12:16),bits,0);if(a.flags&0x40)copy_bytes(target-16,p-16,16);else if(a.flags&0x80)copy_bytes(target-4,p-4,4);allocate_data(target);const auto data=state.arrays.at(target).data;for(std::uint64_t i=0;i<a.count;++i)copy_value(a.type,a.size,data+i*a.size,a.data+i*a.size,depth+1);return target;}
    void destroy(std::uint64_t p,bool descriptor_only=false,bool data_only=false){
        if(!p)return;const auto a=view(p,false);own_array(a);if(a.locks)throw ArrayError{locked};if(!descriptor_only && a.data){for(std::uint64_t i=0;i<a.count;++i)clear_value(a.type,a.data+i*a.size);free(a.data);state.arrays.at(p).data=0;store(p+(unit==4?12:16),bits,0);}if(!data_only){state.arrays.erase(p);free(p-16);}
    }
    std::uint64_t index(const ArrayView& a,std::uint64_t indices){if(!indices)throw ArrayError{invalid};std::uint64_t offset=0,stride=1;for(unsigned i=0;i<a.dims;++i){const auto& bound=a.bounds[a.dims-i-1];const auto index_value=static_cast<std::int64_t>(static_cast<std::int32_t>(load(indices+i*4,32)))-bound.second;if(index_value<0 || static_cast<std::uint64_t>(index_value)>=bound.first)throw ArrayError{badindex};offset+=static_cast<std::uint64_t>(index_value)*stride;stride*=bound.first;}return a.data+offset*a.size;}
};
}
std::uint64_t WindowsServices::create_safearray_bytes(GuestState& guest,GuestMemory& memory,std::span<const std::uint8_t> bytes){
    if(!safe_arrays_)safe_arrays_=std::make_shared<SafeArrayState>();ArrayOperations op(memory,*safe_arrays_,x86_);
    op.allocate=[&](std::uint64_t size){return allocate_heap(memory,size);};op.release=[&](std::uint64_t p){(void)free_heap(memory,p,guest);};
    try{
        if(bytes.size()>output_limit)return 0;const auto array=op.descriptor(17,1);
        op.store(array+op.header,32,bytes.size());op.store(array+op.header+4,32,0);op.store(array+2,16,op.load(array+2,16)|0x10);
        op.allocate_data(array);const auto view=op.view(array);
        if(!bytes.empty())memory.write(GuestAddress{view.data},bytes);op.committed=true;return array;
    }catch(const ArrayError&){return 0;}catch(const std::bad_alloc&){return 0;}
}
std::optional<std::uint32_t> WindowsServices::invoke_safearray(GuestState& guest,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(guest,memory,service);const auto name=service.symbol;const auto d=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    if(!safe_arrays_)safe_arrays_=std::make_shared<SafeArrayState>();ArrayOperations op(memory,*safe_arrays_,x86_);
    op.allocate=[&](std::uint64_t size){return allocate_heap(memory,size);};op.release=[&](std::uint64_t p){(void)free_heap(memory,p,guest);};op.owned=[&](std::uint64_t p){return allocations_.contains(p);};op.executable=[&](std::uint64_t p){if(!handles(GuestAddress{p}))memory.check(GuestAddress{p},1,Access::Execute);};
    const bool creating=name.starts_with("SafeArrayCreate");
    std::uint64_t callback_lock=0;
    const auto done=[&](std::uint64_t result)->std::optional<std::uint32_t>{op.committed=true;if(!op.callbacks.empty()){if(callback_lock)op.store(callback_lock+8,32,op.load(callback_lock+8,32)+1);start_callbacks(guest,memory,std::move(op.callbacks),callback_lock?Continuation::SafeArrayOperation:Continuation::Return,callback_lock?callback_lock:result);return std::nullopt;}arg.return_to(guest,result);return std::nullopt;};
    try{
        if(name=="VariantClear" || name=="VariantCopy"){
            const bool copying=name=="VariantCopy";if(!arg(0) || (copying && !arg(1)))throw ArrayError{invalid};if(copying && arg(0)==arg(1))return done(0);
            memory.check(GuestAddress{arg(0)},op.variant_size,Access::Write);op.validate_element(12,arg(0),true);
            std::uint64_t temporary=0;if(copying){op.validate_element(12,arg(1),false);temporary=op.alloc(op.variant_size);op.copy_value(12,op.variant_size,temporary,arg(1));}
            op.clear_value(12,arg(0));if(copying){op.copy_bytes(arg(0),temporary,op.variant_size);op.free(temporary);}return done(0);
        }
        if(creating || name=="SafeArrayAllocDescriptor" || name=="SafeArrayAllocDescriptorEx"){
            const bool untyped=name=="SafeArrayAllocDescriptor",vector=name.find("Vector")!=std::string_view::npos;const auto type=untyped?0:d(0);const auto dims=untyped?d(0):vector?1:d(1);const auto output=creating?0:arg(untyped?1:2);if(!creating){if(!output)throw ArrayError{invalid};op.store(output,op.bits,0);}
            const auto p=op.descriptor(type,dims);if(creating){
                if(vector){op.store(p+op.header,32,d(2));op.store(p+op.header+4,32,d(1));op.store(p+2,16,op.load(p+2,16)|0x10);}
                else {if(!arg(2))throw ArrayError{invalid};for(unsigned i=0;i<dims;++i)op.copy_bytes(p+op.header+i*8,arg(2)+(dims-i-1)*8,8);}
                if(name.ends_with("Ex") && arg(3)){if(type!=9 && type!=13)throw ArrayError{invalid};op.copy_bytes(p-16,arg(3),16);}op.allocate_data(p);return done(p);
            }op.store(output,op.bits,p);return done(0);
        }
        if(name=="SafeArrayGetDim" || name=="SafeArrayGetElemsize")return done(arg(0)?op.load(arg(0)+(name=="SafeArrayGetDim"?0:4),name=="SafeArrayGetDim"?16:32):0);
        if(!arg(0)){if(name=="SafeArrayDestroy" || name=="SafeArrayDestroyData" || name=="SafeArrayDestroyDescriptor")return done(0);if(name=="SafeArrayCopy" && arg(1)){op.store(arg(1),op.bits,0);return done(0);}throw ArrayError{invalid};}
        const auto a=op.view(arg(0),false);
        if(name=="SafeArrayGetVartype"){if(!arg(1) || !a.type)throw ArrayError{invalid};op.store(arg(1),16,a.type);return done(0);}
        if(name=="SafeArrayGetLBound" || name=="SafeArrayGetUBound"){if(!arg(2))throw ArrayError{invalid};if(!d(1) || d(1)>a.dims)throw ArrayError{badindex};const auto& bound=a.bounds[a.dims-d(1)];const auto value=static_cast<std::int64_t>(bound.second)+(name=="SafeArrayGetUBound"?static_cast<std::int64_t>(bound.first)-1:0);if(value<INT32_MIN || value>INT32_MAX)throw ArrayError{badindex};op.store(arg(2),32,static_cast<std::uint32_t>(value));return done(0);}
        if(name=="SafeArraySetIID" || name=="SafeArrayGetIID"){if(!(a.flags&0x40) || !arg(1))throw ArrayError{invalid};if(name=="SafeArrayGetIID")op.copy_bytes(arg(1),a.address-16,16);else op.copy_bytes(a.address-16,arg(1),16);return done(0);}
        if(name=="SafeArrayLock" || name=="SafeArrayAccessData"){if(a.locks==UINT32_MAX)throw ArrayError{0x8000ffffU};if(name=="SafeArrayAccessData"){if(!arg(1))throw ArrayError{invalid};op.store(arg(1),op.bits,a.data);}op.store(a.address+8,32,a.locks+1);return done(0);}
        if(name=="SafeArrayUnlock" || name=="SafeArrayUnaccessData"){if(!a.locks)throw ArrayError{0x8000ffffU};op.store(a.address+8,32,a.locks-1);return done(0);}
        if(name=="SafeArrayAllocData"){op.allocate_data(a.address);return done(0);}
        if(name.starts_with("SafeArrayDestroy")){if(name!="SafeArrayDestroyDescriptor" && a.data)op.validate(a.address,true);op.destroy(a.address,name=="SafeArrayDestroyDescriptor",name=="SafeArrayDestroyData");return done(0);}
        if(name=="SafeArrayGetElement" || name=="SafeArrayPutElement" || name=="SafeArrayPtrOfIndex"){
            if(a.locks==UINT32_MAX)throw ArrayError{0x8000ffffU};const auto source=op.index(op.view(a.address),arg(1));if(name=="SafeArrayPtrOfIndex"){if(!arg(2))throw ArrayError{invalid};op.store(arg(2),op.bits,source);return done(0);}const bool put=name=="SafeArrayPutElement";if(!arg(2) && (!put || (a.type!=8 && a.type!=9 && a.type!=13)))throw ArrayError{invalid};
            const auto temporary=op.alloc(a.size);if(put){std::uint64_t from=arg(2),pointer_slot=0;if(a.type==8 || a.type==9 || a.type==13){pointer_slot=op.alloc(op.unit);op.store(pointer_slot,op.bits,from);from=pointer_slot;}op.validate_element(a.type,from,false);op.validate_element(a.type,source,true);op.copy_value(a.type,a.size,temporary,from);op.clear_value(a.type,source);op.copy_bytes(source,temporary,a.size);if(pointer_slot)op.free(pointer_slot);}
            else {memory.check(GuestAddress{arg(2)},a.size,Access::Write);op.validate_element(a.type,source,false);op.copy_value(a.type,a.size,temporary,source);op.copy_bytes(arg(2),temporary,a.size);}op.free(temporary);callback_lock=a.address;return done(0);
        }
        if(name=="SafeArrayCopy" || name=="SafeArrayCopyData"){
            if(!arg(1))throw ArrayError{invalid};if(name=="SafeArrayCopy")op.store(arg(1),op.bits,0);op.validate(a.address,false);
            if(name=="SafeArrayCopyData"){
                if(a.address==arg(1))return done(0);const auto b=op.view(arg(1));if(a.dims!=b.dims || a.size!=b.size || a.type!=b.type)throw ArrayError{invalid};for(unsigned i=0;i<a.dims;++i)if(a.bounds[i].first!=b.bounds[i].first)throw ArrayError{invalid};op.own_array(b);for(std::uint64_t i=0;i<b.count;++i)op.validate_element(b.type,b.data+i*b.size,true);
                const auto replacement=op.clone(a.address);const auto data=op.state.arrays.at(replacement).data;for(std::uint64_t i=0;i<b.count;++i)op.clear_value(b.type,b.data+i*b.size);op.copy_bytes(b.data,data,b.count*b.size);op.free(data);op.state.arrays.at(replacement).data=0;op.store(replacement+(op.unit==4?12:16),op.bits,0);op.destroy(replacement,true);return done(0);
            }const auto replacement=op.clone(a.address);op.store(arg(1),op.bits,replacement);return done(0);
        }
        if(name=="SafeArrayRedim"){
            if(!arg(1))throw ArrayError{invalid};if(a.locks || (a.flags&0x10))throw ArrayError{locked};op.own_array(a);op.validate(a.address,true);const auto count=static_cast<std::uint32_t>(op.load(arg(1),32));const auto lower=static_cast<std::int32_t>(op.load(arg(1)+4,32));if(count && static_cast<std::int64_t>(lower)+count-1>INT32_MAX)throw ArrayError{invalid};std::uint64_t stride=1;for(unsigned i=1;i<a.dims;++i)stride*=a.bounds[i].first;if(count && stride>output_limit/count)throw ArrayError{oom};const auto total=stride*count;if(!a.size || total>output_limit/a.size)throw ArrayError{oom};
            if(count!=a.bounds[0].first){const auto data=op.alloc(total*a.size),kept=std::min(a.count,total);op.copy_bytes(data,a.data,kept*a.size);for(auto i=kept;i<a.count;++i)op.clear_value(a.type,a.data+i*a.size);op.free(a.data);op.state.arrays.at(a.address).data=data;op.store(a.address+(op.unit==4?12:16),op.bits,data);}op.store(a.address+op.header,32,count);op.store(a.address+op.header+4,32,static_cast<std::uint32_t>(lower));return done(0);
        }
        throw ArrayError{notimpl};
    }catch(const ArrayError& error){arg.return_to(guest,creating?0:error.status);return std::nullopt;}catch(const std::bad_alloc&){arg.return_to(guest,creating?0:oom);return std::nullopt;}
}
}

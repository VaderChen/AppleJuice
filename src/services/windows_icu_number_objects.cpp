#include "x64arm/windows_services.hpp"
#include "icu_guest_abi.hpp"
#include "icu_number_layout.hpp"
#include "icu_number_state.hpp"

namespace x64arm {
using namespace icu_abi;

bool WindowsServices::publish_icu_field_positions(GuestState& state,GuestMemory& memory,
        std::uint64_t handle,UFieldPositionIterator& source){
    if(!handle)return true;
    if(!icu_number_)return false;
    const auto found=icu_number_->positions.find(handle);
    if(found==icu_number_->positions.end())return false;
    std::vector<icu_number::FieldPosition> positions;
    for(;;){
        icu_number::FieldPosition position{};
        position.field=ufieldpositer_next(&source,&position.begin,&position.end);
        if(position.field<0)break;
        positions.push_back(position);
    }
    const auto records=positions.empty()?0:allocate_heap(memory,positions.size()*sizeof(positions.front()));
    if(!records && !positions.empty())return false;
    const auto bits=static_cast<std::uint8_t>(x86_?32:64);
    try{
        if(records)memory.write(GuestAddress{records},icu_bytes(positions.data(),positions.size()));
        memory.store(GuestAddress{handle},32,positions.size());
        memory.store(GuestAddress{handle+4},32,0);
        memory.store(GuestAddress{handle+8},bits,records);
    }catch(...){
        if(records)(void)free_heap(memory,records,state);
        throw;
    }
    if(const auto previous=std::exchange(found->second,records))(void)free_heap(memory,previous,state);
    return true;
}

void WindowsServices::publish_icu_formattable(GuestMemory& memory,std::uint64_t handle,const void* provider){
    const auto* value=static_cast<const UFormattable*>(provider);
    UErrorCode status=U_ZERO_ERROR;
    const auto type=ufmt_getType(value,&status);
    const icu_number::FormattableView view{static_cast<std::int32_t>(type),ufmt_isNumeric(value)?1U:0U};
    memory.write(GuestAddress{handle},icu_bytes(&view,1));
}

std::uint64_t WindowsServices::adopt_icu_formattable(GuestState& state,GuestMemory& memory,IcuFormattableObject object){
    if(!object.get())return 0;
    if(!icu_number_)icu_number_=std::make_shared<IcuNumberState>();
    const auto handle=allocate_heap(memory,sizeof(icu_number::FormattableView));
    if(!handle)return 0;
    try{
        publish_icu_formattable(memory,handle,object.get());
        icu_number_->values.emplace(handle,std::move(object));
    }catch(...){
        (void)free_heap(memory,handle,state);
        throw;
    }
    return handle;
}

void WindowsServices::clear_icu_formattable_references(GuestState& state,GuestMemory& memory,IcuFormattableObject& object){
    for(const auto& entry:object.children){
        const auto child=icu_number_->values.find(entry.second);
        if(child==icu_number_->values.end())continue;
        clear_icu_formattable_references(state,memory,child->second);
        icu_number_->values.erase(child);
        (void)free_heap(memory,entry.second,state);
    }
    object.children.clear();
    for(const auto& entry:object.objects)(void)free_heap(memory,entry.second,state);
    object.objects.clear();
    for(std::size_t index=0;index<object.text.size();++index){
        if(object.text[index])(void)free_heap(memory,object.text[index],state);
        object.text[index]=0;
        object.capacities[index]=0;
    }
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_number_object(GuestState& state,GuestMemory& memory,
        const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);
    const auto name=service.symbol;
    const auto finish=[&](std::uint64_t result,bool wide=false){
        call.return_to(state,result,wide);return std::optional<std::uint32_t>{};
    };
    const auto bits=static_cast<std::uint8_t>(x86_?32:64);
    if(!icu_number_)icu_number_=std::make_shared<IcuNumberState>();
    auto& number=*icu_number_;
    if(name=="ufieldpositer_open"){
        IcuStatus status(memory,call(0));
        if(U_FAILURE(status.value))return finish(0);
        const auto handle=allocate_heap(memory,x86_?12U:16U);
        if(handle){
            memory.store(GuestAddress{handle},64,0);
            memory.store(GuestAddress{handle+8},bits,0);
            number.positions.emplace(handle,0);
        }else status.value=U_MEMORY_ALLOCATION_ERROR;
        status.commit();return finish(handle);
    }
    if(name=="ufieldpositer_close"){
        if(const auto found=number.positions.find(call(0));found!=number.positions.end()){
            if(found->second)(void)free_heap(memory,found->second,state);
            number.positions.erase(found);
            (void)free_heap(memory,call(0),state);
        }
        return finish(0);
    }
    if(name=="ufieldpositer_next"){
        const auto handle=call(0);
        if(!number.positions.contains(handle))return finish(UINT32_MAX);
        const auto cursor=signed_argument(memory.load(GuestAddress{handle+4},32));
        const auto count=signed_argument(memory.load(GuestAddress{handle},32));
        const auto records=memory.load(GuestAddress{handle+8},bits);
        if(!records || cursor<0 || cursor>=count)return finish(UINT32_MAX);
        const auto address=records+static_cast<std::uint64_t>(cursor)*sizeof(icu_number::FieldPosition);
        const auto field=memory.load(GuestAddress{address},32);
        memory.store(GuestAddress{handle+4},32,static_cast<std::uint32_t>(cursor+1));
        if(call(1))memory.store(GuestAddress{call(1)},32,memory.load(GuestAddress{address+4},32));
        if(call(2))memory.store(GuestAddress{call(2)},32,memory.load(GuestAddress{address+8},32));
        return finish(field);
    }

    const auto found=number.values.find(call(0));
    auto* object=found==number.values.end()?nullptr:&found->second;
    const auto* native=object?object->get():nullptr;
    auto* writable=const_cast<UFormattable*>(native);
    if(name=="ufmt_close"){
        if(object && object->owned){
            clear_icu_formattable_references(state,memory,*object);
            number.values.erase(found);
            (void)free_heap(memory,call(0),state);
        }
        return finish(0);
    }
    if(name=="ufmt_isNumeric")return finish(native && ufmt_isNumeric(native));
    IcuStatus status(memory,call(service.arguments-1));
    const bool floating=name=="ufmt_getDate" || name=="ufmt_getDouble";
    const auto real=[&](double result){windows_floating_return(state,x86_,result,false);return finish(0);};
    if(U_FAILURE(status.value) && name=="ufmt_getDecNumChars")return finish(publish_icu_literal(memory,""));
    if(U_FAILURE(status.value))
        return floating?real(0.0):finish(name=="ufmt_getType"?UFMT_COUNT:0,name=="ufmt_getInt64");
    if(name=="ufmt_open"){
        IcuFormattableObject opened;
        opened.owned.reset(ufmt_open(&status.value));
        const auto handle=adopt_icu_formattable(state,memory,std::move(opened));
        if(!handle && U_SUCCESS(status.value))status.value=U_MEMORY_ALLOCATION_ERROR;
        status.commit();return finish(handle);
    }
    if(!native){
        status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();
        return floating?real(0.0):finish(name=="ufmt_getType"?UFMT_COUNT:0,name=="ufmt_getInt64");
    }
    if(floating){
        const auto value=name=="ufmt_getDate"?ufmt_getDate(native,&status.value):ufmt_getDouble(writable,&status.value);
        status.commit();return real(value);
    }
    if(name=="ufmt_getInt64"){
        const auto value=ufmt_getInt64(writable,&status.value);
        status.commit();return finish(static_cast<std::uint64_t>(value),true);
    }
    if(name=="ufmt_getType" || name=="ufmt_getLong" || name=="ufmt_getArrayLength"){
        std::int32_t value{};
        if(name=="ufmt_getType")value=ufmt_getType(native,&status.value);
        else if(name=="ufmt_getLong")value=ufmt_getLong(writable,&status.value);
        else value=ufmt_getArrayLength(native,&status.value);
        status.commit();return finish(static_cast<std::uint32_t>(value));
    }
    if(name=="ufmt_getObject"){
        const auto* value=ufmt_getObject(native,&status.value);
        std::uint64_t handle{};
        if(value && U_SUCCESS(status.value)){
            auto& entries=object->objects;
            if(const auto existing=entries.find(value);existing!=entries.end())handle=existing->second;
            else if((handle=allocate_heap(memory,1)))entries.emplace(value,handle);
            else status.value=U_MEMORY_ALLOCATION_ERROR;
        }
        status.commit();return finish(handle);
    }
    if(name=="ufmt_getArrayItemByIndex"){
        const auto* item=ufmt_getArrayItemByIndex(writable,signed_argument(call(1)),&status.value);
        std::uint64_t handle{};
        if(item && U_SUCCESS(status.value)){
            auto& entries=object->children;
            if(const auto previous=entries.find(item);previous!=entries.end())handle=previous->second;
            else{
                IcuFormattableObject borrowed;
                borrowed.owner=call(0);
                borrowed.borrowed=item;
                handle=adopt_icu_formattable(state,memory,std::move(borrowed));
                if(handle)entries.emplace(item,handle);
                else status.value=U_MEMORY_ALLOCATION_ERROR;
            }
        }
        status.commit();return finish(handle);
    }

    const bool wide=name=="ufmt_getUChars";
    std::int32_t length{};
    const auto* data=wide?static_cast<const void*>(ufmt_getUChars(writable,&length,&status.value)):
        static_cast<const void*>(ufmt_getDecNumChars(writable,&length,&status.value));
    std::uint64_t address{};
    if(data && U_SUCCESS(status.value)){
        const auto unit=wide?sizeof(UChar):1;
        const auto required=(static_cast<std::size_t>(length)+1)*unit;
        const unsigned index=wide?1U:0U;
        auto& buffer=object->text[index];
        auto& capacity=object->capacities[index];
        if(capacity<required){
            const auto next=allocate_heap(memory,required);
            if(next){
                if(const auto previous=std::exchange(buffer,next))(void)free_heap(memory,previous,state);
                capacity=required;
            }else status.value=U_MEMORY_ALLOCATION_ERROR;
        }
        if(U_SUCCESS(status.value)){
            memory.write(GuestAddress{buffer},{static_cast<const std::uint8_t*>(data),required-unit});
            memory.store(GuestAddress{buffer+required-unit},static_cast<std::uint8_t>(unit*8),0);
            if(call(1))memory.store(GuestAddress{call(1)},32,static_cast<std::uint32_t>(length));
            address=buffer;
        }
    }
    if(U_FAILURE(status.value) && !wide)address=publish_icu_literal(memory,"");
    status.commit();return finish(address);
}
}

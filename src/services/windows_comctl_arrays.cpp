// comctl32 動態陣列家族：DPA（指標陣列）與 DSA（固定大小項目陣列）。
// 結構放在客體 heap，且依 commctrl.h 巨集可直接讀取的版面：DPA{int cp; void** pp; HANDLE heap; int alloc; int grow}、
// DSA{int count; void* items; int alloc; int item_size; int grow}（DPA_GetPtrCount／DPA_FastGetPtr／DSA_GetItemCount 讀前兩欄）。
// 列舉、比較與銷毀回呼經協程呼叫客體函式；排序為穩定的合併排序。
#include "x64arm/windows_services.hpp"
#include "service_callback_task.hpp"
#include <algorithm>

namespace x64arm {
std::span<const WindowsServiceDescriptor> comctl_array_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"comctl32.dll","DPA_Create",1,0,0,0x1},{"comctl32.dll","DPA_CreateEx",2,0,0,0x1},{"comctl32.dll","DPA_Clone",2},{"comctl32.dll","DPA_Destroy",1},
        {"comctl32.dll","DPA_DestroyCallback",3},{"comctl32.dll","DPA_DeleteAllPtrs",1},{"comctl32.dll","DPA_DeletePtr",2,0,0,0x2},{"comctl32.dll","DPA_EnumCallback",3},
        {"comctl32.dll","DPA_GetPtr",2},{"comctl32.dll","DPA_GetPtrIndex",2},{"comctl32.dll","DPA_Grow",2,0,0,0x2},{"comctl32.dll","DPA_InsertPtr",3,0,0,0x2},
        {"comctl32.dll","DPA_SetPtr",3,0,0,0x2},{"comctl32.dll","DPA_Search",6,0,0,0x24},{"comctl32.dll","DPA_Sort",3},
        {"comctl32.dll","DSA_Create",2,0,0,0x3},{"comctl32.dll","DSA_Destroy",1},{"comctl32.dll","DSA_DestroyCallback",3},{"comctl32.dll","DSA_DeleteAllItems",1},
        {"comctl32.dll","DSA_DeleteItem",2,0,0,0x2},{"comctl32.dll","DSA_EnumCallback",3},{"comctl32.dll","DSA_GetItem",3,0,0,0x2},{"comctl32.dll","DSA_GetItemPtr",2,0,0,0x2},
        {"comctl32.dll","DSA_InsertItem",3,0,0,0x2},{"comctl32.dll","DSA_SetItem",3,0,0,0x2}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_comctl_arrays(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const auto name=service.symbol;const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const bool dpa=name.starts_with("DPA_");
    // 欄位位移：DPA {count 0, pp unit, heap 2unit, alloc 3unit, grow 3unit+4}；DSA {count 0, items unit, alloc 2unit, size 2unit+4, grow 2unit+8}。
    const unsigned items_at=unit,heap_at=2*unit,dpa_alloc=3*unit,dpa_grow=3*unit+4,dsa_alloc=2*unit,dsa_size=2*unit+4,dsa_grow=2*unit+8;
    const unsigned dpa_bytes=3*unit+8,dsa_bytes=2*unit+12;
    const auto count_of=[&](std::uint64_t array){return static_cast<std::int32_t>(memory.load(GuestAddress{array},32));};
    const auto items_of=[&](std::uint64_t array){return memory.load(GuestAddress{array+items_at},bits);};
    const auto slot=[&](std::uint64_t array,std::int32_t index){return items_of(array)+static_cast<std::uint64_t>(index)*unit;};
    if(name=="DPA_Sort" || name=="DPA_Search" || name.ends_with("EnumCallback") || name.ends_with("DestroyCallback"))
        return nested_continuation(state,memory,"comctl32 動態陣列",[this,service,unit,bits,dpa,items_at,dsa_size](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            // 協程會在回呼後續行，存取一律經由自己的 GuestMemory 參考。
            const auto count_of=[&m](std::uint64_t array){return static_cast<std::int32_t>(m.load(GuestAddress{array},32));};
            const auto items_of=[&m,items_at,bits](std::uint64_t array){return m.load(GuestAddress{array+items_at},bits);};
            const auto slot=[&](std::uint64_t array,std::int32_t index){return items_of(array)+static_cast<std::uint64_t>(index)*unit;};
            const WindowsCall call(s,m,service);const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
            const auto symbol=service.symbol;const auto array=call(0);if(!array)co_return finish(symbol=="DPA_Search"?UINT32_MAX:0U);
            const auto item=[&](std::int32_t index)->std::uint64_t{
                if(dpa)return m.load(GuestAddress{slot(array,index)},bits);
                return items_of(array)+static_cast<std::uint64_t>(index)*m.load(GuestAddress{array+dsa_size},32);
            };
            if(symbol.ends_with("EnumCallback") || symbol.ends_with("DestroyCallback")){
                // 依序呼叫 callback(項目, pData)，回 0 即停止；陣列在回呼中可能變動，每次重新讀取數量。
                const auto callback=call(1),data=call(2);
                if(callback)for(std::int32_t index=0;index<count_of(array);++index)
                    if(!static_cast<std::uint32_t>(co_await service_callback::call(callback,{item(index),data})))break;
                if(symbol.ends_with("DestroyCallback")){
                    if(const auto items=items_of(array))(void)free_heap(m,items,s);(void)free_heap(m,array,s);
                }
                co_return finish(0);
            }
            const auto compare=call(symbol=="DPA_Search"?3:1);if(!compare)co_return finish(symbol=="DPA_Search"?UINT32_MAX:0U);
            const auto parameter=call(symbol=="DPA_Search"?4:2);
            const auto cmp=[&](std::uint64_t a,std::uint64_t b)->service_callback::Task<std::int32_t>{
                co_return static_cast<std::int32_t>(static_cast<std::uint32_t>(co_await service_callback::call(compare,{a,b,parameter})));};
            const auto count=count_of(array);
            if(symbol=="DPA_Search"){
                // (pFind, iStart, pfnCompare, lParam, options)：DPAS_SORTED（1）二分搜尋，找不到時 DPAS_INSERTBEFORE（2）／INSERTAFTER（4）回插入位置。
                const auto find=call(1);const auto options=static_cast<std::uint32_t>(call(5));
                if(options&1U){
                    std::int32_t lo=0,hi=count;
                    while(lo<hi){const auto mid=lo+(hi-lo)/2;const auto result=co_await cmp(find,item(mid));
                        if(!result)co_return finish(static_cast<std::uint32_t>(mid));if(result<0)hi=mid;else lo=mid+1;}
                    co_return finish((options&6U)?static_cast<std::uint32_t>(lo):UINT32_MAX);
                }
                for(auto index=std::max<std::int32_t>(0,static_cast<std::int32_t>(call(2)));index<count;++index)
                    if(!(co_await cmp(find,item(index))))co_return finish(static_cast<std::uint32_t>(index));
                co_return finish(UINT32_MAX);
            }
            // DPA_Sort：穩定合併排序，結果寫回指標陣列。
            std::vector<std::uint64_t> values(static_cast<std::size_t>(std::max(count,0)));for(std::int32_t i=0;i<count;++i)values[static_cast<std::size_t>(i)]=item(i);
            std::vector<std::uint64_t> work(values.size());
            for(std::size_t width=1;width<values.size();width*=2){
                for(std::size_t left=0;left<values.size();left+=2*width){
                    const auto mid=std::min(left+width,values.size()),right=std::min(left+2*width,values.size());std::size_t a=left,b=mid,out=left;
                    while(a<mid && b<right){if((co_await cmp(values[b],values[a]))<0)work[out++]=values[b++];else work[out++]=values[a++];}
                    while(a<mid)work[out++]=values[a++];while(b<right)work[out++]=values[b++];
                }
                values.swap(work);
            }
            for(std::int32_t i=0;i<count;++i)m.store(GuestAddress{slot(array,i)},bits,values[static_cast<std::size_t>(i)]);
            co_return finish(1);
        });
    const WindowsCall call(state,memory,service);const auto finish=[&](std::uint64_t value){call.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto array=call(0);const auto index=static_cast<std::int32_t>(call(1));
    // 容量依 grow 的倍數擴充；新配置以 0 填滿，舊內容複製後釋放。
    const auto reserve=[&](std::uint64_t target,std::int32_t needed)->bool{
        const auto alloc_at=dpa?dpa_alloc:dsa_alloc,grow_at=dpa?dpa_grow:dsa_grow;const auto element=dpa?unit:static_cast<unsigned>(memory.load(GuestAddress{target+dsa_size},32));
        const auto capacity=static_cast<std::int32_t>(memory.load(GuestAddress{target+alloc_at},32));if(needed<=capacity)return true;
        const auto grow=std::max<std::int32_t>(1,static_cast<std::int32_t>(memory.load(GuestAddress{target+grow_at},32)));
        const auto fresh=((needed+grow-1)/grow)*grow;if(fresh>(1<<26))return false;
        const auto heap=dpa?memory.load(GuestAddress{target+heap_at},bits):0;
        const auto block=heap?allocate_heap(memory,static_cast<std::uint64_t>(fresh)*element,heap):allocate_heap(memory,static_cast<std::uint64_t>(fresh)*element);if(!block)return false;
        memory.fill(GuestAddress{block},0,static_cast<std::size_t>(fresh)*element);
        if(const auto old=items_of(target)){std::vector<std::uint8_t> bytes(static_cast<std::size_t>(capacity)*element);memory.read(GuestAddress{old},bytes);memory.write(GuestAddress{block},bytes);(void)free_heap(memory,old,state);}
        memory.store(GuestAddress{target+items_at},bits,block);memory.store(GuestAddress{target+alloc_at},32,static_cast<std::uint32_t>(fresh));return true;
    };
    const auto shift=[&](std::uint64_t from,std::uint64_t to,std::size_t bytes){if(!bytes)return;std::vector<std::uint8_t> data(bytes);memory.read(GuestAddress{from},data);memory.write(GuestAddress{to},data);};
    if(name=="DPA_Create" || name=="DPA_CreateEx" || name=="DSA_Create"){
        const bool table=name=="DSA_Create";const auto heap=name=="DPA_CreateEx"?call(1):0;
        const auto block=heap?allocate_heap(memory,dpa_bytes,heap):allocate_heap(memory,table?dsa_bytes:dpa_bytes);if(!block)return finish(0);
        memory.fill(GuestAddress{block},0,table?dsa_bytes:dpa_bytes);
        if(table){const auto size=static_cast<std::int32_t>(call(0)),grow=static_cast<std::int32_t>(call(1));if(size<=0){(void)free_heap(memory,block,state);return finish(0);}
            memory.store(GuestAddress{block+dsa_size},32,static_cast<std::uint32_t>(size));memory.store(GuestAddress{block+dsa_grow},32,static_cast<std::uint32_t>(grow>0?grow:8));}
        else {const auto grow=static_cast<std::int32_t>(call(0));memory.store(GuestAddress{block+heap_at},bits,heap);memory.store(GuestAddress{block+dpa_grow},32,static_cast<std::uint32_t>(grow>0?grow:8));}
        return finish(block);
    }
    if(!array)return finish(name=="DPA_GetPtrIndex" || name=="DPA_InsertPtr" || name=="DSA_InsertItem"?UINT32_MAX:name=="DPA_Destroy" || name=="DSA_Destroy"?1U:0U);
    const auto count=count_of(array);
    if(name=="DPA_Destroy" || name=="DSA_Destroy" || name=="DPA_DeleteAllPtrs" || name=="DSA_DeleteAllItems"){
        if(const auto items=items_of(array))(void)free_heap(memory,items,state);
        if(name.ends_with("Destroy")){(void)free_heap(memory,array,state);return finish(1);}
        memory.store(GuestAddress{array},32,0);memory.store(GuestAddress{array+items_at},bits,0);memory.store(GuestAddress{array+(dpa?dpa_alloc:dsa_alloc)},32,0);return finish(1);
    }
    if(name=="DPA_GetPtr"){const auto i=static_cast<std::int64_t>(x86_?static_cast<std::int32_t>(call(1)):static_cast<std::int64_t>(call(1)));
        return finish(i>=0 && i<count?memory.load(GuestAddress{slot(array,static_cast<std::int32_t>(i))},bits):0);}
    if(name=="DPA_GetPtrIndex"){for(std::int32_t i=0;i<count;++i)if(memory.load(GuestAddress{slot(array,i)},bits)==call(1))return finish(static_cast<std::uint32_t>(i));return finish(UINT32_MAX);}
    if(name=="DPA_Grow")return finish(reserve(array,index)?1U:0U);
    if(name=="DPA_Clone"){
        // 目的為 NULL 時建立新的 DPA（沿用來源的 grow 與 heap）；指標陣列整份複製。
        auto target=call(1);bool created=false;
        if(!target){const auto heap=memory.load(GuestAddress{array+heap_at},bits);target=heap?allocate_heap(memory,dpa_bytes,heap):allocate_heap(memory,dpa_bytes);if(!target)return finish(0);
            memory.fill(GuestAddress{target},0,dpa_bytes);memory.store(GuestAddress{target+heap_at},bits,heap);memory.store(GuestAddress{target+dpa_grow},32,memory.load(GuestAddress{array+dpa_grow},32));created=true;}
        if(!reserve(target,count)){if(created)(void)free_heap(memory,target,state);return finish(0);}
        shift(items_of(array),items_of(target),static_cast<std::size_t>(count)*unit);memory.store(GuestAddress{target},32,static_cast<std::uint32_t>(count));return finish(target);
    }
    if(name=="DPA_InsertPtr" || name=="DSA_InsertItem"){
        // 索引大於數量時附加到尾端（DPA_APPEND／DSA_APPEND）；負值為失敗。
        if(index<0)return finish(UINT32_MAX);const auto at=std::min(index,count);if(!reserve(array,count+1))return finish(UINT32_MAX);
        const auto element=dpa?unit:static_cast<unsigned>(memory.load(GuestAddress{array+dsa_size},32));const auto base=items_of(array);
        shift(base+static_cast<std::uint64_t>(at)*element,base+static_cast<std::uint64_t>(at+1)*element,static_cast<std::size_t>(count-at)*element);
        if(dpa)memory.store(GuestAddress{base+static_cast<std::uint64_t>(at)*unit},bits,call(2));
        else {if(!call(2))return finish(UINT32_MAX);shift(call(2),base+static_cast<std::uint64_t>(at)*element,element);}
        memory.store(GuestAddress{array},32,static_cast<std::uint32_t>(count+1));return finish(static_cast<std::uint32_t>(at));
    }
    if(name=="DPA_SetPtr" || name=="DSA_SetItem"){
        // 索引超出目前數量時擴充，其間的項目為 0／NULL。
        if(index<0 || (!dpa && !call(2)))return finish(0);if(index>=count){if(!reserve(array,index+1))return finish(0);memory.store(GuestAddress{array},32,static_cast<std::uint32_t>(index+1));}
        const auto element=dpa?unit:static_cast<unsigned>(memory.load(GuestAddress{array+dsa_size},32));const auto at=items_of(array)+static_cast<std::uint64_t>(index)*element;
        if(dpa)memory.store(GuestAddress{at},bits,call(2));else shift(call(2),at,element);return finish(1);
    }
    if(name=="DPA_DeletePtr" || name=="DSA_DeleteItem"){
        if(index<0 || index>=count)return finish(0);
        const auto element=dpa?unit:static_cast<unsigned>(memory.load(GuestAddress{array+dsa_size},32));const auto base=items_of(array);
        const auto removed=dpa?memory.load(GuestAddress{base+static_cast<std::uint64_t>(index)*unit},bits):1;
        shift(base+static_cast<std::uint64_t>(index+1)*element,base+static_cast<std::uint64_t>(index)*element,static_cast<std::size_t>(count-index-1)*element);
        memory.store(GuestAddress{array},32,static_cast<std::uint32_t>(count-1));return finish(removed);
    }
    if(name=="DSA_GetItemPtr" || name=="DSA_GetItem"){
        if(index<0 || index>=count)return finish(0);
        const auto element=static_cast<unsigned>(memory.load(GuestAddress{array+dsa_size},32));const auto at=items_of(array)+static_cast<std::uint64_t>(index)*element;
        if(name=="DSA_GetItemPtr")return finish(at);if(!call(2))return finish(0);shift(at,call(2),element);return finish(1);
    }
    return std::nullopt;
}
}

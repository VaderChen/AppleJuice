#include "x64arm/windows_services.hpp"
#include "ldap_api.hpp"
#include <algorithm>

namespace x64arm {
std::span<const WindowsServiceDescriptor> heap_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{{"kernel32.dll","HeapCompact",2,0,0,2},{"crt","_get_heap_handle",0},{"crt","_heapchk",0},{"crt","_heapmin",0},{"crt","_heapwalk",1}};return table;
}
bool WindowsServices::validate_heap(const GuestMemory& memory,std::uint64_t heap,std::uint64_t pointer) const{
    if(heap!=process_heap && !runtime_.private_heaps.contains(heap))return false;
    const auto owner=[&](std::uint64_t address){const auto found=runtime_.heap_owners.find(address);return found==runtime_.heap_owners.end()?process_heap:found->second;};
    if(pointer && (!allocations_.contains(pointer) || owner(pointer)!=heap))return false;
    std::uint64_t previous_end=0;
    for(const auto& [address,size]:allocations_){
        if(address<previous_end || size>UINT64_MAX-address)return false;
        previous_end=address+size;
        if(owner(address)!=heap || (pointer && pointer!=address))continue;
        if(!size || (address&15U) || (size&15U))return false;
        // 配置資訊位於宿主；客體頁仍須完整提交，不以 NULL 區塊直接返回成功。
        for(auto at=address;at<previous_end;){
            const auto region=memory.query(GuestAddress{at});
            if(!region || !region->size || region->size>UINT64_MAX-region->base.value)return false;
            const auto end=region->base.value+region->size;if(end<=at)return false;at=std::min(previous_end,end);
        }
        const auto free=free_heap_blocks_.upper_bound(address);
        if(free!=free_heap_blocks_.end() && free->first<previous_end)return false;
        if(free!=free_heap_blocks_.begin()){const auto prior=std::prev(free);if(prior->second>UINT64_MAX-prior->first || prior->first+prior->second>address)return false;}
    }
    return true;
}
std::optional<std::uint32_t> WindowsServices::invoke_heap(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(service.family=="crt"){
        const WindowsCall arg(state,memory,service);
        if(service.symbol=="_heapwalk"){
            const auto done=[&](std::int32_t status){arg.return_to(state,static_cast<std::uint32_t>(status));return std::optional<std::uint32_t>{};};
            if(!arg(0)){
                memory.store(GuestAddress{runtime_.errno_cell},32,22);
                if(const auto handler=current_invalid_parameter_handler()){
                    start_callbacks(state,memory,{{handler,{0,0,0,0,0}}},Continuation::Return,static_cast<std::uint32_t>(-6));return std::nullopt;
                }
                return 0xc0000417U;
            }
            const unsigned width=x86_?4:8;const std::uint8_t bits=x86_?32:64;
            memory.check(GuestAddress{arg(0)},width*2+4,Access::Read);
            memory.check(GuestAddress{arg(0)},width*2+4,Access::Write);
            const auto previous=memory.load(GuestAddress{arg(0)},bits);
            // 快速重用清單的區塊屬於空閒區間；走訪前歸還一般空閒結構，才能列為空閒項目。
            flush_heap_quick(memory);
            if(!validate_heap(memory,process_heap))return done(-4);
            // 依共用配置器的真實配置與仍提交的空閒區間走訪；不讀客體資料
            // 猜測 header，也不將私有 heap 或已解除提交的空洞列為存活物件。
            std::map<std::uint64_t,std::pair<std::uint64_t,bool>> entries;
            for(const auto& [address,size]:allocations_){
                const auto owner=runtime_.heap_owners.find(address);
                if(owner==runtime_.heap_owners.end() || owner->second==process_heap)entries.emplace(address,std::pair{size,true});
            }
            for(const auto& [base,size]:free_heap_blocks_){
                if(size>UINT64_MAX-base)return done(-4);const auto end=base+size;auto cursor=base;
                while(cursor<end){
                    auto mapping=memory.query(GuestAddress{cursor});if(!mapping)mapping=memory.next_mapping(GuestAddress{cursor});
                    if(!mapping || mapping->base.value>=end)break;
                    cursor=std::max(cursor,mapping->base.value);
                    const auto last=std::min({end,mapping->base.value+mapping->size,(cursor&~UINT64_C(4095))+4096});
                    if(last<=cursor)return done(-4);
                    if(runtime_.heap_pages.contains(cursor&~UINT64_C(4095))){
                        auto prior=entries.lower_bound(cursor);
                        if(prior!=entries.begin() && !std::prev(prior)->second.second &&
                           std::prev(prior)->first+std::prev(prior)->second.first==cursor)std::prev(prior)->second.first+=last-cursor;
                        else entries.emplace(cursor,std::pair{last-cursor,false});
                    }
                    cursor=last;
                }
            }
            // 客體配置區（x86 chunk 或 x64 連續保留區）的 process heap 區塊：已配置逐一列出，空閒段依 chunk 內相鄰合併。
            if(guest_heap_chunked_ || guest_heap_base_){
                for(const auto& [address,size]:guest_heap_allocated_blocks(memory))entries.emplace(address,std::pair{size,true});
                for(const auto& [address,size]:guest_heap_walk_free_)entries.emplace(address,std::pair{size,false});
            }
            if(previous && !entries.contains(previous))return done(-6);
            const auto next=previous?entries.upper_bound(previous):entries.begin();
            if(next==entries.end())return done(entries.empty()?-1:-5);
            memory.store(GuestAddress{arg(0)},bits,next->first);
            memory.store(GuestAddress{arg(0)+width},bits,next->second.first);
            memory.store(GuestAddress{arg(0)+width*2},32,next->second.second?1:0);
            return done(-2);
        }
        if(service.symbol=="_heapmin"){
            flush_heap_quick(memory);
            bool valid=validate_heap(memory,process_heap);
            try{if(valid)for(const auto& [base,size]:free_heap_blocks_){const auto end=base+size;auto cursor=base;
                while(cursor<end){auto mapping=memory.query(GuestAddress{cursor});if(!mapping)mapping=memory.next_mapping(GuestAddress{cursor});if(!mapping || mapping->base.value>=end)break;
                    cursor=std::max(cursor,mapping->base.value);const auto last=std::min({end,mapping->base.value+mapping->size,(cursor&~UINT64_C(4095))+4096});
                    if(runtime_.heap_pages.contains(cursor&~UINT64_C(4095)))memory.discard_contents(GuestAddress{cursor},static_cast<std::size_t>(last-cursor));cursor=last;}
            }}catch(const MemoryFault&){valid=false;}catch(const std::invalid_argument&){valid=false;}catch(const std::bad_alloc&){valid=false;}
            // free_heap 已立即解除完整空頁；此處回收共用邊界頁內的空閒 backing，保留存活物件。
            if(!valid)memory.store(GuestAddress{runtime_.errno_cell},32,40);arg.return_to(state,valid?0U:UINT32_MAX);return std::nullopt;
        }
        arg.return_to(state,service.symbol=="_get_heap_handle"?process_heap:static_cast<std::uint32_t>(validate_heap(memory,process_heap)?-2:-4));return std::nullopt;
    }
    const WindowsCall arg(state,memory,service);const auto heap=arg(0);const auto flags=static_cast<std::uint32_t>(arg(1));
    std::uint64_t largest=0;
    if(heap!=process_heap && !runtime_.private_heaps.contains(heap))last_error_=6;
    else if((flags&~1U) || (heap==process_heap && (flags&1U)))last_error_=87;
    else try {
        flush_heap_quick(memory,heap);
        // 同一個共用配置器會在 free 時合併相鄰空洞；此處查詢其真正仍提交的頁。
        std::set<std::uint64_t> pages;constexpr auto page=GuestMemory::guest_page_size;
        for(const auto& [address,size]:allocations_){const auto owner=runtime_.heap_owners.find(address);const auto owned_heap=owner==runtime_.heap_owners.end()?process_heap:owner->second;
            if(owned_heap!=heap)continue;const auto last=address+size;
            for(const auto edge:{address&~(page-1),(last-1)&~(page-1)})if(runtime_.heap_pages.contains(edge))pages.insert(edge);
        }
        std::vector<std::pair<std::uint64_t,std::uint64_t>> regions;
        for(const auto base:pages){const auto map=memory.query(GuestAddress{base});if(!map)continue;
            const auto end=std::min(base+page,map->base.value+map->size);if(!regions.empty() && regions.back().second==base)regions.back().second=end;else regions.emplace_back(base,end);
        }
        for(const auto& [begin,end]:regions){auto at=begin;auto object=allocations_.lower_bound(begin);
            if(object!=allocations_.begin()){const auto previous=std::prev(object);if(previous->first+previous->second+16>begin)object=previous;}
            for(;object!=allocations_.end() && object->first<end;++object){const auto first=object->first,last=std::min(end,first+object->second+16);
                if(first>at)largest=std::max(largest,(first-at)&~UINT64_C(15));at=std::max(at,last);
            }
            if(end>at)largest=std::max(largest,(end-at)&~UINT64_C(15));
        }
        // 沒有已提交空閒區塊亦是有效查詢；區分零結果與錯誤。
        if(!largest)last_error_=0;
    }catch(const std::bad_alloc&){largest=0;last_error_=8;}
    arg.return_to(state,largest);return std::nullopt;
}
bool WindowsServices::await_heap_service(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    // 沒有 HeapLock 紀錄時不可能等待；API 本體仍負責完整的框架與 heap 檢查。
    if(runtime_.heap_locks.empty())return false;
    const auto name=service.symbol;
    const bool explicit_heap=name=="HeapCompact" || name=="HeapLock" || name=="HeapDestroy" || name=="HeapAlloc" || name=="HeapReAlloc" || name=="HeapFree" || name=="HeapSize" || name=="HeapValidate" || name=="HeapWalk" || name=="HeapSetInformation" || name=="HeapQueryInformation";
    // 這些入口共用本引擎的 process heap，不能在另一執行緒持有 HeapLock 時穿越。
    static constexpr std::string_view process_allocators[]{
        "malloc","calloc","realloc","free","_aligned_malloc","_aligned_realloc","_aligned_free","_aligned_offset_malloc","_aligned_offset_realloc","_aligned_recalloc","_aligned_offset_recalloc","_aligned_msize","_strdup","_wcsdup","_heapchk","_heapmin","_heapwalk",
        "setlocale","_wsetlocale","localeconv","_Getdays","_Getmonths","_W_Getdays","_W_Getmonths","_Gettnames","_W_Gettnames",
        "_tempnam","_wtempnam","tmpnam","_wtmpnam","tmpnam_s","_wtmpnam_s",
        "_create_locale","_wcreate_locale","_get_current_locale","_free_locale","__free_locale","_configthreadlocale",
        "__x64arm_create_locale_legacy","__x64arm_wcreate_locale_legacy","__x64arm_current_locale_legacy",
        "__std_exception_copy","__std_exception_destroy","_dupenv_s","_wdupenv_s",
        "_putenv","_wputenv","_putenv_s","_wputenv_s",
        "RtlCreateProcessParameters","RtlCreateProcessParametersEx","RtlCreateProcessParametersWithTemplate","RtlDestroyProcessParameters",
        "NtCreateUserProcess","ZwCreateUserProcess",
        "__std_type_info_name","__std_type_info_destroy_list","??1type_info@@UEAA@XZ","??1type_info@@UAE@XZ",
        "LocalAlloc","LocalReAlloc","LocalFree","LocalSize","GlobalAlloc","GlobalReAlloc","GlobalFree","GlobalSize",
        "CopyImage","CreateDIBSection","CreateCompatibleBitmap","DeleteObject",
        "CertCreateCRLContext","CertFreeCRLContext","CertEnumCRLsInStore","CertDeleteCRLFromStore","CertAddCRLContextToStore","CertAddEncodedCRLToStore",
        "WinHttpGetDefaultProxyConfiguration","WinHttpGetIEProxyConfigForCurrentUser","WinHttpQueryOption",
        "SHGetMalloc","CoGetMalloc","IMalloc.Alloc","IMalloc.Realloc","IMalloc.Free","IMalloc.HeapMinimize",
        "CoTaskMemAlloc","CoTaskMemRealloc","CoTaskMemFree","SysAllocString","SysAllocStringLen","SysAllocStringByteLen","SysReAllocString","SysReAllocStringLen","SysFreeString"
        ,"ReadClassStm","WriteClassStm","OleSaveToStream","OleLoadFromStream"
        ,"CreateBindCtx","GetRunningObjectTable"
    };
    const bool stl_family=service.family=="msvcp100.dll" || service.family=="msvcp140.dll" || service.family=="internal-msvcp100" || service.family=="internal-msvcp140";
    const bool stl_allocator=stl_family && (name.starts_with("??0") || name.starts_with("??1") || name.starts_with("??6") || name.starts_with("?_Getcat@") || name.starts_with("?_New_Locimp@") || name.starts_with("?_Locimp_Addfac@") || name.starts_with("?__ExceptionPtr") || name.find("FacetDelete")!=name.npos || name.find("IosDelete")!=name.npos || name.find("StreamDelete")!=name.npos || name.find("ExceptionControl")!=name.npos || name.find("ErrorCategoryMessage")!=name.npos || name.starts_with("?_Throw_C_error@") || name.starts_with("?_XGetLastError@"));
    const bool exception_allocator=service.family=="x64arm.msvc" && (name=="ExceptionCopy" || name=="ExceptionDestroy" || name=="ExceptionDelete");
    const bool bind_allocator=service.family.starts_with("internal-com-bind") || service.family.starts_with("internal-com-rot");
    if(!explicit_heap && !stl_allocator && !exception_allocator && !bind_allocator && !(service.family=="wldap32.dll" && ldap_api::uses_heap(name)) && std::find(std::begin(process_allocators),std::end(process_allocators),name)==std::end(process_allocators))return false;
    const WindowsCall call(state,memory,service);const auto heap=explicit_heap?call(0):process_heap;
    if(name=="__std_type_info_name" && memory.load(GuestAddress{call(0)},x86_?32:64))return false;
    if(runtime_.unserialized_heaps.contains(heap))return false;
    const bool per_call_flags=name=="HeapCompact" || name=="HeapAlloc" || name=="HeapReAlloc" || name=="HeapFree" || name=="HeapSize" || name=="HeapValidate";
    if(per_call_flags && (call(1)&1))return false;
    const auto lock=runtime_.heap_locks.find(heap);
    if(lock==runtime_.heap_locks.end() || !lock->second.owner || lock->second.owner==threads_.current)return false;
    auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::HeapRetry;thread.wait.handles={heap};
    schedule(state,memory);return true;
}
}

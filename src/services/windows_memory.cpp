#include "x64arm/windows_services.hpp"
#include "x64arm/windows_security_attributes.hpp"
#include "x64arm/security_descriptor.hpp"
#include <algorithm>
#include <optional>
#include <atomic>
#include <filesystem>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif
#include "x64arm/system_performance.hpp"
namespace x64arm {
namespace {
constexpr std::uint64_t page=4096, granularity=65536;
std::uint64_t round_page(std::uint64_t value) {return (value+page-1)&~(page-1);}
std::optional<Permissions> permissions(std::uint32_t mode) {
    if((mode&~0x1ffU) || ((mode&0x100U) && (mode&0xffU)==1))return std::nullopt;
    mode&=~0x100U;
    switch (mode) {case 1:return 0;case 2:return 1;case 4:return 3;case 16:return 4;case 32:return 5;case 64:return 7;default:return std::nullopt;}
}
std::uint32_t protection(Permissions value) {return value==7?64U:value==3?4U:value==5?32U:value==4?16U:value?2U:1U;}
std::uint32_t residency_error(const std::error_code& error){
    if(error==std::errc::permission_denied || error==std::errc::operation_not_permitted)return 5;
    if(error==std::errc::not_enough_memory || error==std::errc::resource_unavailable_try_again)return 1453;
    if(error==std::errc::invalid_argument)return 87;
    if(error==std::errc::operation_not_supported || error==std::errc::function_not_supported)return 50;
    return 998;
}
}
VirtualMemoryInformation WindowsServices::virtual_memory_information(const GuestMemory& memory,std::uint64_t raw) const {
    const auto address=raw,address_limit=user_address_limit();const auto& regions=runtime_.virtual_regions;
    auto found=regions.upper_bound(address);if(found!=regions.begin()){--found;if(address-found->first>=found->second.size)found=regions.end();}else found=regions.end();
    const auto mapped=memory.query(GuestAddress{address});
    const auto view=mapping_view_info(address);
    const bool section_image=view && (view->protection&0x1000000U);
    std::uint64_t base=address,allocation=0,end=address_limit;std::uint32_t initial=0,mode=0,allocation_state=0x10000,type=0;
    if (found!=regions.end()) {
        allocation=found->first;initial=found->second.protection;end=allocation+found->second.size;allocation_state=0x2000;type=0x20000;
    } else if (const auto next=regions.lower_bound(address);next!=regions.end()) end=std::min(end,next->first);
    if(!allocation){
        const auto stack=[&](std::uint64_t start,std::uint64_t size){if(size && address>=start && address-start<size){allocation=start;initial=4;end=std::min(end,start+size);allocation_state=0x2000;type=0x20000;}};
        for(const auto& [id,thread]:threads_.items){(void)id;stack(thread.stack_base,thread.stack_size);}
        for(const auto& [handle,fiber]:threads_.fibers){(void)handle;stack(fiber.stack_base,fiber.stack_size);}
    }
    if(section_image){allocation=view->base;initial=view->protection&0xffU;type=0x1000000U;allocation_state=0x2000;end=std::min(end,view->base+view->size);}
    if (mapped) {
        // 查詢從包含 lpAddress 的頁面開始，大小只涵蓋同狀態／權限的連續區間。
        base=address;end=std::min(end,mapped->base.value+mapped->size);mode=protection(mapped->permissions);allocation_state=0x1000;
        if (!allocation) {
            const auto* image=module_image(address,true);allocation=image?image->base:mapped->base.value;
            initial=mode;type=image?0x1000000U:0x20000U;
        }
        if(view){
            allocation=view->base;initial=view->protection&0xffU;
            if(section_image){if((mapped->permissions&permission(Access::Write)) && !shared_mapping_page(address) && memory.is_copy_on_write(GuestAddress{address}))mode=(mapped->permissions&permission(Access::Execute))?128U:8U;}
            else{if((initial==8 || initial==128) && memory.is_copy_on_write(GuestAddress{address}))mode=initial;type=0x40000U;}
            end=std::min(end,view->base+view->size);
        }
        const auto page_address=address&~(page-1);const auto override=runtime_.virtual_page_protections.find(page_address);
        if(override!=runtime_.virtual_page_protections.end()){
            mode=override->second.mode;if((mode==8 || mode==128) && !memory.is_copy_on_write(GuestAddress{address}))mode>>=1;initial=override->second.initial;auto cursor=page_address+page;
            while(cursor<end){const auto next=runtime_.virtual_page_protections.find(cursor);if(next==runtime_.virtual_page_protections.end() || next->second!=override->second)break;cursor+=page;}end=std::min(end,cursor);
        }else if(const auto next=runtime_.virtual_page_protections.upper_bound(page_address);next!=runtime_.virtual_page_protections.end())end=std::min(end,next->first);
        while (!section_image && end<address_limit) {
            const auto next=memory.query(GuestAddress{end});
            if (!next || next->permissions!=mapped->permissions || next->base.value!=end || found==regions.end() || end>=found->first+found->second.size) break;
            end=std::min(found->first+found->second.size,next->base.value+next->size);
        }
    } else if (const auto next=memory.next_mapping(GuestAddress{address})) end=std::min(end,next->base.value);
    return {base,allocation,end-base,initial,allocation_state,mode,type};
}
bool WindowsServices::protection_allocation_range(std::uint64_t begin,std::uint64_t end) const {
    const auto& regions=runtime_.virtual_regions;auto found=regions.upper_bound(begin);
    if(found!=regions.begin()){
        const auto before=std::prev(found);
        if(begin-before->first<before->second.size)return end<=before->first+before->second.size;
    }
    // 不能從另一個映射跨入獨立 VirtualAlloc 的 reservation。
    if(found!=regions.end() && found->first<end)return false;
    if(const auto view=mapping_view_info(begin))return end<=view->base+((view->size+4095)&~UINT64_C(4095));
    if(const auto* image=module_image(begin,true))return end<=image->base+((image->size+4095)&~UINT64_C(4095));
    return true;
}
std::uint32_t WindowsServices::local_memory_operation(GuestMemory& memory,std::uint32_t operation,std::array<std::uint64_t,5>& values,int descriptor,std::uint64_t frame,std::uint64_t output,const VirtualAddressRequirements* requirements){
    VirtualAddressRequirements transferred_requirements;
    if(operation==9){
#if !defined(_WIN32)
        std::array<std::uint8_t,24> bytes{};ssize_t count{};do{count=::pread(descriptor,bytes.data(),bytes.size(),0);}while(count<0 && errno==EINTR);
        if(count!=static_cast<ssize_t>(bytes.size()))return 13;
        const auto get=[&](unsigned at){std::uint64_t value=0;for(unsigned i=0;i<8;++i)value|=static_cast<std::uint64_t>(bytes[at+i])<<(i*8);return value;};
        transferred_requirements={get(0),get(8),get(16)};requirements=&transferred_requirements;operation=2;
#else
        return 50;
#endif
    }
    const auto arg=[&](unsigned index){return values.at(index);};std::uint64_t result=0;std::uint32_t error=0;
    const auto fail=[&](std::uint32_t code){
        error=code;result=0;
        if(operation==2 && std::getenv("X64ARM_TRACE_VIRTUAL_FAILURES")){
            std::cerr<<"[virtual_failure] pid="<<process_id()<<" error="<<code
                <<" address=0x"<<std::hex<<arg(0)<<" size=0x"<<arg(1)<<" type=0x"<<arg(2)<<" protect=0x"<<arg(3)
                <<std::dec<<" remaining="<<memory.remaining_capacity()<<" limit="<<memory.limit()
                <<" reservations="<<runtime_.virtual_regions.size()<<'\n';
        }
    };const auto address_limit=user_address_limit();
    auto& regions=runtime_.virtual_regions;
    const auto region_at=[&](std::uint64_t address){auto found=regions.upper_bound(address);if(found==regions.begin())return regions.end();--found;return address-found->first<found->second.size?found:regions.end();};
    const auto offered_overlap=[&](std::uint64_t begin,std::uint64_t end){const auto found=runtime_.offered_memory_pages.lower_bound(begin&~(page-1));return found!=runtime_.offered_memory_pages.end() && found->first<end;};
    const auto executable_range=[&](std::uint64_t begin,std::uint64_t size){
        if(!size || size>UINT64_MAX-begin)return false;const auto end=begin+size;
        auto mapping=memory.query(GuestAddress{begin});if(!mapping)mapping=memory.next_mapping(GuestAddress{begin});
        for(;mapping && mapping->base.value<end;mapping=memory.next_mapping(GuestAddress{mapping->base.value+mapping->size}))if(mapping->permissions&permission(Access::Execute))return true;
        return false;
    };
    try{
    if(operation==4){
        const auto at = arg(0) & ~UINT64_C(4095), count = arg(1); const auto mode = static_cast<std::uint32_t>(arg(2));
        const auto mapping = memory.query(GuestAddress{arg(0)});
        if (!count || count > memory.limit() || arg(0) > UINT64_MAX - count - 4095 || !mapping) fail(487);
        else if ((mode&~0x1ffU) || ((mode&0x100U) && (mode&0xffU)==1) ||
                 ((mode&0xffU)!=1 && (mode&0xffU)!=2 && (mode&0xffU)!=4 && (mode&0xffU)!=8 && (mode&0xffU)!=16 && (mode&0xffU)!=32 && (mode&0xffU)!=64 && (mode&0xffU)!=128)) fail(87);
        else {

            const auto end = (arg(0) + count + 4095) & ~UINT64_C(4095);
            const auto previous=virtual_memory_information(memory,arg(0));const auto old=previous.protection;
            const auto basic=mode&0xffU;const bool copy=basic==8 || basic==128;
            const Permissions permissions = basic == 1 ? 0 : basic == 2 ? 1 : basic == 4 || basic==8 ? 3 : basic == 16 ? 4 : basic == 32 ? 5 : 7;
            const auto actual=static_cast<Permissions>((mode&0x100U)?0:permissions);
            bool shared_image_code=false;
            if(previous.type==0x1000000U && (permissions&permission(Access::Execute)))
                for(auto cursor=at;cursor<end;cursor+=page)if(shared_mapping_page(cursor)){shared_image_code=true;break;}
            // 不可撤銷目前 API return frame 或 oldProtect out parameter 的存取權限。
            if(!protection_allocation_range(at,end))fail(487);
            else if(copy && previous.type!=0x1000000U && previous.type!=0x40000U)fail(87);
            else if(offered_overlap(at,end))fail(487);
            else if(shared_image_code)fail(50); // 跨程序共享程式碼的失效協定尚未提供。
            else if(dynamic_code_blocked() && ((permissions&permission(Access::Execute)) || ((permissions&permission(Access::Write)) && executable_range(at,end-at))))fail(1655);
            else if (!(actual & permission(Access::Write)) && ((output && at < output + 4 && output < end) || (at <= frame && frame < end))) fail(87);
            else {
                try {
                    auto protections=runtime_.virtual_page_protections;
                    for(auto cursor=at;cursor<end;cursor+=page){const auto info=virtual_memory_information(memory,cursor);if(copy && (info.allocation!=previous.allocation || (info.type!=0x1000000U && info.type!=0x40000U)))throw std::invalid_argument("私人寫入必須在同一映射內");protections.insert_or_assign(cursor,RuntimeState::PageProtection{mode,info.initial});}
                    if(copy)memory.make_copy_on_write(GuestAddress{at},static_cast<std::size_t>(end-at),actual);else memory.protect(GuestAddress{at}, static_cast<std::size_t>(end - at), actual);
                    runtime_.virtual_page_protections=std::move(protections);result = 1;
                }
                catch (const MemoryFault&) { fail(487); } catch(const std::invalid_argument&){fail(5);}
                if(result)values[4]=old;
            }
        }

    } else if(operation==2){
        const auto requested=arg(0),size=arg(1);const auto type=static_cast<std::uint32_t>(arg(2)),mode=static_cast<std::uint32_t>(arg(3));
        if(const auto view=mapping_view_info(requested);view && (view->protection&0x1000000U))return 487;
        const bool replace=bool(type&0x4000),placeholder=bool(type&0x40000);
        const bool reserve=(bool(type&0x2000) || !requested) && !replace;
        const auto access=permissions(mode);
        const auto align=requirements && requirements->alignment?std::max(granularity,requirements->alignment):granularity;
        const auto lowest=requirements?std::max(granularity,requirements->lowest):granularity;
        const auto highest=requirements && requirements->highest?std::min(address_limit-1,requirements->highest):address_limit-1;
        const auto align_up=[&](std::uint64_t value){return value>UINT64_MAX-(align-1)?UINT64_MAX:(value+align-1)&~(align-1);};
        if (!size || size>=address_limit || requested>=address_limit || size>address_limit-requested) fail(87);
        else if((requirements && requirements->alignment && (requirements->alignment&(requirements->alignment-1))) ||
                (align&(align-1)) || lowest>highest || size>highest-lowest+1 || (requirements && requested && (requirements->lowest || requirements->highest || requirements->alignment)))fail(87);
        else if (type==0x80000U || type==0x1000000U) {
            const auto begin=requested&~(page-1),end=round_page(requested+size);
            const auto found=region_at(begin);
            if(!access)fail(87);
            else if(!requested || found==regions.end() || end>found->first+found->second.size || offered_overlap(begin,end))fail(487);
            else {
                bool committed=true;
                for(auto cursor=begin;cursor<end;){const auto mapping=memory.query(GuestAddress{cursor});
                    if(!mapping){committed=false;break;}cursor=std::min(end,mapping->base.value+mapping->size);}
                // 保留原先 RESET 的內容保留策略，並追蹤後續明確丟棄的完整性。
                if(!committed)fail(487);
                else try{
                    auto& resets=runtime_.reset_memory_pages;bool intact=true;
                    if(type==0x80000U)for(auto cursor=begin;cursor<end;cursor+=page)resets.insert_or_assign(cursor,false);
                    else {
                        for(auto cursor=begin;cursor<end;cursor+=page){const auto found_reset=resets.find(cursor);
                            if(found_reset==resets.end() || found_reset->second){intact=false;break;}}
                        resets.erase(resets.lower_bound(begin),resets.lower_bound(end));
                    }
                    if(intact)result=begin;else fail(487);
                }catch(const std::bad_alloc&){fail(8);}
            }
        }
        else if ((type&~0x20547000U) || !(type&0x3000) || !access ||
                 (placeholder && (!(type&0x2000) || (type&0x1000) || replace || mode!=1)) ||
                 (replace && (!requested || placeholder)) ||
                 ((type&0x20400000U) && (type&0x20400000U)!=0x20400000U) ||
                 ((type&0x20400000U) && ((size&65535U) || (requested&65535U)))) fail(87);
        else if(dynamic_code_blocked() && ((*access&permission(Access::Execute)) || ((type&0x1000) && (*access&permission(Access::Write)) && executable_range(requested&~(page-1),round_page(requested+size)-(requested&~(page-1))))))fail(1655);
        else {
            auto begin=requested&~(reserve?granularity-1:page-1);
            const auto span=requested?round_page(requested+size)-begin:round_page(size);
            if (!requested) {
                const bool descending=bool(type&0x100000);
                begin=descending?(highest+1-span)&~(align-1):align_up(requirements?lowest:runtime_.next_virtual);
                for (;;) {
                    bool conflict=false;std::uint64_t low=begin,high=begin;
                    // 保留區互不重疊：只有起點前一個與起點落在候選範圍內者可能衝突，以有序查詢取代全表掃描。
                    auto overlap=regions.upper_bound(begin);
                    if (overlap!=regions.begin()) --overlap;
                    for (;overlap!=regions.end() && overlap->first<begin+span;++overlap) if (begin<overlap->first+overlap->second.size) {
                        conflict=true;low=std::min(low,overlap->first);high=std::max(high,overlap->first+overlap->second.size);
                    }
                    if (const auto mapping=memory.query(GuestAddress{begin})) {
                        conflict=true;low=std::min(low,mapping->base.value);high=std::max(high,mapping->base.value+mapping->size);
                    }
                    if (const auto mapping=memory.next_mapping(GuestAddress{begin});mapping && mapping->base.value<begin+span) {
                        conflict=true;low=std::min(low,mapping->base.value);high=std::max(high,mapping->base.value+mapping->size);
                    }
                    if (!conflict) break;
                    if (descending) {if (low<span || ((low-span)&~(align-1))<lowest) {begin=0;break;}begin=(low-span)&~(align-1);}
                    else {begin=align_up(high);if (begin>highest || span>highest+1-begin) {begin=0;break;}}
                }
            }
            auto found=region_at(begin);
            if (begin<lowest || begin>highest || span>highest+1-begin) fail(487);
            else if(offered_overlap(begin,begin+span))fail(487);
            else if (reserve && (!memory.range_available(GuestAddress{begin},span) || found!=regions.end() ||
                (regions.lower_bound(begin)!=regions.end() && regions.lower_bound(begin)->first<begin+span))) fail(487);
            else if (!reserve && (found==regions.end() || span>found->first+found->second.size-begin)) fail(487);
            else if (replace && (found==regions.end() || !found->second.placeholder || found->first!=requested || found->second.size!=size)) fail(487);
            else if (!replace && !reserve && found->second.placeholder)fail(487);
            else if (reserve && regions.size()>=65536) fail(8);
            else {
                const auto actual=static_cast<Permissions>((mode&0x100U)?0:*access);
                std::vector<std::pair<std::uint64_t,std::size_t>> commits;std::uint64_t needed=0;bool reprotect=false;
                if (type&0x1000) for (auto cursor=begin;cursor<begin+span;) {
                    if (const auto existing=memory.query(GuestAddress{cursor})) {reprotect|=existing->permissions!=actual;cursor=std::min(begin+span,existing->base.value+existing->size);continue;}
                    auto end=begin+span;
                    if (const auto following=memory.next_mapping(GuestAddress{cursor})) end=std::min(end,following->base.value);
                    commits.emplace_back(cursor,static_cast<std::size_t>(end-cursor));needed+=end-cursor;cursor=end;
                }
                if (needed>memory.remaining_capacity()) fail(8);
                else {
                    std::size_t committed=0;bool reserved_recorded=false;
                    const auto rollback=[&]{
                        for(std::size_t i=0;i<committed;++i)memory.unmap(GuestAddress{commits[i].first},commits[i].second);
                        if(reserve){regions.erase(begin);if(reserved_recorded)memory.release_virtual_range(GuestAddress{begin},span);}
                    };
                    try {
                        auto protections=runtime_.virtual_page_protections;
                        if(type&0x1000)for(auto cursor=begin;cursor<begin+span;cursor+=page){
                            if(mode&0x100U)protections.insert_or_assign(cursor,RuntimeState::PageProtection{mode,reserve || replace?mode:found->second.protection});else protections.erase(cursor);}
                        if (reserve){memory.reserve_virtual_range(GuestAddress{begin},span);reserved_recorded=true;regions.emplace(begin,VirtualRegion{span,mode,placeholder,false});}
                        for (const auto& [base,count] : commits) {memory.map(GuestAddress{base},count,actual);++committed;}
                        // 重新提交保留既有內容，但必須套用本次 flProtect。
                        if(reprotect)memory.protect(GuestAddress{begin},static_cast<std::size_t>(span),actual);
                        if(replace){found->second.placeholder=false;found->second.replaced_placeholder=true;found->second.protection=mode;}
                        runtime_.virtual_page_protections=std::move(protections);
                        result=begin;
                        if (!requested && !(type&0x100000)) runtime_.next_virtual=(begin+span+granularity-1)&~(granularity-1);
                    } catch (const std::bad_alloc&) {
                        rollback();fail(8);
                    } catch(const std::invalid_argument&) {
                        rollback();fail(487);
                    } catch(const MemoryFault&) {
                        rollback();fail(487);
                    }
                }
            }
        }
    } else if(operation==3){
        const auto address=arg(0),size=arg(1);const auto type=static_cast<std::uint32_t>(arg(2));auto found=region_at(address);
        if(mapping_view_info(address))return 487; // section view 只能由 Unmap 流程釋放，保留其物件生命週期。
        if(type==0x8001U || type==0x8002U){
            if(found==regions.end() || (address&4095U) || (size&4095U) || size>address_limit-address)fail(87);
            else if(type==0x8001U){
                const auto end=address+size;auto cursor=address;unsigned count=0;auto item=regions.lower_bound(address);
                for(;item!=regions.end() && item->first==cursor && cursor<end;++item){if(!item->second.placeholder || item->second.size>end-cursor)break;cursor+=item->second.size;++count;}
                if(!size || cursor!=end || count<2)fail(87);
                else {auto changed=regions;changed.erase(changed.lower_bound(address),changed.lower_bound(end));changed.emplace(address,VirtualRegion{size,1,true,false});regions=std::move(changed);result=1;}
            }else if(found->second.placeholder){
                const auto first=found->first,last=first+found->second.size,end=address+size;
                if(!size || end>last || (address==first && end==last))fail(87);
                else {auto changed=regions;changed.erase(first);if(address>first)changed.emplace(first,VirtualRegion{address-first,1,true,false});changed.emplace(address,VirtualRegion{size,1,true,false});if(end<last)changed.emplace(end,VirtualRegion{last-end,1,true,false});regions=std::move(changed);result=1;}
            }else if(!found->second.replaced_placeholder || address!=found->first || (size && size!=found->second.size))fail(87);
            else if(frame>=address && frame-address<found->second.size)fail(87);
            else {const auto end=address+found->second.size;memory.discard(GuestAddress{address},static_cast<std::size_t>(found->second.size));
                runtime_.virtual_page_protections.erase(runtime_.virtual_page_protections.lower_bound(address),runtime_.virtual_page_protections.lower_bound(end));
                runtime_.offered_memory_pages.erase(runtime_.offered_memory_pages.lower_bound(address),runtime_.offered_memory_pages.lower_bound(end));
                runtime_.reset_memory_pages.erase(runtime_.reset_memory_pages.lower_bound(address),runtime_.reset_memory_pages.lower_bound(end));
                found->second={found->second.size,1,true,false};result=1;}
        }
        else if (type!=0x4000 && type!=0x8000) fail(87);
        else if (found==regions.end()) fail(487);
        else if(type==0x4000 && found->second.placeholder)fail(487);
        else if (type==0x8000 && (size || address!=found->first)) fail(87);
        else {
            auto begin=address&~(page-1);auto end=begin;
            if (type==0x8000 || (!size && address==found->first)) {begin=found->first;end=begin+found->second.size;}
            else if (size && size<address_limit && address<=address_limit-size) end=round_page(address+size);
            if (end<=begin || end>found->first+found->second.size) fail(487);
            else if (begin<=frame && frame<end) fail(87);
            else {
                memory.discard(GuestAddress{begin},static_cast<std::size_t>(end-begin));
                runtime_.virtual_page_protections.erase(runtime_.virtual_page_protections.lower_bound(begin),runtime_.virtual_page_protections.lower_bound(end));
                runtime_.offered_memory_pages.erase(runtime_.offered_memory_pages.lower_bound(begin),runtime_.offered_memory_pages.lower_bound(end));
                runtime_.reset_memory_pages.erase(runtime_.reset_memory_pages.lower_bound(begin),runtime_.reset_memory_pages.lower_bound(end));
                if (type==0x8000){memory.release_virtual_range(GuestAddress{begin},end-begin);regions.erase(found);}values[0]=begin;values[1]=end-begin;result=1;
            }
        }
    }else if(operation==5){
        const auto address=values[0]&~(page-1);if(address>=address_limit)return 87;
        const auto info=virtual_memory_information(memory,address);
        values={info.base,info.allocation,info.size,static_cast<std::uint64_t>(info.initial)|(static_cast<std::uint64_t>(info.state)<<32),static_cast<std::uint64_t>(info.protection)|(static_cast<std::uint64_t>(info.type)<<32)};return 0;
    }else if(operation==6){
        if(values[0] && (values[0]>=address_limit || values[1]>address_limit-values[0]))return 87;
        if(values[0] && values[1] && values[1]<=UINT64_C(64)*1024*1024)memory.flush_instruction_cache(GuestAddress{values[0]},static_cast<std::size_t>(values[1]));
        else memory.flush_instruction_cache();return 0;
    }else if(operation==7){
        if(values[2]&~UINT64_C(3))return 87;
        const auto view=mapping_view_info(values[0]);if(!view)return 487;
        if(values[1] && view->base!=values[0])return 487;
        if(frame && view->base<=frame && frame-view->base<view->size)return 87;
        const auto saved=last_error_;const bool result_unmap=unmap_file_view(memory,view->base,false,static_cast<std::uint32_t>(values[2]));const auto error_unmap=result_unmap?0U:last_error_;last_error_=saved;return error_unmap;
    }else if(operation<=1){
#if !defined(_WIN32)
        const auto address=values[0],size=values[1];values[4]=0;
        if(size>SIZE_MAX || size>memory.limit() || address>=address_limit || size>address_limit-address)return 299;
        if(operation==1 && dynamic_code_blocked() && executable_range(address,size))return 1655;
        memory.check(GuestAddress{address},static_cast<std::size_t>(size),operation?Access::Write:Access::Read);
        std::array<std::uint8_t,65536> bytes{};
        for(std::uint64_t offset=0;offset<size;){const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(),size-offset));auto part=std::span(bytes).first(count);
            if(operation==0)memory.read(GuestAddress{address+offset},part);
            std::size_t done=0;
            while(done<count){const auto transferred=operation ? ::pread(descriptor,bytes.data()+done,count-done,static_cast<off_t>(offset+done)) : ::pwrite(descriptor,bytes.data()+done,count-done,static_cast<off_t>(offset+done));
                if(transferred<0 && errno==EINTR)continue;if(transferred<=0)return 299;done+=static_cast<std::size_t>(transferred);}
            if(operation==1)memory.write(GuestAddress{address+offset},part);offset+=count;values[4]=offset;
        }
        return 0;
#else
        (void)descriptor;return 50;
#endif
    }else return 87;
    }catch(const MemoryFault&){return operation<=1?299U:487U;}catch(const std::bad_alloc&){return 8;}catch(const std::invalid_argument&){return 87;}
    if(error)return error;
    if(operation==2){const auto requested=values[0];values[0]=result;values[1]=requested?round_page(requested+values[1])-result:round_page(values[1]);}
    else if(operation==4){const auto end=round_page(values[0]+values[1]);values[0]&=~(page-1);values[1]=end-values[0];}
    return result?0U:487U;
}
std::span<const WindowsServiceDescriptor> working_set_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"kernel32.dll","K32EmptyWorkingSet",1},{"psapi.dll","EmptyWorkingSet",1},
        {"kernel32.dll","VirtualLock",2},{"kernel32.dll","VirtualUnlock",2},
        {"kernel32.dll","GetProcessWorkingSetSize",3},{"kernel32.dll","SetProcessWorkingSetSize",3}
    };return table;
}
std::uint32_t WindowsServices::process_memory_copy(GuestMemory& memory,std::uint64_t handle,std::uint64_t address,std::uint64_t buffer,std::uint64_t size,bool writing,std::uint64_t& copied,bool native){
    copied=0;std::uint32_t pid{};std::uint64_t birth{};
    if(native && !size)return 0;
    const auto rights=writing?(native?0x20U:0x28U):0x10U;auto error=process_memory_target(memory,handle,rights,pid,birth);
    // macOS 的讀寫保留有界 stream、探查與逐封包回覆驗證；共用 target 檢查仍先驗證權限及退出狀態。
    // 其他平台沿用 descriptor 通道，不能把已驗證失敗的回覆改送另一通道。
    const bool debug_transport=(!error && pid!=process_id() && debug_memory_) ||
        (error==87 && !current_process_handle(handle) && !process_remote_target(handle,rights,pid,birth));
    if(error && !debug_transport)return error;
    if(size>SIZE_MAX || size>memory.limit())return 299;
    try{
        memory.check(GuestAddress{buffer},static_cast<std::size_t>(size),writing?Access::Read:Access::Write);
        if(pid==process_id()){
            const auto source=writing?buffer:address,destination=writing?address:buffer;
            if(writing && dynamic_code_blocked() && size){
                if(size>UINT64_MAX-destination)return 299;const auto end=destination+size;
                auto mapping=memory.query(GuestAddress{destination});if(!mapping)mapping=memory.next_mapping(GuestAddress{destination});
                for(;mapping && mapping->base.value<end;mapping=memory.next_mapping(GuestAddress{mapping->base.value+mapping->size}))if(mapping->permissions&permission(Access::Execute))return 1655;
            }
            memory.check(GuestAddress{source},static_cast<std::size_t>(size),Access::Read);memory.check(GuestAddress{destination},static_cast<std::size_t>(size),Access::Write);
            // 沿用 SDK 的重疊／別名與可執行頁失效契約，不再另配一份完整大小的服務層暫存。
            memory.move(GuestAddress{destination},GuestAddress{source},static_cast<std::size_t>(size));copied=size;return 0;
        }
        if(debug_transport){
            if(size>UINT64_MAX-address)return 299;
            std::vector<std::uint8_t> probe;
            if(const auto checked=transfer_debug_memory(memory,pid,birth,writing?7U:6U,address,size,probe))return checked;
            do{
                const auto chunk=static_cast<std::size_t>(std::min<std::uint64_t>(65536,size-copied));std::vector<std::uint8_t> data(writing?chunk:0);
                if(writing)memory.read(GuestAddress{buffer+copied},data);
                if(const auto transferred=transfer_debug_memory(memory,pid,birth,writing?2U:1U,address+copied,chunk,data))return transferred;
                if(!writing)memory.write(GuestAddress{buffer+copied},data);copied+=chunk;
            }while(copied<size);return 0;
        }
#if !defined(_WIN32)
        if(size>static_cast<std::uint64_t>(INT64_MAX))return 299;
        auto pattern=(std::filesystem::temp_directory_path()/"x64arm-memory-XXXXXX").string();std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);
        const auto fd=::mkstemp(name.data());if(fd<0)return 8;
        struct Close{int fd;~Close(){::close(fd);}} cleanup{fd};(void)::unlink(name.data());(void)::fcntl(fd,F_SETFD,FD_CLOEXEC);
        if(::ftruncate(fd,static_cast<off_t>(size)))return 8;
        const auto copy=[&](bool to_file){
            std::array<std::uint8_t,65536> bytes{};
            for(std::uint64_t offset=0;offset<size;){const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(),size-offset));auto part=std::span(bytes).first(count);
                if(to_file)memory.read(GuestAddress{buffer+offset},part);
                std::size_t done=0;while(done<count){const auto transferred=to_file ? ::pwrite(fd,bytes.data()+done,count-done,static_cast<off_t>(offset+done)) : ::pread(fd,bytes.data()+done,count-done,static_cast<off_t>(offset+done));if(transferred<0 && errno==EINTR)continue;if(transferred<=0)return false;done+=static_cast<std::size_t>(transferred);}
                if(!to_file){memory.write(GuestAddress{buffer+offset},part);copied=offset+count;}offset+=count;
            }return true;
        };
        if(writing && !copy(true))return 8;
        std::array<std::uint64_t,5> values{address,size,0,0,0};std::uint32_t mode{};
        error=remote_process_request(memory,pid,birth,15,writing?1U:0U,values,mode,fd);if(writing)copied=std::min(size,values[4]);if(error)return error;
        if(values[4]!=size)return 299;
        if(!writing && !copy(false))return 299;return 0;
#else
        return 50;
#endif
    }catch(const MemoryFault&){return 299;}catch(const std::bad_alloc&){return 8;}
}
std::optional<std::uint32_t> WindowsServices::invoke_process_memory(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);const auto saved_error=last_error_;const bool native=service.family=="ntdll.dll";
    auto symbol=service.symbol;if(native && symbol.starts_with("Zw"))symbol.remove_prefix(2);else if(native && symbol.starts_with("Nt"))symbol.remove_prefix(2);
    const auto finish=[&](std::uint64_t result,std::uint32_t error){
        if(native){last_error_=saved_error;const auto status=error==0?0U:error==5?0xc0000022U:error==6?(object_type(call(0)).empty()?0xc0000008U:0xc0000024U):error==8?0xc0000017U:error==50?0xc00000bbU:error==299?0x8000000dU:error==487?(symbol.starts_with("UnmapViewOfSection")?0xc0000019U:0xc0000018U):error==0x10003U?0xc0000003U:error==998?0xc0000005U:error==1655?0xc0000604U:error==24?0xc0000004U:0xc000000dU;call.return_to(state,status);}
        else {if(error)last_error_=error;call.return_to(state,error?0:result);}return std::optional<std::uint32_t>{};
    };
    const bool wow64=symbol=="Wow64ReadVirtualMemory64" || symbol=="Wow64WriteVirtualMemory64";
    const bool reading=symbol=="ReadProcessMemory" || symbol=="ReadVirtualMemory" || symbol=="Wow64ReadVirtualMemory64",writing=symbol=="WriteProcessMemory" || symbol=="WriteVirtualMemory" || symbol=="Wow64WriteVirtualMemory64";
    const auto unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    try{
        if(reading || writing){
            const auto count_bits=static_cast<std::uint8_t>(wow64?64:bits);
            const auto size=call(3),count_pointer=call(4);if(count_pointer)memory.check(GuestAddress{count_pointer},count_bits/8,Access::Write);
            std::uint64_t copied{};const auto error=process_memory_copy(memory,call(0),call(1),call(2),size,writing,copied,native);
            // 搬移可以改寫客體 stack 上的參數；輸出位址沿用呼叫進入時擷取的值。
            if(count_pointer)memory.store(GuestAddress{count_pointer},count_bits,copied);return finish(1,error);
        }
        const bool query=symbol=="VirtualQuery" || symbol=="VirtualQueryEx" || symbol=="QueryVirtualMemory";
        const bool allocation2=symbol=="VirtualAlloc2" || symbol=="VirtualAlloc2FromApp" || symbol=="AllocateVirtualMemoryEx";
        const bool numa_allocate=symbol=="VirtualAllocExNuma";
        const bool allocate=symbol=="VirtualAlloc" || symbol=="VirtualAllocEx" || symbol=="AllocateVirtualMemory" || allocation2 || numa_allocate;
        const bool freeing=symbol=="VirtualFree" || symbol=="VirtualFreeEx" || symbol=="FreeVirtualMemory";
        const bool protect=symbol=="VirtualProtect" || symbol=="VirtualProtectEx" || symbol=="ProtectVirtualMemory";
        const bool flush=symbol=="FlushInstructionCache",unmap=symbol.starts_with("UnmapViewOfSection") || symbol.starts_with("UnmapViewOfFile");
        const bool extended=unmap?(native || symbol=="UnmapViewOfFile2"):native || symbol.ends_with("Ex") || flush || allocation2 || numa_allocate;
        const auto handle=allocation2 && !native && !call(0)?(x86_?UINT32_MAX:UINT64_MAX):extended?call(0):(x86_?UINT32_MAX:UINT64_MAX);std::uint32_t pid{};std::uint64_t birth{};
        const auto rights=query?0x400U:flush?0U:8U;
        auto error=process_memory_target(memory,handle,rights,pid,birth);
        const bool debug_transport=error==87 && (query || protect || flush) && !current_process_handle(handle) && !process_remote_target(handle,rights,pid,birth);
        if(error && !debug_transport)return finish(0,error);
        std::array<std::uint64_t,5> values{};std::uint64_t output=0,length=0,address_pointer=0,size_pointer=0;
        if(unmap){values[0]=call(extended?1:0);values[1]=native?0U:1U;values[2]=symbol=="UnmapViewOfSection"?0U:static_cast<std::uint32_t>(call(extended?2:1));}
        else if(native){
            if(query){if(static_cast<std::uint32_t>(call(2)))return finish(0,static_cast<std::uint32_t>(call(2))==2?50U:0x10003U);output=call(3);length=call(4);values[0]=call(1);if(call(5))memory.check(GuestAddress{call(5)},unit,Access::Write);}
            else {
                address_pointer=call(1);size_pointer=call(allocate && !allocation2?3U:2U);
                memory.check(GuestAddress{address_pointer},unit,Access::Read);memory.check(GuestAddress{address_pointer},unit,Access::Write);
                memory.check(GuestAddress{size_pointer},unit,Access::Read);memory.check(GuestAddress{size_pointer},unit,Access::Write);
                values[0]=memory.load(GuestAddress{address_pointer},bits);values[1]=memory.load(GuestAddress{size_pointer},bits);
                if(allocation2){values[2]=call(3);values[3]=call(4);}
                else if(allocate){if(call(2))return finish(0,50);values[2]=call(4);values[3]=call(5);}
                else if(protect){values[2]=call(3);output=call(4);memory.check(GuestAddress{output},4,Access::Write);}
                else values[2]=call(3);
            }
        }else {
            const auto arg=[&](unsigned index){return call(index+(extended?1U:0U));};
            values[0]=arg(0);if(query){output=arg(1);length=arg(2);}else {values[1]=arg(1);if(!flush)values[2]=arg(2);if(allocate)values[3]=arg(3);if(protect){output=arg(3);memory.check(GuestAddress{output},4,Access::Write);}}
        }
        VirtualAddressRequirements requirements;const VirtualAddressRequirements* constraints=nullptr;
        if(numa_allocate && static_cast<std::uint32_t>(call(5)))return finish(0,87);
        if(allocation2){
            const auto array=call(5);const auto count=static_cast<std::uint32_t>(call(6));
            if(count>32 || (count && !array))return finish(0,87);
            if(count)memory.check(GuestAddress{array},static_cast<std::size_t>(count)*16,Access::Read);
            std::uint64_t used=0;
            for(unsigned i=0;i<count;++i){
                const auto type=memory.load(GuestAddress{array+i*16},64),value=memory.load(GuestAddress{array+i*16+8},64);
                if(!type || type>6 || (used&(UINT64_C(1)<<type)))return finish(0,87);used|=UINT64_C(1)<<type;
                if(type==1){
                    if(!value || (x86_ && value>UINT32_MAX))return finish(0,87);memory.check(GuestAddress{value},unit*3,Access::Read);
                    requirements={memory.load(GuestAddress{value},bits),memory.load(GuestAddress{value+unit},bits),memory.load(GuestAddress{value+2*unit},bits)};constraints=&requirements;
                }else if(type==2){if(value)return finish(0,87);} // 所有虛擬 CPU 與記憶體位於 node 0。
                else if(type==5){if(value)return finish(0,50);}
                else return finish(0,50); // partition／AWE／image machine 需各自的物件契約。
            }
            if(!native && ((values[1]&4095U) || (values[0]&65535U)))return finish(0,87);
            if(symbol=="VirtualAlloc2FromApp" && (static_cast<std::uint32_t>(values[3])&0xf0U))return finish(0,87);
        }
        const auto structure_size=x86_?28U:48U;
        if(query){if(native && call(5))memory.store(GuestAddress{call(5)},bits,structure_size);if(length<structure_size)return finish(0,native?24U:87U);memory.check(GuestAddress{output},structure_size,Access::Write);}
        const auto operation=query?5U:allocate?2U:freeing?3U:protect?4U:flush?6U:unmap?7U:8U;std::uint32_t mode{};
        if(pid==process_id())error=local_memory_operation(memory,operation,values,-1,state.reg(Gpr::Rsp),output,constraints);
        else if(debug_transport){
            std::vector<std::uint8_t> data(protect?4U:0U);
            if(protect)for(unsigned i=0;i<4;++i)data[i]=static_cast<std::uint8_t>(values[2]>>(i*8));
            error=transfer_debug_memory(memory,pid,birth,query?4U:protect?5U:3U,values[0],values[1],data);
            if(!error){const auto get=[&](std::size_t at,unsigned bytes){std::uint64_t value=0;for(unsigned i=0;i<bytes;++i)value|=std::uint64_t{data.at(at+i)}<<(8*i);return value;};
                if(query)values={get(0,8),get(8,8),get(16,8),get(24,4)|(get(28,4)<<32),get(32,4)|(get(36,4)<<32)};
                else if(protect)values[4]=get(0,4);
            }
        }else if(allocation2){
#if !defined(_WIN32)
            auto pattern=(std::filesystem::temp_directory_path()/"x64arm-memory-requirements-XXXXXX").string();std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);
            const auto fd=::mkstemp(name.data());if(fd<0)return finish(0,8);
            struct Close{int fd;~Close(){::close(fd);}} cleanup{fd};(void)::unlink(name.data());(void)::fcntl(fd,F_SETFD,FD_CLOEXEC);
            std::array<std::uint8_t,24> bytes{};const std::array<std::uint64_t,3> fields{requirements.lowest,requirements.highest,requirements.alignment};
            for(unsigned i=0;i<3;++i)for(unsigned j=0;j<8;++j)bytes[i*8+j]=static_cast<std::uint8_t>(fields[i]>>(j*8));
            ssize_t written{};do{written=::pwrite(fd,bytes.data(),bytes.size(),0);}while(written<0 && errno==EINTR);
            if(written!=static_cast<ssize_t>(bytes.size()))return finish(0,8);
            error=remote_process_request(memory,pid,birth,15,9,values,mode,fd);
#else
            error=50;
#endif
        }else error=remote_process_request(memory,pid,birth,15,operation,values,mode);
        if(error)return finish(0,error);
        if(query){
            if(x86_ && (values[0]>UINT32_MAX || values[1]>UINT32_MAX || values[2]>UINT32_MAX))return finish(0,50);
            memory.write(GuestAddress{output},std::vector<std::uint8_t>(structure_size));
            memory.store(GuestAddress{output},bits,values[0]);memory.store(GuestAddress{output+unit},bits,values[1]);
            memory.store(GuestAddress{output+unit*2},32,static_cast<std::uint32_t>(values[3]));memory.store(GuestAddress{output+(x86_?12U:24U)},bits,values[2]);
            memory.store(GuestAddress{output+(x86_?16U:32U)},32,values[3]>>32);memory.store(GuestAddress{output+(x86_?20U:36U)},32,static_cast<std::uint32_t>(values[4]));memory.store(GuestAddress{output+(x86_?24U:40U)},32,values[4]>>32);
        }else if(protect)memory.store(GuestAddress{output},32,values[4]);
        if(native && !query && !unmap){if(x86_ && (values[0]>UINT32_MAX || values[1]>UINT32_MAX))return finish(0,50);memory.store(GuestAddress{address_pointer},bits,values[0]);memory.store(GuestAddress{size_pointer},bits,values[1]);}
        return finish(query?structure_size:allocate?values[0]:1U,0);
    }catch(const MemoryFault&){return finish(0,998);}catch(const std::bad_alloc&){return finish(0,8);}
}
std::uint64_t WindowsServices::installed_physical_memory(const GuestMemory& memory) const{
    // 與 GlobalMemoryStatusEx 的 ullTotalPhys 相同來源；比較模式或宿主查詢失敗時退回客體配置上限。
    if(runtime_.network.external_io){const auto snapshot=native_system_performance();if(!snapshot.error && snapshot.pages[3] && snapshot.pages[9])return snapshot.pages[3]*snapshot.pages[9];}
    return memory.limit();
}
std::uint32_t WindowsServices::memory_status_fields(GuestMemory& memory,std::array<std::uint64_t,6>& values) const{
    if(!runtime_.network.external_io)return 50;
    auto& cache=memory_status_cache_;const auto now=std::chrono::steady_clock::now();
    // 宿主實體記憶體快照（host_statistics64＋vm.swapusage sysctl 約 0.3 ms）每 100 ms 取一次；Windows 本身此 API 約 1 µs。
    if(!cache.physical_valid || now-cache.physical_at>std::chrono::milliseconds(100)){
        const auto snapshot=native_system_performance();cache.physical_at=now;cache.physical_valid=true;cache.physical_error=snapshot.error;
        if(!snapshot.error){
            const auto unit=snapshot.pages[9];if(!unit || !snapshot.pages[3] || snapshot.pages[4]>snapshot.pages[3] || snapshot.pages[0]>snapshot.pages[1])cache.physical_error=13;
            else cache.physical={snapshot.pages[3]*unit,snapshot.pages[4]*unit,snapshot.pages[1]*unit,(snapshot.pages[1]-snapshot.pages[0])*unit};
        }
    }
    if(cache.physical_error)return cache.physical_error;
    values[0]=cache.physical[0];values[1]=cache.physical[1];
    values[2]=std::min<std::uint64_t>(cache.physical[2],memory.limit());
    values[3]=std::min<std::uint64_t>(cache.physical[3],memory.remaining_capacity());
    const auto limit=user_address_limit();constexpr std::uint64_t minimum=0x10000;
    // 虛擬使用量在保留區數量、已配置容量或模組數量改變時重算（VirtualAlloc 保留後必須立即反映，見 memory_resource fixture），
    // 否則最多 5 秒重用；5 萬個保留區時單次走訪約 2～4 ms，合併走訪避免每次排序。
    if(!cache.valid || cache.regions!=runtime_.virtual_regions.size() || cache.capacity!=memory.remaining_capacity() ||
       cache.modules!=runtime_.loaded_modules.size() || cache.resources!=runtime_.resource_modules.size() || now-cache.at>std::chrono::seconds(5)){
        // 三個來源各自已排序（GuestMemory 映射、virtual_regions 的 std::map、少量模組映像），以合併方式累計聯集長度，不再建向量排序。
        std::vector<std::pair<std::uint64_t,std::uint64_t>> modules;
        const auto clip=[&](std::uint64_t begin,std::uint64_t size)->std::pair<std::uint64_t,std::uint64_t>{
            if(!size || begin>=limit)return {0,0};const auto end=begin+std::min(size,limit-begin);begin=std::max(begin,minimum);return begin<end?std::pair{begin,end}:std::pair<std::uint64_t,std::uint64_t>{0,0};
        };
        const auto push_module=[&](std::uint64_t begin,std::uint64_t size){const auto range=clip(begin,size);if(range.second)modules.push_back(range);};
        push_module(runtime_.image.base,runtime_.image.size);
        for(const auto& [base,module]:runtime_.loaded_modules){(void)base;push_module(module.image.base,module.image.size);}
        for(const auto& [base,module]:runtime_.resource_modules){(void)base;push_module(module.image.base,module.image.size);}
        std::sort(modules.begin(),modules.end());
        std::uint64_t used=0,last=minimum;
        const auto consume=[&](std::pair<std::uint64_t,std::uint64_t> range){if(range.second>last){used+=range.second-std::max(last,range.first);last=range.second;}};
        auto mapping=memory.next_mapping(GuestAddress{0});auto region=runtime_.virtual_regions.begin();std::size_t module_index=0;
        while(true){
            std::optional<std::pair<std::uint64_t,std::uint64_t>> next;unsigned source=0;
            if(mapping){next=std::pair{mapping->base.value,std::uint64_t{mapping->size}};source=1;}
            if(region!=runtime_.virtual_regions.end() && (!next || region->first<next->first)){next=std::pair{region->first,std::uint64_t{region->second.size}};source=2;}
            if(module_index<modules.size() && (!next || modules[module_index].first<next->first)){next=std::pair{modules[module_index].first,modules[module_index].second-modules[module_index].first};source=3;}
            if(!next)break;
            if(source==1){consume(clip(next->first,next->second));mapping=memory.next_mapping(GuestAddress{mapping->base.value+mapping->size});}
            else if(source==2){consume(clip(next->first,next->second));++region;}
            else {consume(modules[module_index]);++module_index;}
        }
        cache.at=now;cache.regions=runtime_.virtual_regions.size();cache.capacity=memory.remaining_capacity();cache.modules=runtime_.loaded_modules.size();cache.resources=runtime_.resource_modules.size();cache.used=used;cache.valid=true;
    }
    values[4]=limit-minimum;values[5]=values[4]-cache.used;return 0;
}
std::uint32_t WindowsServices::refresh_memory_resource_notifications(bool force){
    if(!force && memory_resource_poll_deadline_>runtime_.clock_ticks)return 0;
    std::optional<MemoryResourceState> condition;std::uint64_t next=UINT64_MAX;
    for(auto& [handle,object]:runtime_.wait_objects){
        (void)handle;if(object.kind!=WaitObject::Kind::MemoryResource)continue;
        if(!force && object.due>runtime_.clock_ticks){next=std::min(next,object.due);continue;}
        if(!condition)condition=system_memory_resource_state(native_system_performance());
        object.due=runtime_.clock_ticks+2500000; // 在既有等待迴圈中，最多每 250 ms 查核一次；不建立背景 producer。
        next=std::min(next,object.due);
        if(!condition->error){const bool signaled=object.memory_notification_type?condition->high:condition->low;if(object.signaled()!=signaled)object.set_signal(signaled);}
    }
    memory_resource_poll_deadline_=next;
    return condition?condition->error:0;
}
std::span<const WindowsServiceDescriptor> memory_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"kernel32.dll","CreateMemoryResourceNotification",1},{"kernel32.dll","QueryMemoryResourceNotification",2},
        {"kernel32.dll","InterlockedExchange",2},{"kernel32.dll","InterlockedIncrement",1},{"kernel32.dll","InterlockedDecrement",1},
        {"kernel32.dll","InterlockedExchangeAdd",2},{"kernel32.dll","InterlockedCompareExchange",3},
        {"kernel32.dll","InterlockedExchangePointer",2},{"kernel32.dll","InterlockedCompareExchangePointer",3},
        {"kernel32.dll","IsBadReadPtr",2},{"kernel32.dll","IsBadWritePtr",2},{"kernel32.dll","IsBadHugeReadPtr",2},{"kernel32.dll","IsBadHugeWritePtr",2},
        {"kernel32.dll","IsBadCodePtr",1},{"kernel32.dll","IsBadStringPtrA",2},{"kernel32.dll","IsBadStringPtrW",2},
        {"psapi.dll","QueryWorkingSetEx",3},{"kernel32.dll","K32QueryWorkingSetEx",3},
        {"psapi.dll","GetPerformanceInfo",2},{"kernel32.dll","K32GetPerformanceInfo",2},
        {"psapi.dll","GetProcessMemoryInfo",3},{"kernel32.dll","K32GetProcessMemoryInfo",3},
        {"kernel32.dll","FlushInstructionCache",3},{"kernel32.dll","FlushProcessWriteBuffers",0},{"psapi.dll","QueryWorkingSet",3},{"kernel32.dll","K32QueryWorkingSet",3},
        {"kernel32.dll","ReadProcessMemory",5},{"kernel32.dll","WriteProcessMemory",5},
        {"kernel32.dll","VirtualAllocEx",5},{"kernel32.dll","VirtualFreeEx",4},{"kernel32.dll","VirtualProtect",4},{"kernel32.dll","VirtualProtectEx",5},
        {"kernel32.dll","UnmapViewOfFileEx",2},{"kernel32.dll","UnmapViewOfFile2",3},
        {"kernel32.dll","VirtualAlloc",4},{"kernel32.dll","VirtualFree",3},{"kernel32.dll","VirtualQuery",3},{"kernel32.dll","VirtualQueryEx",4},
        {"kernel32.dll","VirtualAlloc2",7},{"kernel32.dll","VirtualAlloc2FromApp",7},{"kernel32.dll","VirtualAllocExNuma",6},
        {"kernel32.dll","GetNumaHighestNodeNumber",1},{"kernel32.dll","GetNumaProcessorNode",2},{"kernel32.dll","GetNumaProcessorNodeEx",2},
        {"kernel32.dll","GetNumaNodeProcessorMask",2},{"kernel32.dll","GetNumaNodeProcessorMaskEx",2},{"kernel32.dll","GetNumaNodeProcessorMask2",4},
        {"kernel32.dll","GetNumaAvailableMemoryNode",2},{"kernel32.dll","GetNumaAvailableMemoryNodeEx",2},
        {"kernel32.dll","GetNumaProximityNode",2},{"kernel32.dll","GetNumaProximityNodeEx",2},
        {"kernel32.dll","DiscardVirtualMemory",2},{"kernel32.dll","PrefetchVirtualMemory",4},
        {"kernel32.dll","OfferVirtualMemory",3},{"kernel32.dll","ReclaimVirtualMemory",2},
        {"ntdll.dll","NtAllocateVirtualMemory",6},{"ntdll.dll","NtFreeVirtualMemory",4},
        {"ntdll.dll","NtAllocateVirtualMemoryEx",7},{"ntdll.dll","ZwAllocateVirtualMemoryEx",7},
        {"ntdll.dll","NtProtectVirtualMemory",5},{"ntdll.dll","NtQueryVirtualMemory",6},{"ntdll.dll","NtUnmapViewOfSection",2},
        {"ntdll.dll","NtUnmapViewOfSectionEx",3,0,0,4},{"ntdll.dll","ZwUnmapViewOfSectionEx",3,0,0,4},
        {"ntdll.dll","NtReadVirtualMemory",5},{"ntdll.dll","NtWriteVirtualMemory",5},
        {"ntdll.dll","NtWow64ReadVirtualMemory64",5,0xa},{"ntdll.dll","NtWow64WriteVirtualMemory64",5,0xa},
        {"ntdll.dll","ZwReadVirtualMemory",5},{"ntdll.dll","ZwWriteVirtualMemory",5},
        {"ntdll.dll","ZwAllocateVirtualMemory",6},{"ntdll.dll","ZwFreeVirtualMemory",4},
        {"ntdll.dll","ZwProtectVirtualMemory",5},{"ntdll.dll","ZwQueryVirtualMemory",6},{"ntdll.dll","ZwUnmapViewOfSection",2}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_memory(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    poll_job_termination(memory);
    if(service.symbol=="ReadProcessMemory" || service.symbol=="WriteProcessMemory" || service.symbol=="FlushInstructionCache" || service.symbol.starts_with("UnmapViewOfFile") || service.symbol.starts_with("VirtualAlloc") || service.symbol.starts_with("VirtualFree") || service.symbol.starts_with("VirtualProtect") || service.symbol.starts_with("VirtualQuery") || service.family=="ntdll.dll")return invoke_process_memory(state,memory,service);
    const WindowsCall call(state,memory,service);
    if(service.symbol.starts_with("GetNuma")){
        const auto name=service.symbol;const bool extended=name.ends_with("Ex");
        const auto fail=[&](std::uint32_t error){last_error_=error;call.return_to(state,0);return std::optional<std::uint32_t>{};};
        // 虛擬 CPU 與同一 GuestMemory 共用一個 NUMA node；不借用宿主 CPU 編號／群組。
        const auto count=runtime_.processor_count;const auto mask=count==64?UINT64_MAX:(UINT64_C(1)<<count)-1;
        try{
            if(name=="GetNumaHighestNodeNumber"){
                if(!call(0))return fail(87);memory.store(GuestAddress{call(0)},32,0);
            }else if(name=="GetNumaProcessorNode" || name=="GetNumaProcessorNodeEx"){
                if(!call(1) || (extended && !call(0)))return fail(87);
                std::uint32_t number=static_cast<std::uint8_t>(call(0));bool valid=true;
                if(extended){memory.check(GuestAddress{call(0)},4,Access::Read);number=static_cast<std::uint32_t>(memory.load(GuestAddress{call(0)+2},8));valid=!memory.load(GuestAddress{call(0)},16) && !memory.load(GuestAddress{call(0)+3},8);}
                memory.check(GuestAddress{call(1)},extended?2U:1U,Access::Write);
                memory.store(GuestAddress{call(1)},static_cast<std::uint8_t>(extended?16:8),valid && number<count?0U:extended?0xffffU:0xffU);
                if(!valid || number>=count)return fail(87);
            }else if(name=="GetNumaProximityNode" || name=="GetNumaProximityNodeEx"){
                if(!call(1))return fail(87);const auto proximity=static_cast<std::uint32_t>(call(0));
                memory.store(GuestAddress{call(1)},static_cast<std::uint8_t>(extended?16:8),proximity?extended?0xffffU:0xffU:0U);
                if(proximity)return fail(87);
            }else {
                const auto node=name=="GetNumaNodeProcessorMask" || name=="GetNumaAvailableMemoryNode"?static_cast<std::uint8_t>(call(0)):static_cast<std::uint16_t>(call(0));
                if(node)return fail(87);
                if(name=="GetNumaNodeProcessorMask2"){
                    if(!call(3))return fail(87);memory.store(GuestAddress{call(3)},16,1);
                    if(!static_cast<std::uint16_t>(call(2)))return fail(122);
                }
                if(!call(1))return fail(87);
                if(name=="GetNumaNodeProcessorMask")memory.store(GuestAddress{call(1)},64,mask);
                else if(name=="GetNumaNodeProcessorMaskEx" || name=="GetNumaNodeProcessorMask2"){
                    const unsigned unit=x86_?4U:8U;memory.check(GuestAddress{call(1)},unit+8,Access::Write);
                    std::array<std::uint8_t,16> bytes{};for(unsigned i=0;i<unit;++i)bytes[i]=static_cast<std::uint8_t>(mask>>(i*8));
                    memory.write(GuestAddress{call(1)},std::span<const std::uint8_t>{bytes}.first(unit+8));
                }else if(name=="GetNumaAvailableMemoryNode" || name=="GetNumaAvailableMemoryNodeEx"){
                    auto available=static_cast<std::uint64_t>(memory.remaining_capacity());
                    if(runtime_.network.external_io){const auto snapshot=native_system_performance();if(snapshot.error)return fail(snapshot.error);if(!snapshot.pages[9] || snapshot.pages[4]>UINT64_MAX/snapshot.pages[9])return fail(13);available=std::min(available,snapshot.pages[4]*snapshot.pages[9]);}
                    memory.store(GuestAddress{call(1)},64,available);
                }else return fail(50);
            }
            call.return_to(state,1);return std::nullopt;
        }catch(const MemoryFault&){return fail(998);}
    }
    if(service.symbol=="CreateMemoryResourceNotification" || service.symbol=="QueryMemoryResourceNotification"){
        std::uint64_t result=0;
        try{
            if(service.symbol=="CreateMemoryResourceNotification"){
                const auto type=static_cast<std::uint32_t>(call(0));
                if(type>1)last_error_=87;else if(!runtime_.network.external_io)last_error_=50;
                else {const auto condition=system_memory_resource_state(native_system_performance());
                    if(condition.error)last_error_=condition.error;else{
                        WaitObject object;object.kind=WaitObject::Kind::MemoryResource;object.manual_reset=true;object.maximum=1;
                        object.memory_notification_type=type;object.count=(type?condition.high:condition.low)?1U:0U;
                        object.active=true;object.due=runtime_.clock_ticks+2500000;object.access=0x100001;
                        object.security_descriptor=synchronization_descriptor(memory,0).bytes;
                        memory_resource_poll_deadline_=std::min(memory_resource_poll_deadline_,object.due);
                        result=runtime_.allocate_wait_handle();runtime_.wait_objects.emplace(result,std::move(object));
                    }
                }
            }else{
                const auto object=runtime_.wait_objects.find(call(0));
                if(object==runtime_.wait_objects.end() || object->second.kind!=WaitObject::Kind::MemoryResource)last_error_=6;
                else if(!(object->second.access&1U))last_error_=5;
                else if(!call(1))last_error_=87;
                else {memory.check(GuestAddress{call(1)},4,Access::Write);const auto error=refresh_memory_resource_notifications(true);
                    if(error)last_error_=error;else{memory.store(GuestAddress{call(1)},32,object->second.signaled());result=1;}}
            }
        }catch(const MemoryFault&){last_error_=998;}catch(const SecurityDescriptorError& error){last_error_=error.code;}catch(const std::bad_alloc&){last_error_=8;}
        call.return_to(state,result);return std::nullopt;
    }
    if(service.symbol=="VirtualLock" || service.symbol=="VirtualUnlock"){
        std::uint64_t result=0;
        const auto address=call(0),size=call(1),limit=user_address_limit();
        if(!size || size>SIZE_MAX || address>=limit || size>limit-address)last_error_=87;
        else try {
            const auto locked=service.symbol=="VirtualLock"?memory.lock_pages(GuestAddress{address},static_cast<std::size_t>(size)):memory.unlock_pages(GuestAddress{address},static_cast<std::size_t>(size));
            if(locked.error)last_error_=residency_error(locked.error);
            else if(!locked.complete)last_error_=158;
            else result=1;
        }catch(const MemoryFault&){last_error_=998;}catch(const std::invalid_argument&){last_error_=87;}
        catch(const std::system_error& error){last_error_=residency_error(error.code());}catch(const std::bad_alloc&){last_error_=8;}
        call.return_to(state,result);return std::nullopt;
    }
    if(service.symbol=="GetProcessWorkingSetSize" || service.symbol=="SetProcessWorkingSetSize"){
        const bool setting=service.symbol=="SetProcessWorkingSetSize";std::uint64_t result=0;
        const auto handle=call(0),minimum=call(1),maximum=call(2),mask=x86_?UINT64_C(0xffffffff):UINT64_MAX;
        const bool allowed=setting?current_process_handle(handle,0x100):current_process_handle(handle,0x400) || current_process_handle(handle,0x1000);
        if(!allowed)last_error_=current_process_handle(handle)?5:process_object_type(handle)==u"Process"?50:6;
        else try {
            if(!setting){
                if(!minimum || !maximum)last_error_=87;
                else {const auto limits=memory.working_set_limits();const unsigned unit=x86_?4:8;
                    memory.check(GuestAddress{minimum},unit,Access::Write);memory.check(GuestAddress{maximum},unit,Access::Write);
                    memory.store(GuestAddress{minimum},static_cast<std::uint8_t>(unit*8),limits.minimum);
                    memory.store(GuestAddress{maximum},static_cast<std::uint8_t>(unit*8),limits.maximum);result=1;}
            }else if(minimum==mask && maximum==mask){
                const auto trim=memory.trim_working_set();if(trim.error)last_error_=residency_error(trim.error);else result=1;
            }else if(!minimum || minimum>maximum || minimum==mask || maximum==mask)last_error_=87;
            else {
                const auto snapshot=native_system_performance();
                if(snapshot.error)last_error_=snapshot.error;
                else if(snapshot.pages[4]<=512 || maximum/page>snapshot.pages[4]-512)last_error_=87;
                else if(const auto error=memory.set_working_set_limits(minimum,maximum))last_error_=residency_error(error);
                else result=1;
            }
        }catch(const MemoryFault&){last_error_=998;}catch(const std::system_error& error){last_error_=residency_error(error.code());}catch(const std::bad_alloc&){last_error_=8;}
        call.return_to(state,result);return std::nullopt;
    }
    if(service.symbol=="K32EmptyWorkingSet" || service.symbol=="EmptyWorkingSet"){
        std::uint64_t result=0;const auto handle=call(0);
        if(!current_process_handle(handle,0x500) && !current_process_handle(handle,0x1100))last_error_=current_process_handle(handle)?5:process_object_type(handle)==u"Process"?50:6;
        else try{const auto trimmed=memory.trim_working_set();if(!trimmed.error)result=1;
            else if(trimmed.error==std::errc::permission_denied)last_error_=5;
            else if(trimmed.error==std::errc::not_enough_memory)last_error_=8;
            else if(trimmed.error==std::errc::invalid_argument)last_error_=87;
            else if(trimmed.error==std::errc::operation_not_supported || trimmed.error==std::errc::function_not_supported)last_error_=50;
            else last_error_=998;
        }catch(const std::bad_alloc&){last_error_=8;}
        call.return_to(state,result);return std::nullopt;
    }
    if(service.symbol.starts_with("Interlocked")){
        const bool pointer=service.symbol.ends_with("Pointer");const unsigned unit=pointer && !x86_?8U:4U;
        memory.check(GuestAddress{call(0)},unit,Access::Read);memory.check(GuestAddress{call(0)},unit,Access::Write);
        auto* data=memory.native_pointer(GuestAddress{call(0)},unit,read_write);
        if(!data || call(0)%unit)throw HostCallFault("Interlocked 需要自然對齊且連續的客體記憶體");
        const auto exchange=[&]<class T>(T* location){
            std::atomic_ref<T> value(*location);const auto symbol=service.symbol;
            if(symbol=="InterlockedIncrement")return static_cast<std::uint64_t>(value.fetch_add(1)+T{1});
            if(symbol=="InterlockedDecrement")return static_cast<std::uint64_t>(value.fetch_sub(1)-T{1});
            if(symbol=="InterlockedExchangeAdd")return static_cast<std::uint64_t>(value.fetch_add(static_cast<T>(call(1))));
            if(symbol.find("CompareExchange")!=symbol.npos){auto expected=static_cast<T>(call(2));value.compare_exchange_strong(expected,static_cast<T>(call(1)));return static_cast<std::uint64_t>(expected);}
            return static_cast<std::uint64_t>(value.exchange(static_cast<T>(call(1))));
        };
        const auto result=unit==8?exchange(static_cast<std::uint64_t*>(data)):exchange(static_cast<std::uint32_t*>(data));
        call.return_to(state,result);return std::nullopt;
    }
    const auto saved_error=last_error_;const unsigned pointer_unit=x86_?4U:8U;const auto pointer_bits=static_cast<std::uint8_t>(pointer_unit*8);
    const auto arg=[&](unsigned index){return call(index);};const auto symbol=service.symbol;std::uint64_t result=0;
    const auto address_limit=user_address_limit();
    const auto fail=[&](std::uint32_t error){last_error_=error;result=0;};
    const auto executable_range=[&](std::uint64_t begin,std::uint64_t size){
        if(!size || size>UINT64_MAX-begin)return false;
        const auto end=begin+size;
        auto mapping=memory.query(GuestAddress{begin});if(!mapping)mapping=memory.next_mapping(GuestAddress{begin});
        for(;mapping && mapping->base.value<end;mapping=memory.next_mapping(GuestAddress{mapping->base.value+mapping->size}))
            if(mapping->permissions&permission(Access::Execute))return true;
        return false;
    };
    const auto offered_overlap=[&](std::uint64_t begin,std::uint64_t end){
        const auto item=runtime_.offered_memory_pages.lower_bound(begin&~(page-1));
        return item!=runtime_.offered_memory_pages.end() && item->first<end;
    };
    if(symbol=="DiscardVirtualMemory" || symbol=="OfferVirtualMemory" || symbol=="ReclaimVirtualMemory"){
        const auto address=arg(0),size=arg(1);
        const bool offer=symbol=="OfferVirtualMemory",reclaim=symbol=="ReclaimVirtualMemory";
        const auto priority=offer?static_cast<std::uint32_t>(arg(2)):0U;
        // DWORD 錯誤碼直接回傳；成功和失敗皆不以 LastError 取代這項契約。
        std::uint32_t status=0;
        if(!address || address%page || !size || size%page || address>=address_limit || size>address_limit-address)status=87;
        else if(offer && (priority<1 || priority>4))status=87;
        else try{
            auto& offered=runtime_.offered_memory_pages;const auto end=address+size;
            if(reclaim){
                std::vector<GuestMemory::Mapping> restore;bool lost=false;
                for(auto cursor=address;cursor<end;cursor+=page){const auto found=offered.find(cursor);
                    if(found==offered.end() || !memory.mapped(GuestAddress{cursor})){status=487;break;}
                    lost|=found->second.discarded;const auto perms=found->second.permissions;
                    if(!restore.empty() && restore.back().base.value+restore.back().size==cursor && restore.back().permissions==perms)restore.back().size+=page;
                    else restore.push_back({GuestAddress{cursor},page,perms});
                }
                if(!status){memory.protect_ranges(restore);offered.erase(offered.lower_bound(address),offered.lower_bound(end));status=lost?170U:0U;}
            }else{
                if(offered_overlap(address,end))status=487;
                for(auto cursor=address;!status && cursor<end;){const auto mapping=memory.query(GuestAddress{cursor});
                    if(!mapping)status=487;
                    else if(virtual_memory_information(memory,cursor).type!=0x20000U)status=87;
                    else if(!offer && mapping->permissions!=read_write)status=87;
                    else cursor=std::min(end,mapping->base.value+mapping->size);
                }
                if(!status && dynamic_code_blocked() && executable_range(address,size))status=1655;
                // 不能使目前 API 返回框架消失，或把返回地址當成可丟棄資料。
                if(!status && address<=state.reg(Gpr::Rsp) && state.reg(Gpr::Rsp)<end)status=87;
                if(!status){
                    std::map<std::uint64_t,OfferedMemoryPage> pending;
                    if(offer)for(auto cursor=address;cursor<end;cursor+=page){const auto mapping=memory.query(GuestAddress{cursor});
                        for(auto piece=cursor;piece<cursor+page;){const auto next=memory.query(GuestAddress{piece});
                            if(!next || next->permissions!=mapping->permissions){status=87;break;}
                            piece=std::min(cursor+page,next->base.value+next->size);
                        }
                        if(status)break;
                        pending.emplace(cursor,OfferedMemoryPage{mapping->permissions,priority,true});}
                    // 此後端採立即丟棄策略；Reclaim 依實際 lost 狀態要求客體重寫資料。
                    if(!status){
                        (void)memory.discard_contents(GuestAddress{address},static_cast<std::size_t>(size),offer);
                        if(offer)offered.merge(pending);
                        for(auto item=runtime_.reset_memory_pages.lower_bound(address);item!=runtime_.reset_memory_pages.end() && item->first<end;++item)item->second=true;
                    }
                }
            }
        }catch(const MemoryFault&){status=487;}catch(const std::invalid_argument&){status=87;}catch(const std::bad_alloc&){status=8;}
        last_error_=saved_error;call.return_to(state,status);return std::nullopt;
    }else if(symbol=="PrefetchVirtualMemory"){
        if(!current_process_handle(arg(0),0x10))fail(current_process_handle(arg(0))?5:process_object_type(arg(0))==u"Process"?50:6);
        else if(static_cast<std::uint32_t>(arg(3)) || !arg(1) || arg(1)>memory.limit()/(pointer_unit*2) || !arg(2))fail(87);
        else try{
            const auto count=arg(1),array=arg(2);const auto stride=pointer_unit*2;
            memory.check(GuestAddress{array},static_cast<std::size_t>(count*stride),Access::Read);
            std::vector<GuestMemory::MemoryRange> ranges;ranges.reserve(static_cast<std::size_t>(count));
            for(std::uint64_t i=0;i<count;++i){const auto address=memory.load(GuestAddress{array+i*stride},pointer_bits),size=memory.load(GuestAddress{array+i*stride+pointer_unit},pointer_bits);
                if(!size || address>=address_limit || size>address_limit-address){fail(87);break;}
                ranges.push_back({GuestAddress{address},static_cast<std::size_t>(size)});
            }
            if(ranges.size()==count){const auto prefetch=memory.prefetch(ranges);
                if(!prefetch.error)result=1;
                else fail(prefetch.error==std::errc::permission_denied?5:prefetch.error==std::errc::not_enough_memory?8:prefetch.error==std::errc::operation_not_supported?50:87);
            }
        }catch(const MemoryFault&){fail(998);}catch(const std::invalid_argument&){fail(87);}catch(const std::bad_alloc&){fail(8);}
    }else if(symbol=="GetPerformanceInfo" || symbol=="K32GetPerformanceInfo"){
        const unsigned unit=x86_?4:8,size=x86_?56:104;
        if(!arg(0))fail(87);else if(arg(1)<size)fail(24);else if(!runtime_.network.external_io)fail(50);
        else {memory.check(GuestAddress{arg(0)},size,Access::Write);auto snapshot=native_system_performance();
            std::uint32_t handles=0,processes=0,threads=0;
            const auto error=snapshot.error?snapshot.error:session_process_counters(memory,handles,processes,threads);
            if(error)fail(error);else {
                runtime_.observed_commit_peak=std::max(runtime_.observed_commit_peak,snapshot.pages[0]);snapshot.pages[2]=runtime_.observed_commit_peak;
                memory.write(GuestAddress{arg(0)},std::vector<std::uint8_t>(size));memory.store(GuestAddress{arg(0)},32,size);
                for(unsigned i=0;i<10;++i)memory.store(GuestAddress{arg(0)+unit+i*unit},x86_?32:64,snapshot.pages[i]);
                const auto counts=arg(0)+11*unit;memory.store(GuestAddress{counts},32,handles);memory.store(GuestAddress{counts+4},32,processes);memory.store(GuestAddress{counts+8},32,threads);result=1;
            }
        }
    }else if(symbol.starts_with("IsBad")){
        try {
            const auto start=arg(0),limit=x86_?UINT64_C(0xffffffff):UINT64_MAX;
            if(symbol.starts_with("IsBadStringPtr")){
                const unsigned unit=symbol.ends_with('W')?2:1;auto address=start;
                for(std::uint64_t i=0;i<arg(1);++i){
                    if(address>limit-(unit-1)){result=1;break;}
                    if(!memory.load(GuestAddress{address},static_cast<std::uint8_t>(unit*8)))break;
                    if(i+1<arg(1) && address>limit-unit){result=1;break;}address+=unit;
                }
            }else {
                const auto count=symbol=="IsBadCodePtr"?UINT64_C(1):arg(1);
                if(count && (start>limit || count-1>limit-start))result=1;
                else memory.check(GuestAddress{start},static_cast<std::size_t>(count),symbol.find("Write")!=symbol.npos?Access::Write:Access::Read);
            }
        }catch(const MemoryFault&){result=1;}
    }else if(symbol=="GetProcessMemoryInfo" || symbol=="K32GetProcessMemoryInfo"){
        const unsigned unit=x86_?4U:8U,base=8+8*unit,extended=base+unit;const auto bytes=static_cast<std::uint32_t>(arg(2));
        if(!current_process_handle(arg(0),0x400) && !current_process_handle(arg(0),0x1000))fail(current_process_handle(arg(0))?5:process_object_type(arg(0))==u"Process"?50:6);
        else if(!arg(1))fail(87);else if(bytes<base)fail(122);
        else{
            const auto size=bytes>=extended?extended:base;const auto usage=memory.usage();std::vector<std::uint8_t> output(size,0);
            memory.write(GuestAddress{arg(1)},output);memory.store(GuestAddress{arg(1)},32,size);
            const auto put=[&](unsigned index,std::uint64_t value){memory.store(GuestAddress{arg(1)+8+index*unit},static_cast<std::uint8_t>(unit*8),value);};
            put(0,usage.peak_committed);put(1,usage.committed);put(6,usage.private_commit);put(7,usage.peak_private_commit);if(size==extended)put(8,usage.private_commit);
            // 客體無 kernel pool／分頁檔 I/O；所有 commit 均有 backing，不編造宿主 fault 或 quota。
            result=1;
        }
    } else if(symbol=="QueryWorkingSetEx" || symbol=="K32QueryWorkingSetEx"){
        const unsigned unit=x86_?4U:8U;const auto bytes=static_cast<std::uint32_t>(arg(2));const auto bits=static_cast<std::uint8_t>(unit*8);
        if(!current_process_handle(arg(0),0x400))fail(current_process_handle(arg(0))?5:process_object_type(arg(0))==u"Process"?50:6);
        else if(bytes%(unit*2) || (bytes && !arg(1)))fail(87);
        else{
            memory.check(GuestAddress{arg(1)},bytes,Access::Read);memory.check(GuestAddress{arg(1)},bytes,Access::Write);
            for(std::uint64_t offset=0;offset<bytes;offset+=unit*2){const auto address=memory.load(GuestAddress{arg(1)+offset},bits);const auto mapping=memory.query(GuestAddress{address});std::uint64_t flags=0;
                if(mapping && !offered_overlap(address,address+1))flags=1U|(static_cast<std::uint64_t>(protection(mapping->permissions))<<4)|(shared_mapping_page(address)?0x8002U:0U)|(memory.page_locked(GuestAddress{address})?UINT64_C(1)<<22:0U);
                memory.store(GuestAddress{arg(1)+offset+unit},bits,flags);
            }result=1;
        }
    } else if(symbol=="FlushProcessWriteBuffers"){
        // 服務入口只在原生客體 worker 全數返回後執行；批次 join 的 acquire
        // 已納入每個 worker 的寫入，再由完整 fence 發布給後續客體執行。
        std::atomic_thread_fence(std::memory_order_seq_cst);
    } else if (symbol=="QueryWorkingSet" || symbol=="K32QueryWorkingSet") {
        const unsigned width=x86_?4U:8U;const auto bytes=static_cast<std::uint32_t>(arg(2));
        if(!current_process_handle(arg(0),0x410)) fail(current_process_handle(arg(0))?5:process_object_type(arg(0))==u"Process"?50:6);
        else if(bytes<width) fail(24);
        else {
            // 客體已提交的頁面均有 backing；保留區與宿主配置不屬於客體 working set。
            std::vector<std::pair<std::uint64_t,Permissions>> pages;
            for(auto mapping=memory.next_mapping(GuestAddress{0});mapping;mapping=memory.next_mapping(GuestAddress{mapping->base.value+mapping->size})) {
                for(auto base=mapping->base.value&~(page-1);base<mapping->base.value+mapping->size;base+=page) {
                    if(offered_overlap(base,base+page))continue;
                    if(!pages.empty() && pages.back().first==base) pages.back().second|=mapping->permissions;
                    else pages.emplace_back(base,mapping->permissions);
                }
            }
            memory.store(GuestAddress{arg(1)},static_cast<std::uint8_t>(width*8),pages.size());
            if(pages.size()>(bytes-width)/width) fail(24);
            else {
                memory.check(GuestAddress{arg(1)},(pages.size()+1)*width,Access::Write);
                for(std::size_t i=0;i<pages.size();++i) {
                    const auto [base,access]=pages[i];
                    const unsigned mode=(access&2)?((access&4)?6U:4U):(access&4)?((access&1)?3U:2U):(access&1)?1U:0U;
                    const auto attributes=mode|(shared_mapping_page(base)?0x120U:0U);
                    memory.store(GuestAddress{arg(1)+(i+1)*width},static_cast<std::uint8_t>(width*8),base|attributes);
                }
                result=1;
            }
        }
    } else throw HostCallFault("未知的虛擬記憶體服務");
    call.return_to(state,result);return std::nullopt;
}
}

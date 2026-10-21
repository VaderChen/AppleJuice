#include "x64arm/windows_services.hpp"
#include "x64arm/minidump.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <utility>

namespace x64arm {
namespace {
struct DumpFailure {std::uint32_t error;};
enum class DumpStep {CancelStart,IncludeThread,Thread,IncludeModule,Module,Memory,Remove,Vm,CancelEnd,IoStart,Write,IoFinish,Done};
struct DumpOperation {
    WindowsServiceDescriptor service;DumpSnapshot snapshot;
    std::uint64_t process{},file{},callback{},parameter{},scratch{},thread_handle{};
    std::uint32_t original_error{};DumpStep step{DumpStep::CancelStart};
    std::size_t index{},vm_pass{},written{},callback_count{};bool cancel{true},alternate{};
    DumpBytes encoded;std::vector<VirtualMemoryInformation> mappings;bool gathered{};
};
constexpr std::size_t callback_output=0x600,path_buffer=0x700,io_buffer=0x10800,io_chunk=65536,scratch_size=io_buffer+io_chunk;
constexpr std::uint32_t supported_flags=1U|2U|0x80U|0x100U|0x200U|0x400U|0x800U|0x1000U|0x2000U|0x4000U|0x20000U|0x80000U|0x1000000U|0x2000000U;
std::uint32_t hresult(std::uint32_t error){return error&0x80000000U?error:0x80070000U|(error&0xffffU);}
void put(DumpBytes& bytes,std::size_t at,unsigned width,std::uint64_t value){for(unsigned i=0;i<width;++i)bytes.at(at+i)=static_cast<std::uint8_t>(value>>(8*i));}
std::uint64_t read(const GuestMemory& memory,std::uint64_t address,unsigned width){return memory.load(GuestAddress{address},static_cast<std::uint8_t>(width*8));}
DumpBytes bytes(const GuestMemory& memory,std::uint64_t at,std::size_t size){if(size>minidump_maximum_bytes)throw DumpFailure{8};DumpBytes value(size);memory.read(GuestAddress{at},value);return value;}
std::size_t memory_bytes(const DumpSnapshot& snapshot){std::size_t total=0;for(const auto& range:snapshot.memory){if(range.bytes.size()>minidump_maximum_bytes-total)throw DumpFailure{8};total+=range.bytes.size();}return total;}
void capture(DumpSnapshot& snapshot,const GuestMemory& memory,std::uint64_t start,std::uint64_t size){
    if(!size)return;if(size>minidump_maximum_bytes || start>UINT64_MAX-size || snapshot.memory.size()>=65536)throw DumpFailure{8};
    const auto end=start+size;auto at=start;
    // 先列出未覆蓋的範圍，再修改容器；不在迭代 snapshot.memory 時追加。
    std::sort(snapshot.memory.begin(),snapshot.memory.end(),[](const auto& a,const auto& b){return a.base<b.base;});
    std::vector<std::pair<std::uint64_t,std::uint64_t>> missing;
    for(const auto& region:snapshot.memory){const auto region_end=region.base+region.bytes.size();if(region_end<=at || region.base>=end)continue;if(region.base>at)missing.emplace_back(at,region.base-at);at=std::max(at,region_end);if(at>=end)break;}
    if(at<end)missing.emplace_back(at,end-at);for(const auto& [base,length]:missing){if(length>minidump_maximum_bytes-memory_bytes(snapshot) || snapshot.memory.size()>=65536)throw DumpFailure{8};snapshot.memory.push_back({base,bytes(memory,base,static_cast<std::size_t>(length))});}
    std::sort(snapshot.memory.begin(),snapshot.memory.end(),[](const auto& a,const auto& b){return a.base<b.base;});
}
void remove(DumpSnapshot& snapshot,std::uint64_t begin,std::uint64_t size){
    if(!size)return;if(begin>UINT64_MAX-size)throw DumpFailure{87};const auto end=begin+size;std::vector<DumpMemory> remaining;
    for(auto& range:snapshot.memory){const auto last=range.base+range.bytes.size();if(begin>=last || end<=range.base){remaining.push_back(std::move(range));continue;}
        if(range.base<begin)remaining.push_back({range.base,DumpBytes(range.bytes.begin(),range.bytes.begin()+static_cast<std::ptrdiff_t>(begin-range.base))});
        if(last>end)remaining.push_back({end,DumpBytes(range.bytes.begin()+static_cast<std::ptrdiff_t>(end-range.base),range.bytes.end())});
    }
    if(remaining.size()>65536)throw DumpFailure{8};snapshot.memory=std::move(remaining);
    for(auto& thread:snapshot.threads){const auto last=thread.stack_begin+thread.stack.size();if(begin>=last || end<=thread.stack_begin)continue;
        if(begin<=thread.stack_begin){const auto length=std::min(last,end)-thread.stack_begin;thread.stack.erase(thread.stack.begin(),thread.stack.begin()+static_cast<std::ptrdiff_t>(length));thread.stack_begin+=length;}
        else thread.stack.resize(static_cast<std::size_t>(begin-thread.stack_begin));
    }
}
}
struct MinidumpState {std::map<std::uint64_t,DumpOperation> operations;std::uint64_t next{1};};
std::span<const WindowsServiceDescriptor> minidump_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"dbghelp.dll","MiniDumpWriteDump",7},{"dbghelp.dll","MiniDumpReadDumpStream",5},
        {"dbgcore.dll","MiniDumpWriteDump",7},{"dbgcore.dll","MiniDumpReadDumpStream",5}
    };return table;
}
void WindowsServices::discard_minidump(std::uint64_t token,GuestMemory& memory,const GuestState& state){
    if(!minidump_)return;if(minidump_.use_count()!=1)minidump_=std::make_shared<MinidumpState>(*minidump_);
    const auto found=minidump_->operations.find(token);if(found==minidump_->operations.end()){if(minidump_->operations.empty())minidump_.reset();return;}
    if(found->second.thread_handle){runtime_.process_thread_handles.erase(found->second.thread_handle);runtime_.thread_handle_access.erase(found->second.thread_handle);runtime_.handle_flags.erase(found->second.thread_handle);}
    if(found->second.scratch)(void)free_heap(memory,found->second.scratch,state);minidump_->operations.erase(found);if(minidump_->operations.empty())minidump_.reset();
}
std::optional<std::uint32_t> WindowsServices::resume_minidump(GuestState& state,GuestMemory& memory,std::uint64_t token,std::uint64_t result){return advance_minidump(state,memory,token,result);}
std::optional<std::uint32_t> WindowsServices::invoke_minidump(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto unit=x86_?4U:8U;
    if(service.symbol=="MiniDumpReadDumpStream"){
        std::uint64_t directory=0,pointer=0;std::uint32_t size=0,error=0;
        try{
            for(unsigned i:{2U,3U})if(arg(i))memory.check(GuestAddress{arg(i)},unit,Access::Write);if(arg(4))memory.check(GuestAddress{arg(4)},4,Access::Write);
            const auto view=mapping_view_info(arg(0));
            const auto check=[&](std::uint64_t offset,std::uint64_t length){if(arg(0)>UINT64_MAX-offset || length>UINT64_MAX-arg(0)-offset)throw DumpFailure{13};
                if(view && (arg(0)<view->base || arg(0)-view->base>view->size || offset>view->size-(arg(0)-view->base) || length>view->size-(arg(0)-view->base)-offset))throw DumpFailure{13};
                memory.check(GuestAddress{arg(0)+offset},static_cast<std::size_t>(length),Access::Read);
            };
            if(!arg(0))throw DumpFailure{13};check(0,32);
            if(!arg(0) || read(memory,arg(0),4)!=0x504d444d || (read(memory,arg(0)+4,4)&0xffff)!=0xa793)throw DumpFailure{13};
            const auto count=read(memory,arg(0)+8,4),rva=read(memory,arg(0)+12,4);if(count>65536 || arg(0)>UINT64_MAX-rva || count>(UINT64_MAX-arg(0)-rva)/12)throw DumpFailure{13};
            check(rva,count*12);
            for(std::uint64_t i=0;i<count;++i){const auto at=arg(0)+rva+i*12;if(read(memory,at,4)!=static_cast<std::uint32_t>(arg(1)))continue;
                size=static_cast<std::uint32_t>(read(memory,at+4,4));const auto offset=read(memory,at+8,4);if(arg(0)>UINT64_MAX-offset || size>minidump_maximum_bytes)throw DumpFailure{13};
                pointer=arg(0)+offset;check(offset,size);directory=at;break;
            }
            if(!directory)throw DumpFailure{2};
        }catch(const DumpFailure& failed){error=failed.error;}catch(const MemoryFault&){error=998;}
        if(error){last_error_=error;directory=pointer=size=0;}
        try{if(arg(2))memory.store(GuestAddress{arg(2)},x86_?32:64,directory);if(arg(3))memory.store(GuestAddress{arg(3)},x86_?32:64,pointer);if(arg(4))memory.store(GuestAddress{arg(4)},32,size);}catch(const MemoryFault&){last_error_=998;error=998;}
        arg.return_to(state,error?0:1);return std::nullopt;
    }
    std::uint64_t token=0;
    try{
        const auto process=arg(0);const auto pid=static_cast<std::uint32_t>(arg(1)),flags=static_cast<std::uint32_t>(arg(3));
        if(flags&~0x03ffffffU)throw DumpFailure{87};if(flags&~supported_flags)throw DumpFailure{50};
        const auto target=process_handle_pid(process,0x410);if(!target)throw DumpFailure{process_object_type(process)==u"Process"?5U:6U};
        if(pid!=target)throw DumpFailure{87};if(!current_process_handle(process,0x410))throw DumpFailure{50};
        if(!minidump_)minidump_=std::make_shared<MinidumpState>();else if(minidump_.use_count()!=1)minidump_=std::make_shared<MinidumpState>(*minidump_);
        if(minidump_->operations.size()>=16)throw DumpFailure{8};token=minidump_->next++;
        auto& operation=minidump_->operations[token];operation.service=service;operation.process=process;operation.file=arg(2);operation.original_error=last_error_;
        auto& snapshot=operation.snapshot;snapshot.x86=x86_;snapshot.flags=flags;snapshot.pid=pid;snapshot.processors=runtime_.processor_count;
        const auto filetime=runtime_.system_filetime();snapshot.timestamp=static_cast<std::uint32_t>(filetime/10000000-11644473600ULL);
        if(arg(6)){operation.callback=read(memory,arg(6),unit);operation.parameter=read(memory,arg(6)+unit,unit);}
        std::uint32_t exception_thread=0;std::optional<GuestState> exception_context;
        if(arg(4)){
            exception_thread=static_cast<std::uint32_t>(read(memory,arg(4),4));const auto pointers=read(memory,arg(4)+4,unit);
            const auto record=read(memory,pointers,unit),context=read(memory,pointers+unit,unit);const auto count=read(memory,record+(x86_?16:24),4);if(count>15 || !threads_.items.contains(exception_thread))throw DumpFailure{87};
            DumpBytes exception(152);put(exception,0,4,read(memory,record,4));put(exception,4,4,read(memory,record+4,4));put(exception,8,8,read(memory,record+8,unit));put(exception,16,8,read(memory,record+(x86_?12:16),unit));put(exception,24,4,count);
            for(unsigned i=0;i<count;++i)put(exception,32+i*8,8,read(memory,record+(x86_?20:32)+i*unit,unit));
            exception_context=read_context(memory,context,state);snapshot.exception_context=minidump_context(*exception_context,x86_);snapshot.exception=std::pair{exception_thread,std::move(exception)};
        }
        if(arg(5)){
            const auto count=read(memory,arg(5),4),array=read(memory,arg(5)+4,unit);if(count>1024)throw DumpFailure{87};const auto stride=x86_?12U:16U;std::size_t total=0;
            for(std::uint64_t i=0;i<count;++i){if(array>UINT64_MAX-i*stride)throw DumpFailure{87};const auto at=array+i*stride;const auto type=static_cast<std::uint32_t>(read(memory,at,4));const auto length=read(memory,at+4,4),data=read(memory,at+8,unit);
                if((type<=0xffffU && type!=10 && type!=11) || std::any_of(snapshot.user_streams.begin(),snapshot.user_streams.end(),[&](const auto& stream){return stream.first==type;}))throw DumpFailure{87};
                if(length>16*1024*1024 || length>minidump_maximum_bytes-total || snapshot.user_streams.size()>=1024)throw DumpFailure{8};total+=static_cast<std::size_t>(length);snapshot.user_streams.emplace_back(type,bytes(memory,data,static_cast<std::size_t>(length)));
            }
        }
        for(const auto& [id,thread]:threads_.items){if(thread.status==ThreadStatus::Exited)continue;if(snapshot.threads.size()>=4096)throw DumpFailure{8};
            DumpThread item;item.id=static_cast<std::uint32_t>(id);item.teb=thread.teb;item.suspend=thread.suspended;item.priority_class=runtime_.process_priority_class;item.priority=static_cast<std::uint32_t>(thread.priority);item.start=thread.entry;item.affinity=thread.affinity;item.name=thread.description;item.writing=id==threads_.current;
            auto cursor=id==threads_.current?state:thread.context;if(exception_context && id==exception_thread)cursor=*exception_context;item.context=minidump_context(cursor,x86_);
            const auto stack_low=thread.stack_base,stack_high=stack_low+thread.stack_size;item.stack_begin=cursor.reg(Gpr::Rsp);item.stack_end=stack_high;item.stack_limit=stack_low;item.instruction=cursor.rip.value;
            // active fiber 的 StackLimit／StackBase 位於同一份 TEB，不依舊 thread reserve 推測。
            if(thread.teb){const auto low=read(memory,thread.teb+unit*2,unit),high=read(memory,thread.teb+unit,unit);if(high>low){item.stack_end=high;item.stack_limit=low;}}
            if(flags&0x100)item.flags|=0x20;if(flags&0x1000)item.flags|=0x40;snapshot.threads.push_back(std::move(item));
        }
        const auto module=[&](const RuntimeImage& image){
            if(snapshot.modules.size()>=4096)throw DumpFailure{8};DumpModule item;item.image=image;item.path=image.path;if(item.path.empty())item.path=runtime_.image.path;
            item.flags=1|4|8;if(flags&1)item.flags|=2;if(flags&0x2000)item.flags|=0x40;if(flags&0x100)item.flags|=0x20;
            try{if(read(memory,image.base,2)==0x5a4d){const auto pe=image.base+read(memory,image.base+60,4),opt=pe+24;if(read(memory,pe,4)==0x4550){item.timestamp=static_cast<std::uint32_t>(read(memory,pe+8,4));item.checksum=static_cast<std::uint32_t>(read(memory,opt+64,4));item.header_size=static_cast<std::uint32_t>(read(memory,opt+60,4));if(item.header_size>image.size)throw DumpFailure{13};
                const auto sections=read(memory,pe+6,2),section_table=opt+read(memory,pe+20,2);if(sections>96)throw DumpFailure{13};
                for(std::uint64_t i=0;i<sections;++i){const auto section=section_table+i*40,rva=read(memory,section+12,4),size=std::max(read(memory,section+8,4),read(memory,section+16,4));
                    if(rva>image.size || size>image.size-rva)throw DumpFailure{13};if(size)item.sections.push_back({image.base+rva,size,static_cast<std::uint32_t>(read(memory,section+36,4))});
                }
                const auto directory=read(memory,opt,2)==0x10b?96U:112U;
                if(read(memory,opt+directory-4,4)>6){const auto rva=read(memory,opt+directory+48,4),size=read(memory,opt+directory+52,4);if(size>28*1024 || rva>image.size || size>image.size-rva)throw DumpFailure{13};
                    for(std::uint64_t at=image.base+rva;size && at+28<=image.base+rva+size;at+=28){const auto type=read(memory,at+12,4),length=read(memory,at+16,4),data=read(memory,at+20,4);if((type!=2 && type!=4) || !data || !length)continue;if(data>image.size || length>image.size-data || length>1024*1024)throw DumpFailure{13};
                        if(type==2){item.cv_pointer=image.base+data;item.cv=bytes(memory,item.cv_pointer,static_cast<std::size_t>(length));}else {item.misc_pointer=image.base+data;item.misc=bytes(memory,item.misc_pointer,static_cast<std::size_t>(length));}}
                }
            }}}catch(const MemoryFault&){throw DumpFailure{299};}
            // 沿用既有的 PE 資源查找，讀取實際 VS_FIXEDFILEINFO。
            const auto resource=read(memory,image.base,2)==0x5a4d?gui_resource(memory,image.base,1,16,true):0;
            if(resource){const auto rva=read(memory,resource,4),length=read(memory,resource+4,4);if(rva>image.size || length>image.size-rva || length<6)throw DumpFailure{13};
                const auto data=image.base+rva;const auto size=read(memory,data,2),value_size=read(memory,data+2,2);
                if(size>length || size<6)throw DumpFailure{13};std::uint64_t at=6;std::u16string key;
                for(;at+2<=size;at+=2){const auto c=read(memory,data+at,2);if(!c){at+=2;break;}key+=static_cast<char16_t>(c);}
                at=(at+3)&~UINT64_C(3);if(key==u"VS_VERSION_INFO" && value_size==52 && at<=size && size-at>=52 && read(memory,data+at,4)==0xfeef04bd)
                    for(unsigned i=0;i<13;++i)item.version[i]=static_cast<std::uint32_t>(read(memory,data+at+i*4,4));
            }
            snapshot.modules.push_back(std::move(item));
        };
        if(runtime_.image.base)module(runtime_.image);for(const auto& [base,loaded]:runtime_.loaded_modules){(void)base;module(loaded.image);}for(const auto& [base,image]:runtime_.builtin_images){(void)base;module(image);}
        if(flags&0x800){auto at=UINT64_C(0);while(at<user_address_limit()){const auto info=virtual_memory_information(memory,at);if(!info.size || info.size>user_address_limit()-at || snapshot.information.size()>=65536)throw DumpFailure{8};snapshot.information.push_back(info);at+=info.size;}}
        // 保存客體原有區域；後續回呼使用的 dumper scratch 不應混入 dump。
        for(auto at=UINT64_C(0);;){const auto mapping=memory.next_mapping(GuestAddress{at});if(!mapping)break;
            auto info=virtual_memory_information(memory,mapping->base.value);info.base=mapping->base.value;info.size=mapping->size;operation.mappings.push_back(info);
            if((flags&2) && (mapping->permissions&permission(Access::Read)) && !((flags&0x1000000) && (info.protection&0x400))){
                try{capture(snapshot,memory,mapping->base.value,mapping->size);}catch(const MemoryFault&){if(!(flags&0x20000) || (flags&0x2000000))throw DumpFailure{299};}
            }
            const auto end=mapping->base.value+mapping->size;if(end<=at || operation.mappings.size()>65536)throw DumpFailure{8};at=end;
        }
        if(operation.callback){operation.scratch=allocate_heap(memory,scratch_size);if(!operation.scratch)throw DumpFailure{8};}
        return advance_minidump(state,memory,token);
    }catch(const DumpFailure& error){last_error_=hresult(error.error);}catch(const MemoryFault&){last_error_=hresult(998);}catch(const std::bad_alloc&){last_error_=hresult(8);}catch(const std::length_error&){last_error_=hresult(8);}catch(const HostCallFault&){last_error_=hresult(13);}
    if(token)discard_minidump(token,memory,state);arg.return_to(state,0);return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::advance_minidump(GuestState& state,GuestMemory& memory,std::uint64_t token,std::optional<std::uint64_t> returned){
    if(!minidump_ || !minidump_->operations.contains(token))throw HostCallFault("minidump 回呼缺少狀態");if(minidump_.use_count()!=1)minidump_=std::make_shared<MinidumpState>(*minidump_);
    auto& op=minidump_->operations.at(token);auto& snapshot=op.snapshot;const WindowsCall arg(state,memory,op.service);const auto unit=x86_?4U:8U,body=x86_?12U:16U;
    const auto finish=[&](bool success,std::uint32_t error=0)->std::optional<std::uint32_t>{last_error_=success?op.original_error:hresult(error);discard_minidump(token,memory,state);arg.return_to(state,success);return std::nullopt;};
    const auto release_thread=[&]{if(op.thread_handle){runtime_.process_thread_handles.erase(op.thread_handle);runtime_.thread_handle_access.erase(op.thread_handle);runtime_.handle_flags.erase(op.thread_handle);op.thread_handle=0;}};
    const auto callback=[&](unsigned type,std::uint32_t flags=0)->std::optional<std::uint32_t>{
        if(++op.callback_count>131072)throw DumpFailure{8};DumpBytes input(x86_?772:1312);put(input,0,4,snapshot.pid);put(input,4,unit,op.process);put(input,4+unit,4,type);
        memory.write(GuestAddress{op.scratch+callback_output},DumpBytes(52));memory.store(GuestAddress{op.scratch+callback_output},32,flags);
        if(type==6){memory.store(GuestAddress{op.scratch+callback_output},32,1);}
        else if(type==3)put(input,body,4,snapshot.threads.at(op.index).id);
        else if(type==1){auto& thread=snapshot.threads.at(op.index);if(runtime_.process_thread_handles.size()>=1024)throw DumpFailure{8};op.thread_handle=runtime_.allocate_wait_handle();runtime_.process_thread_handles.emplace(op.thread_handle,std::pair{std::uint64_t{thread.id},false});runtime_.thread_handle_access[op.thread_handle]=0x1fffff;
            put(input,body,4,thread.id);put(input,body+4,unit,op.thread_handle);const auto context=body+(x86_?8U:16U);std::copy(thread.context.begin(),thread.context.end(),input.begin()+context);
            put(input,body+(x86_?724U:1248U),4,thread.context.size());put(input,body+(x86_?728U:1252U),8,thread.stack_end);put(input,body+(x86_?736U:1260U),8,thread.stack_limit);
        }else if(type==4)put(input,body,8,snapshot.modules.at(op.index).image.base);
        else if(type==0){const auto& module=snapshot.modules.at(op.index);if(module.path.size()>32767)throw DumpFailure{87};memory.write(GuestAddress{op.scratch+path_buffer},utf16_bytes(module.path));
            put(input,body,unit,op.scratch+path_buffer);put(input,body+unit,8,module.image.base);put(input,body+unit+8,4,module.image.size);put(input,body+unit+12,4,module.checksum);put(input,body+unit+16,4,module.timestamp);
            for(unsigned i=0;i<13;++i)put(input,body+unit+20+i*4,4,module.version[i]);const auto cv=body+unit+72;put(input,cv,unit,module.cv_pointer);put(input,cv+unit,4,module.cv.size());put(input,cv+unit+4,unit,module.misc_pointer);put(input,cv+unit*2+4,4,module.misc.size());
        }else if(type==10){const auto& range=snapshot.memory.at(op.index);auto info=virtual_memory_information(memory,range.base);info.base=range.base;info.size=range.bytes.size();const auto output=op.scratch+callback_output;
            memory.store(GuestAddress{output},64,info.base);memory.store(GuestAddress{output+8},64,info.allocation);memory.store(GuestAddress{output+16},32,info.initial);memory.store(GuestAddress{output+24},64,info.size);memory.store(GuestAddress{output+32},32,info.state);memory.store(GuestAddress{output+36},32,info.protection);memory.store(GuestAddress{output+40},32,info.type);memory.store(GuestAddress{output+48},32,1);
        }else if(type==11 || type==12 || type==13){put(input,body,unit,op.file);put(input,body+unit,8,op.written);
            if(type==12){const auto count=std::min<std::size_t>(io_chunk,op.encoded.size()-op.written);memory.write(GuestAddress{op.scratch+io_buffer},std::span<const std::uint8_t>{op.encoded}.subspan(op.written,count));put(input,body+unit+8,unit,op.scratch+io_buffer);put(input,body+unit*2+8,4,count);}
        }
        memory.write(GuestAddress{op.scratch},input);GuestCallback call;call.target=op.callback;call.arguments={op.parameter,op.scratch,op.scratch+callback_output};start_callbacks(state,memory,{std::move(call)},Continuation::MiniDumpOperation,token);return std::nullopt;
    };
    const auto gather=[&]{
        if(op.gathered)return;op.gathered=true;const bool full=(snapshot.flags&2)!=0;
        for(auto& thread:snapshot.threads){if(!(thread.flags&1))continue;
            if(thread.flags&2){if(thread.stack_begin<thread.stack_limit || thread.stack_begin>thread.stack_end)throw DumpFailure{299};
                const auto size=thread.stack_end-thread.stack_begin;if(size>16*1024*1024)throw DumpFailure{8};thread.stack=bytes(memory,thread.stack_begin,static_cast<std::size_t>(size));
                if(!full)capture(snapshot,memory,thread.stack_begin,size);
            }
            if(!full && (thread.flags&0x10) && !(snapshot.flags&0x400)){
                if(const auto mapping=memory.query(GuestAddress{thread.instruction});mapping && (mapping->permissions&permission(Access::Read))){
                    const auto begin=std::max(mapping->base.value,thread.instruction>128?thread.instruction-128:0),end=std::min(mapping->base.value+mapping->size,thread.instruction+128);
                    capture(snapshot,memory,begin,end-begin);
                }
            }
            if(!full && (thread.flags&0x20))capture(snapshot,memory,thread.teb,0x4000);
        }
        if(full)return;
        if(snapshot.flags&0x200)for(const auto& mapping:op.mappings)if(mapping.type==0x20000 && mapping.protection==4)capture(snapshot,memory,mapping.base,mapping.size);
        for(const auto& module:snapshot.modules){if(!(module.flags&1))continue;
            if(snapshot.flags&0x80000)capture(snapshot,memory,module.image.base,module.header_size?module.header_size:memory.readable_prefix(GuestAddress{module.image.base},static_cast<std::size_t>(std::min<std::uint64_t>(4096,module.image.size))).size());
            if(module.flags&(2U|0x40U)){
                if(!module.sections.empty()){for(const auto& section:module.sections){const bool data=(section.flags&0xc0)!=0,code=(section.flags&0x20)!=0 || ((section.flags&0x40)!=0 && !(section.flags&0x80000000U));
                    if(((module.flags&2) && data) || ((module.flags&0x40) && code))capture(snapshot,memory,section.base,section.size);
                }}else for(const auto& mapping:op.mappings){const auto start=std::max(mapping.base,module.image.base),end=std::min(mapping.base+mapping.size,module.image.base+module.image.size);if(end<=start)continue;
                    const auto mode=mapping.protection&255;const bool data=mode==4 || mode==8 || mode==64 || mode==128,code=mode==16 || mode==32 || mode==64 || mode==128;
                    if(((module.flags&2) && data) || ((module.flags&0x40) && code))capture(snapshot,memory,start,end-start);
                }
            }
            if((module.flags&0x20) && module.image.tls_index){const auto index=read(memory,module.image.tls_index,4);if(index>=64)throw DumpFailure{13};
                for(const auto& thread:snapshot.threads)if((thread.flags&1) && (thread.flags&0x20)){
                    const auto vector=read(memory,thread.teb+(x86_?0x2c:0x58),unit),data=read(memory,vector+index*unit,unit);
                    capture(snapshot,memory,vector,64*unit);if(data)capture(snapshot,memory,data,std::max<std::size_t>(1,module.image.tls_data.size()+module.image.tls_zero));
                }
            }
        }
        if(snapshot.flags&0x100){capture(snapshot,memory,peb_base_.value,4096);const auto parameters=read(memory,peb_base_.value+(x86_?0x10:0x20),unit);
            if(parameters){const auto size=read(memory,parameters,4);if(size>65536)throw DumpFailure{13};capture(snapshot,memory,parameters,size);
                for(const auto offset:x86_?std::array<unsigned,4>{0x24,0x30,0x38,0x40}:std::array<unsigned,4>{0x38,0x50,0x60,0x70}){
                    const auto length=read(memory,parameters+offset,2),capacity=read(memory,parameters+offset+2,2),buffer=read(memory,parameters+offset+(x86_?4U:8U),unit);
                    if(length>capacity)throw DumpFailure{13};if(buffer && capacity)capture(snapshot,memory,buffer,capacity);
                }
            }
        }
    };
    try{
        if(returned){const bool yes=static_cast<std::uint32_t>(*returned)!=0;const auto output=op.scratch+callback_output;const auto value=static_cast<std::uint32_t>(read(memory,output,4));
            switch(op.step){
            case DumpStep::CancelStart:case DumpStep::CancelEnd:if(yes){if(read(memory,output+4,4))throw DumpFailure{1223};op.cancel=value!=0;}op.step=op.step==DumpStep::CancelStart?DumpStep::IncludeThread:DumpStep::IoStart;op.index=0;break;
            case DumpStep::IncludeThread:if(!yes)snapshot.threads.at(op.index).flags=0;else snapshot.threads.at(op.index).flags&=value;op.step=yes?DumpStep::Thread:DumpStep::IncludeThread;if(!yes)++op.index;break;
            case DumpStep::Thread:release_thread();if(!yes)throw DumpFailure{0x80004005};snapshot.threads.at(op.index).flags&=value;++op.index;op.step=DumpStep::IncludeThread;break;
            case DumpStep::IncludeModule:if(!yes)snapshot.modules.at(op.index).flags=0;else snapshot.modules.at(op.index).flags&=value;op.step=yes?DumpStep::Module:DumpStep::IncludeModule;if(!yes)++op.index;break;
            case DumpStep::Module:if(!yes)throw DumpFailure{0x80004005};if(value&~0x7fU)throw DumpFailure{87};snapshot.modules.at(op.index).flags=value;++op.index;op.step=DumpStep::IncludeModule;break;
            case DumpStep::Memory:case DumpStep::Remove:{const auto address=read(memory,output,8),length=read(memory,output+8,4);if(!yes || !length){op.step=op.step==DumpStep::Memory?DumpStep::Remove:DumpStep::CancelEnd;break;}
                if(op.step==DumpStep::Memory)capture(snapshot,memory,address,length);else remove(snapshot,address,length);break;}
            case DumpStep::Vm:{if(!yes || !read(memory,output+48,4))throw DumpFailure{0x80004004};auto& region=snapshot.memory.at(op.index);const auto address=read(memory,output,8),length=read(memory,output+24,8),end=region.base+region.bytes.size();
                if(address<region.base || address>end || length>end-address || ((address!=region.base || length!=region.bytes.size()) && (address%4096 || length%4096)))throw DumpFailure{87};
                if(address>region.base)for(auto& thread:snapshot.threads){if(thread.stack_begin>=region.base && thread.stack_begin<address){const auto cut=std::min(address,thread.stack_begin+thread.stack.size())-thread.stack_begin;thread.stack.erase(thread.stack.begin(),thread.stack.begin()+static_cast<std::ptrdiff_t>(cut));thread.stack_begin+=cut;}}
                for(auto& thread:snapshot.threads){const auto last=thread.stack_begin+thread.stack.size();if(last>address+length && thread.stack_begin<end)thread.stack.resize(static_cast<std::size_t>(std::max(thread.stack_begin,address+length)-thread.stack_begin));}
                region.bytes=DumpBytes(region.bytes.begin()+static_cast<std::ptrdiff_t>(address-region.base),region.bytes.begin()+static_cast<std::ptrdiff_t>(address-region.base+length));region.base=address;
                if(++op.vm_pass==2){op.vm_pass=0;++op.index;}break;}
            case DumpStep::IoStart:op.alternate=yes && value==1;if(yes && (value&0x80000000U))throw DumpFailure{value};op.step=DumpStep::Write;break;
            case DumpStep::Write:if(!yes || value)throw DumpFailure{value?value:0x80004005U};op.written+=std::min<std::size_t>(io_chunk,op.encoded.size()-op.written);break;
            case DumpStep::IoFinish:if(!yes || value)throw DumpFailure{value?value:0x80004005U};op.step=DumpStep::Done;break;
            default:throw HostCallFault("minidump 回呼階段不正確");
            }
        }
        for(;;){
            if(op.step==DumpStep::CancelStart){if(op.callback && op.cancel)return callback(6);op.step=DumpStep::IncludeThread;}
            else if(op.step==DumpStep::IncludeThread){if(op.index>=snapshot.threads.size()){op.index=0;op.step=DumpStep::IncludeModule;continue;}if(op.callback)return callback(3,snapshot.threads[op.index].flags);op.step=DumpStep::Thread;}
            else if(op.step==DumpStep::Thread){if(op.callback)return callback(1,snapshot.threads[op.index].flags);++op.index;op.step=DumpStep::IncludeThread;}
            else if(op.step==DumpStep::IncludeModule){if(op.index>=snapshot.modules.size()){gather();op.index=0;op.step=(snapshot.flags&2)?DumpStep::Vm:DumpStep::Memory;continue;}if(op.callback)return callback(4,snapshot.modules[op.index].flags);op.step=DumpStep::Module;}
            else if(op.step==DumpStep::Module){if(op.callback)return callback(0,snapshot.modules[op.index].flags);++op.index;op.step=DumpStep::IncludeModule;}
            else if(op.step==DumpStep::Memory){if(op.callback)return callback(5);op.step=DumpStep::Remove;}
            else if(op.step==DumpStep::Remove){if(op.callback)return callback(9);op.step=DumpStep::CancelEnd;}
            else if(op.step==DumpStep::Vm){if(op.callback && op.index<snapshot.memory.size())return callback(10);op.step=DumpStep::CancelEnd;}
            else if(op.step==DumpStep::CancelEnd){
                if(op.encoded.empty()){
                    snapshot.memory.erase(std::remove_if(snapshot.memory.begin(),snapshot.memory.end(),[](const auto& region){return region.bytes.empty();}),snapshot.memory.end());op.encoded=encode_minidump(snapshot);
                }
                if(op.callback && op.cancel)return callback(6);op.step=DumpStep::IoStart;
            }else if(op.step==DumpStep::IoStart){if(op.callback)return callback(11);op.step=DumpStep::Write;}
            else if(op.step==DumpStep::Write){
                if(op.written>=op.encoded.size()){op.step=op.alternate?DumpStep::IoFinish:DumpStep::Done;continue;}
                if(op.alternate)return callback(12);
                if(!runtime_.network.external_io)throw DumpFailure{50};const auto file=files_.find(op.file);if(file==files_.end())throw DumpFailure{6};auto& target=file->second;
                if(!target.native || !(target.access&(0x40000000U|2U)))throw DumpFailure{5};if(target.pipe || target.serial || target.hid || target.metadata.attributes&16)throw DumpFailure{50};
                const auto count=std::min<std::size_t>(io_chunk,op.encoded.size()-op.written);const auto position_guard=runtime_.win_file_positions.at(target.object).lock();const auto io=target.native->write(std::span<const std::uint8_t>{op.encoded}.subspan(op.written,count),op.written);op.written+=static_cast<std::size_t>(io.bytes);runtime_.win_file_positions.at(target.object)=op.written;
                if(io.error)throw DumpFailure{io.error};if(io.bytes!=count)throw DumpFailure{29};if(op.written==op.encoded.size()){const auto error=target.native->truncate(op.written);if(error)throw DumpFailure{error};target.metadata=target.native->information().metadata;}
            }else if(op.step==DumpStep::IoFinish)return callback(13);
            else return finish(true);
        }
    }catch(const DumpFailure& error){return finish(false,error.error);}catch(const MemoryFault&){return finish(false,299);}catch(const std::bad_alloc&){return finish(false,8);}catch(const std::length_error&){return finish(false,8);}
}
}

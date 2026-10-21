#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>
#if defined(__APPLE__)
#define CL_SILENCE_DEPRECATION
#include <OpenCL/opencl.h>
#endif

namespace x64arm {
// 內建 OpenCL（OpenCL.dll／Khronos ICD）：物件首欄為 ICD 派送表，Khronos 載入器與直接呼叫共用。
// 宿主對應 macOS OpenCL.framework；客體指標與宿主不同址，緩衝區、影像與參數經同步複製轉換。
// 派送表順序取自 Khronos cl_icd.h（並以 Geekbench 隨附載入器的各匯出偏移交叉驗證）。
// 參考：https://registry.khronos.org/OpenCL/specs/3.0-unified/html/OpenCL_API.html
struct OpenclState {
    enum class Kind {Platform,Device,Context,Queue,Mem,Sampler,Program,Kernel,Event};
    // host_row／host_slice：USE_HOST_PTR 影像建立時的客體 pitch，映射時直接回傳客體位址。
    struct Object {Kind kind{};void* host{};unsigned references{1};bool permanent{};std::uint64_t host_pointer{};std::size_t host_row{},host_slice{};std::vector<std::pair<std::uint64_t,std::uint64_t>> destructors;};
    // 影像映射只複製區域內的列：row／slice 為宿主 pitch，line 為每列有效位元組，rows／slices 為區域大小。
    struct Mapping {std::uint64_t memory{};void* host{};std::size_t size{};std::uint64_t flags{};bool owned{};bool image{};std::size_t row{},slice{},line{},rows{},slices{},guest_row{},guest_slice{};};
    std::map<std::uint64_t,Object> objects;
    std::map<void*,std::uint64_t> reverse;
    std::map<std::uint64_t,Mapping> mappings;
    std::uint64_t dispatch{};
};
namespace {
constexpr std::int32_t success=0,out_of_host_memory=-6,invalid_value=-30,invalid_platform=-32,invalid_device=-33,invalid_context=-34,
    invalid_queue=-36,invalid_mem=-38,invalid_sampler=-41,invalid_program=-44,invalid_kernel=-48,invalid_wait_list=-57,invalid_event=-58,
    invalid_operation=-59,invalid_property=-64,platform_not_found=-1001;
// ICD 派送表（0–148）；空字串為未提供的 GL／D3D／EGL 共享擴充槽。
constexpr std::array<std::string_view,149> dispatch_names{
    "clGetPlatformIDs","clGetPlatformInfo","clGetDeviceIDs","clGetDeviceInfo","clCreateContext","clCreateContextFromType","clRetainContext","clReleaseContext",
    "clGetContextInfo","clCreateCommandQueue","clRetainCommandQueue","clReleaseCommandQueue","clGetCommandQueueInfo","clSetCommandQueueProperty","clCreateBuffer","clCreateImage2D",
    "clCreateImage3D","clRetainMemObject","clReleaseMemObject","clGetSupportedImageFormats","clGetMemObjectInfo","clGetImageInfo","clCreateSampler","clRetainSampler",
    "clReleaseSampler","clGetSamplerInfo","clCreateProgramWithSource","clCreateProgramWithBinary","clRetainProgram","clReleaseProgram","clBuildProgram","clUnloadCompiler",
    "clGetProgramInfo","clGetProgramBuildInfo","clCreateKernel","clCreateKernelsInProgram","clRetainKernel","clReleaseKernel","clSetKernelArg","clGetKernelInfo",
    "clGetKernelWorkGroupInfo","clWaitForEvents","clGetEventInfo","clRetainEvent","clReleaseEvent","clGetEventProfilingInfo","clFlush","clFinish",
    "clEnqueueReadBuffer","clEnqueueWriteBuffer","clEnqueueCopyBuffer","clEnqueueReadImage","clEnqueueWriteImage","clEnqueueCopyImage","clEnqueueCopyImageToBuffer","clEnqueueCopyBufferToImage",
    "clEnqueueMapBuffer","clEnqueueMapImage","clEnqueueUnmapMemObject","clEnqueueNDRangeKernel","clEnqueueTask","clEnqueueNativeKernel","clEnqueueMarker","clEnqueueWaitForEvents",
    "clEnqueueBarrier","clGetExtensionFunctionAddress","","","","","","",
    "","","","","","","","",
    "","clSetEventCallback","clCreateSubBuffer","clSetMemObjectDestructorCallback","clCreateUserEvent","clSetUserEventStatus","clEnqueueReadBufferRect","clEnqueueWriteBufferRect",
    "clEnqueueCopyBufferRect","","","","","clCreateSubDevices","clRetainDevice","clReleaseDevice",
    "clCreateImage","clCreateProgramWithBuiltInKernels","clCompileProgram","clLinkProgram","clUnloadPlatformCompiler","clGetKernelArgInfo","clEnqueueFillBuffer","clEnqueueFillImage",
    "clEnqueueMigrateMemObjects","clEnqueueMarkerWithWaitList","clEnqueueBarrierWithWaitList","clGetExtensionFunctionAddressForPlatform","","","","",
    "","","","","","","","",
    "","","","clCreateCommandQueueWithProperties","clCreatePipe","clGetPipeInfo","clSVMAlloc","clSVMFree",
    "clEnqueueSVMFree","clEnqueueSVMMemcpy","clEnqueueSVMMemFill","clEnqueueSVMMap","clEnqueueSVMUnmap","clCreateSamplerWithProperties","clSetKernelArgSVMPointer","clSetKernelExecInfo",
    "","clCloneKernel","clCreateProgramWithIL","clEnqueueSVMMigrateMem","clGetDeviceAndHostTimer","clGetHostTimer","clGetKernelSubGroupInfo","clSetDefaultDeviceCommandQueue",
    "clSetProgramReleaseCallback","clSetProgramSpecializationConstant","clCreateBufferWithProperties","clCreateImageWithProperties","clSetContextDestructorCallback",
};
}
std::span<const WindowsServiceDescriptor> opencl_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"opencl.dll","clGetPlatformIDs",3},{"opencl.dll","clIcdGetPlatformIDsKHR",3},{"opencl.dll","clGetPlatformInfo",5},{"opencl.dll","clGetDeviceIDs",5},
        {"opencl.dll","clGetDeviceInfo",5},{"opencl.dll","clCreateContext",6},{"opencl.dll","clCreateContextFromType",5},{"opencl.dll","clRetainContext",1},
        {"opencl.dll","clReleaseContext",1},{"opencl.dll","clGetContextInfo",5},{"opencl.dll","clCreateCommandQueue",4},{"opencl.dll","clCreateCommandQueueWithProperties",4},
        {"opencl.dll","clRetainCommandQueue",1},{"opencl.dll","clReleaseCommandQueue",1},{"opencl.dll","clGetCommandQueueInfo",5},{"opencl.dll","clSetCommandQueueProperty",4},
        {"opencl.dll","clCreateBuffer",5},{"opencl.dll","clCreateBufferWithProperties",6},{"opencl.dll","clCreateSubBuffer",5},{"opencl.dll","clCreateImage",6},
        {"opencl.dll","clCreateImageWithProperties",7},{"opencl.dll","clCreateImage2D",8},{"opencl.dll","clCreateImage3D",10},{"opencl.dll","clRetainMemObject",1},
        {"opencl.dll","clReleaseMemObject",1},{"opencl.dll","clGetSupportedImageFormats",6},{"opencl.dll","clGetMemObjectInfo",5},{"opencl.dll","clGetImageInfo",5},
        {"opencl.dll","clSetMemObjectDestructorCallback",3},{"opencl.dll","clCreateSampler",5},{"opencl.dll","clCreateSamplerWithProperties",3},{"opencl.dll","clRetainSampler",1},
        {"opencl.dll","clReleaseSampler",1},{"opencl.dll","clGetSamplerInfo",5},{"opencl.dll","clCreateProgramWithSource",5},{"opencl.dll","clCreateProgramWithBinary",7},
        {"opencl.dll","clCreateProgramWithBuiltInKernels",5},{"opencl.dll","clCreateProgramWithIL",4},{"opencl.dll","clRetainProgram",1},{"opencl.dll","clReleaseProgram",1},
        {"opencl.dll","clBuildProgram",6},{"opencl.dll","clCompileProgram",9},{"opencl.dll","clLinkProgram",9},{"opencl.dll","clUnloadCompiler",0},
        {"opencl.dll","clUnloadPlatformCompiler",1},{"opencl.dll","clGetProgramInfo",5},{"opencl.dll","clGetProgramBuildInfo",6},{"opencl.dll","clCreateKernel",3},
        {"opencl.dll","clCreateKernelsInProgram",4},{"opencl.dll","clCloneKernel",2},{"opencl.dll","clRetainKernel",1},{"opencl.dll","clReleaseKernel",1},
        {"opencl.dll","clSetKernelArg",4},{"opencl.dll","clGetKernelInfo",5},{"opencl.dll","clGetKernelArgInfo",6},{"opencl.dll","clGetKernelWorkGroupInfo",6},
        {"opencl.dll","clWaitForEvents",2},{"opencl.dll","clGetEventInfo",5},{"opencl.dll","clCreateUserEvent",2},{"opencl.dll","clSetUserEventStatus",2},
        {"opencl.dll","clSetEventCallback",4},{"opencl.dll","clRetainEvent",1},{"opencl.dll","clReleaseEvent",1},{"opencl.dll","clGetEventProfilingInfo",5},
        {"opencl.dll","clFlush",1},{"opencl.dll","clFinish",1},{"opencl.dll","clEnqueueReadBuffer",9},{"opencl.dll","clEnqueueWriteBuffer",9},
        {"opencl.dll","clEnqueueReadBufferRect",14},{"opencl.dll","clEnqueueWriteBufferRect",14},{"opencl.dll","clEnqueueCopyBuffer",9},{"opencl.dll","clEnqueueCopyBufferRect",13},
        {"opencl.dll","clEnqueueFillBuffer",9},{"opencl.dll","clEnqueueReadImage",11},{"opencl.dll","clEnqueueWriteImage",11},{"opencl.dll","clEnqueueCopyImage",9},
        {"opencl.dll","clEnqueueFillImage",8},{"opencl.dll","clEnqueueCopyImageToBuffer",9},{"opencl.dll","clEnqueueCopyBufferToImage",9},{"opencl.dll","clEnqueueMapBuffer",10},
        {"opencl.dll","clEnqueueMapImage",12},{"opencl.dll","clEnqueueUnmapMemObject",6},{"opencl.dll","clEnqueueMigrateMemObjects",7},{"opencl.dll","clEnqueueNDRangeKernel",9},
        {"opencl.dll","clEnqueueTask",5},{"opencl.dll","clEnqueueNativeKernel",10},{"opencl.dll","clEnqueueMarker",2},{"opencl.dll","clEnqueueMarkerWithWaitList",4},
        {"opencl.dll","clEnqueueWaitForEvents",3},{"opencl.dll","clEnqueueBarrier",1},{"opencl.dll","clEnqueueBarrierWithWaitList",4},{"opencl.dll","clGetExtensionFunctionAddress",1},
        {"opencl.dll","clGetExtensionFunctionAddressForPlatform",2},{"opencl.dll","clCreateSubDevices",5},{"opencl.dll","clRetainDevice",1},{"opencl.dll","clReleaseDevice",1},
        {"opencl.dll","clCreatePipe",6},{"opencl.dll","clGetPipeInfo",5},{"opencl.dll","clSVMAlloc",4},{"opencl.dll","clSVMFree",2},
        {"opencl.dll","clEnqueueSVMFree",8},{"opencl.dll","clEnqueueSVMMemcpy",8},{"opencl.dll","clEnqueueSVMMemFill",8},{"opencl.dll","clEnqueueSVMMap",8},
        {"opencl.dll","clEnqueueSVMUnmap",5},{"opencl.dll","clEnqueueSVMMigrateMem",8},{"opencl.dll","clSetKernelArgSVMPointer",3},{"opencl.dll","clSetKernelExecInfo",4},
        {"opencl.dll","clGetDeviceAndHostTimer",3},{"opencl.dll","clGetHostTimer",2},{"opencl.dll","clGetKernelSubGroupInfo",8},{"opencl.dll","clSetDefaultDeviceCommandQueue",3},
        {"opencl.dll","clSetProgramReleaseCallback",3},{"opencl.dll","clSetProgramSpecializationConstant",4},{"opencl.dll","clSetContextDestructorCallback",3},
        {"internal-opencl","Unsupported",0},
    };
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_opencl(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const std::string_view name=service.symbol;
    // X64ARM_TRACE_OPENCL：記錄非成功回傳、建置選項與各核心首次派送，供診斷工作負載。
    static const bool trace=std::getenv("X64ARM_TRACE_OPENCL")!=nullptr;
    struct KernelStat {std::uint64_t count{},micros{},errors{};std::array<std::size_t,3> global{};};
    static std::map<std::string,KernelStat> kernel_stats;static std::map<std::string,std::uint64_t> call_counts;
    static auto reported=std::chrono::steady_clock::now();
    if(trace){
        ++call_counts[std::string(name)];
        if(std::chrono::steady_clock::now()-reported>std::chrono::seconds(10)){
            reported=std::chrono::steady_clock::now();std::string calls;
            for(const auto& [call,count]:call_counts)calls+=" "+call+"="+std::to_string(count);
            std::fprintf(stderr,"[opencl] calls%s\n",calls.c_str());
            for(const auto& [kernel_name,value]:kernel_stats)std::fprintf(stderr,"[opencl] stat %s count=%llu us=%llu errors=%llu global=%zux%zux%zu\n",kernel_name.c_str(),
                static_cast<unsigned long long>(value.count),static_cast<unsigned long long>(value.micros),static_cast<unsigned long long>(value.errors),value.global[0],value.global[1],value.global[2]);
            call_counts.clear();kernel_stats.clear();
        }
    }
    const auto status=[&](std::int32_t value){
        if(trace && value!=0)std::fprintf(stderr,"[opencl] %.*s -> %d\n",static_cast<int>(name.size()),name.data(),value);
        arg.return_to(state,static_cast<std::uint32_t>(value));return std::optional<std::uint32_t>{};
    };
#if !defined(__APPLE__)
    (void)memory;return status(name=="clGetPlatformIDs" || name=="clIcdGetPlatformIDsKHR"?platform_not_found:invalid_operation);
#else
    using Kind=OpenclState::Kind;
    // 目前只提供 x64 客體；Win32 OpenCL 以 stdcall 與 4-byte 指標另需轉換。
    if(x86_)return status(name=="clGetPlatformIDs" || name=="clIcdGetPlatformIDsKHR"?platform_not_found:invalid_operation);
    if(!opencl_)opencl_=std::make_shared<OpenclState>();
    auto& bridge=*opencl_;
    const auto store32=[&](std::uint64_t at,std::int64_t value){if(at)memory.store(GuestAddress{at},32,static_cast<std::uint32_t>(value));};
    const auto store64=[&](std::uint64_t at,std::uint64_t value){if(at)memory.store(GuestAddress{at},64,value);};
    const auto pointer=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto read_bytes=[&](std::uint64_t at,std::size_t size){std::vector<std::uint8_t> bytes(size);if(size)memory.read(GuestAddress{at},bytes);return bytes;};
    const auto read_words=[&](std::uint64_t at,std::size_t count){std::vector<std::uint64_t> words(count);for(std::size_t i=0;i<count;++i)words[i]=memory.load(GuestAddress{at+i*8},64);return words;};
    const auto read_string=[&](std::uint64_t at){std::string text;if(!at)return text;for(std::uint64_t i=0;i<(UINT64_C(1)<<26);++i){const auto c=static_cast<char>(memory.load(GuestAddress{at+i},8));if(!c)break;text.push_back(c);}return text;};
    if(!bridge.dispatch){
        const auto address=runtime_allocate(memory,{},dispatch_names.size()*8);
        const auto unsupported=resolve("internal-opencl","Unsupported").value;
        for(std::size_t i=0;i<dispatch_names.size();++i)memory.store(GuestAddress{address+i*8},64,dispatch_names[i].empty()?unsupported:resolve("opencl.dll",dispatch_names[i]).value);
        bridge.dispatch=address;
    }
    // 宿主物件轉為客體 ICD 物件：[派送表][保留]；同一宿主物件只對應一個客體指標。
    const auto wrap=[&](Kind kind,void* host,bool permanent=false)->std::uint64_t{
        if(!host)return 0;
        if(const auto found=bridge.reverse.find(host);found!=bridge.reverse.end())return found->second;
        const auto block=allocate_heap(memory,16);if(!block)return 0;
        memory.store(GuestAddress{block},64,bridge.dispatch);memory.store(GuestAddress{block+8},64,0);
        OpenclState::Object object;object.kind=kind;object.host=host;object.permanent=permanent;
        bridge.objects.emplace(block,std::move(object));bridge.reverse.emplace(host,block);return block;
    };
    const auto lookup=[&](std::uint64_t guest,Kind kind)->void*{
        const auto found=bridge.objects.find(guest);return found==bridge.objects.end() || found->second.kind!=kind?nullptr:found->second.host;
    };
    const auto guest_handle=[&](Kind kind,void* host){return wrap(kind,host,kind==Kind::Platform || kind==Kind::Device);};
    const auto event_list=[&](std::uint64_t raw_count,std::uint64_t list,std::vector<cl_event>& events)->bool{
        const auto count=static_cast<std::uint32_t>(raw_count);
        if((count && !list) || (!count && list) || count>65536)return false;
        for(const auto handle:read_words(list,static_cast<std::size_t>(count))){const auto event=static_cast<cl_event>(lookup(handle,Kind::Event));if(!event)return false;events.push_back(event);}
        return true;
    };
    const auto give_event=[&](std::uint64_t out,cl_event event){if(out)store64(out,guest_handle(Kind::Event,event));else if(event)clReleaseEvent(event);};
    // 參照計數：客體 Retain／Release 同步轉給宿主；計數歸零時移除對應並釋放客體物件。
    const auto retain=[&](Kind kind,std::int32_t error,auto host_retain){
        const auto found=bridge.objects.find(arg(0));if(found==bridge.objects.end() || found->second.kind!=kind)return status(error);
        if(found->second.permanent)return status(success);
        const auto result=host_retain(found->second.host);if(result==success)++found->second.references;return status(result);
    };
    const auto release=[&](Kind kind,std::int32_t error,auto host_release){
        const auto found=bridge.objects.find(arg(0));if(found==bridge.objects.end() || found->second.kind!=kind)return status(error);
        if(found->second.permanent)return status(success);
        const auto result=host_release(found->second.host);if(result!=success)return status(result);
        if(--found->second.references)return status(success);
        const auto handle=found->first;auto destructors=std::move(found->second.destructors);
        bridge.reverse.erase(found->second.host);bridge.objects.erase(found);(void)free_heap(memory,handle,state);
        if(destructors.empty())return status(success);
        // 記憶體物件解構回呼依登錄的反向順序呼叫。
        std::vector<GuestCallback> callbacks;for(auto it=destructors.rbegin();it!=destructors.rend();++it)callbacks.push_back({it->first,{handle,it->second}});
        start_callbacks(state,memory,std::move(callbacks),Continuation::Return,0);return std::optional<std::uint32_t>{};
    };
    // Get*Info：先取得宿主結果，再轉換其中的物件與指標，最後依客體緩衝區大小寫回。
    enum class Translate {None,Platform,Device,Context,Queue,Mem,Program,ContextProperties,HostPointer,Binaries};
    const auto info=[&](auto&& query,std::uint64_t param,std::uint64_t size,std::uint64_t value,std::uint64_t size_out,Translate translate,std::uint64_t guest_owner=0)->std::int32_t{
        std::size_t needed=0;auto error=query(static_cast<cl_uint>(param),0,nullptr,&needed);
        if(error!=success){if(trace)std::fprintf(stderr,"[opencl] %.*s param=0x%llx\n",static_cast<int>(name.size()),name.data(),static_cast<unsigned long long>(param));return error;}
        std::vector<std::uint8_t> data(needed);if(needed){error=query(static_cast<cl_uint>(param),needed,data.data(),nullptr);if(error!=success)return error;}
        const auto handles=[&](Kind kind){for(std::size_t at=0;at+8<=data.size();at+=8){void* host;std::memcpy(&host,data.data()+at,8);const auto guest=guest_handle(kind,host);std::memcpy(data.data()+at,&guest,8);}};
        switch(translate){
        case Translate::Platform:handles(Kind::Platform);break;
        case Translate::Device:handles(Kind::Device);break;
        case Translate::Context:handles(Kind::Context);break;
        case Translate::Queue:handles(Kind::Queue);break;
        case Translate::Mem:handles(Kind::Mem);break;
        case Translate::Program:handles(Kind::Program);break;
        case Translate::ContextProperties:
            for(std::size_t at=0;at+16<=data.size();at+=16){std::uint64_t key;std::memcpy(&key,data.data()+at,8);if(!key)break;
                if(key==CL_CONTEXT_PLATFORM){void* host;std::memcpy(&host,data.data()+at+8,8);const auto guest=guest_handle(Kind::Platform,host);std::memcpy(data.data()+at+8,&guest,8);}}
            break;
        case Translate::HostPointer:{const auto found=bridge.objects.find(guest_owner);const std::uint64_t guest=found==bridge.objects.end()?0:found->second.host_pointer;data.assign(8,0);std::memcpy(data.data(),&guest,8);break;}
        case Translate::Binaries:{
            // CL_PROGRAM_BINARIES：客體提供每個裝置的輸出緩衝區指標陣列。
            if(!value)break;if(size<data.size())return invalid_value;
            const auto count=data.size()/8;const auto targets=read_words(value,count);
            std::vector<std::size_t> sizes(count);
            if(query(CL_PROGRAM_BINARY_SIZES,count*sizeof(std::size_t),sizes.data(),nullptr)!=success)return invalid_value;
            std::vector<std::vector<unsigned char>> binaries(count);std::vector<unsigned char*> pointers(count);
            for(std::size_t i=0;i<count;++i){binaries[i].resize(sizes[i]);pointers[i]=targets[i]?binaries[i].data():nullptr;}
            error=query(CL_PROGRAM_BINARIES,count*sizeof(void*),pointers.data(),nullptr);if(error!=success)return error;
            for(std::size_t i=0;i<count;++i)if(targets[i])memory.write(GuestAddress{targets[i]},binaries[i]);
            store64(size_out,data.size());return success;
        }
        case Translate::None:break;
        }
        if(value){if(size<data.size())return invalid_value;memory.write(GuestAddress{value},data);}
        store64(size_out,data.size());return success;
    };
    if(name=="Unsupported")return status(invalid_operation);
    // X64ARM_TRACE_OPENCL_ARGS：逐次記錄記憶體物件建立、映射與讀寫參數。
    static const bool trace_args=trace && std::getenv("X64ARM_TRACE_OPENCL_ARGS")!=nullptr;
    if(trace_args){
        if(name=="clCreateBuffer")std::fprintf(stderr,"[opencl] call clCreateBuffer flags=%llx size=%llu host=%llx\n",static_cast<unsigned long long>(arg(1)),static_cast<unsigned long long>(arg(2)),static_cast<unsigned long long>(arg(3)));
        else if(name=="clCreateImage2D")std::fprintf(stderr,"[opencl] call clCreateImage2D flags=%llx format=%llx,%llx size=%llux%llu pitch=%llu host=%llx\n",static_cast<unsigned long long>(arg(1)),
            static_cast<unsigned long long>(arg(2)?memory.load(GuestAddress{arg(2)},32):0),static_cast<unsigned long long>(arg(2)?memory.load(GuestAddress{arg(2)+4},32):0),
            static_cast<unsigned long long>(arg(3)),static_cast<unsigned long long>(arg(4)),static_cast<unsigned long long>(arg(5)),static_cast<unsigned long long>(arg(6)));
        else if(name=="clEnqueueMapImage" || name=="clEnqueueMapBuffer"){
            const bool image_map=name=="clEnqueueMapImage";
            if(image_map)std::fprintf(stderr,"[opencl] call clEnqueueMapImage mem=%llx flags=%llx origin=%llu,%llu region=%llux%llu\n",static_cast<unsigned long long>(arg(1)),static_cast<unsigned long long>(arg(3)),
                static_cast<unsigned long long>(memory.load(GuestAddress{arg(4)},64)),static_cast<unsigned long long>(memory.load(GuestAddress{arg(4)+8},64)),
                static_cast<unsigned long long>(memory.load(GuestAddress{arg(5)},64)),static_cast<unsigned long long>(memory.load(GuestAddress{arg(5)+8},64)));
            else std::fprintf(stderr,"[opencl] call clEnqueueMapBuffer mem=%llx flags=%llx offset=%llu size=%llu\n",static_cast<unsigned long long>(arg(1)),static_cast<unsigned long long>(arg(3)),static_cast<unsigned long long>(arg(4)),static_cast<unsigned long long>(arg(5)));
        }
        else if(name=="clEnqueueReadBuffer" || name=="clEnqueueWriteBuffer")std::fprintf(stderr,"[opencl] call %.*s mem=%llx blocking=%u offset=%llu size=%llu ptr=%llx\n",static_cast<int>(name.size()),name.data(),
            static_cast<unsigned long long>(arg(1)),static_cast<unsigned>(static_cast<std::uint32_t>(arg(2))),static_cast<unsigned long long>(arg(3)),static_cast<unsigned long long>(arg(4)),static_cast<unsigned long long>(arg(5)));
    }
    if(name=="clGetPlatformIDs" || name=="clIcdGetPlatformIDsKHR"){
        const auto count=static_cast<cl_uint>(arg(0));
        if((!count && arg(1)) || (!arg(1) && !arg(2)))return status(invalid_value);
        cl_uint available=0;if(clGetPlatformIDs(0,nullptr,&available)!=success || !available){store32(arg(2),0);return status(platform_not_found);}
        std::vector<cl_platform_id> platforms(available);clGetPlatformIDs(available,platforms.data(),nullptr);
        if(arg(1))for(cl_uint i=0;i<std::min(count,available);++i)store64(arg(1)+i*8,guest_handle(Kind::Platform,platforms[i]));
        store32(arg(2),available);return status(success);
    }
    if(name=="clGetExtensionFunctionAddress" || name=="clGetExtensionFunctionAddressForPlatform"){
        const auto requested=read_string(arg(name=="clGetExtensionFunctionAddress"?0:1));
        const auto descriptors=opencl_service_descriptors();
        for(const auto& descriptor:descriptors)if(descriptor.family=="opencl.dll" && descriptor.symbol==requested && requested.ends_with("KHR"))return pointer(resolve("opencl.dll",requested).value);
        return pointer(0);
    }
    if(name=="clGetPlatformInfo"){
        const auto platform=static_cast<cl_platform_id>(lookup(arg(0),Kind::Platform));if(!platform)return status(invalid_platform);
        const auto param=static_cast<cl_uint>(arg(1));
        // Khronos ICD 協定所需的 cl_khr_icd 與字尾；宿主 Apple 平台沒有這兩項。
        if(param==0x0920 || param==CL_PLATFORM_EXTENSIONS){
            std::string text;
            if(param==0x0920)text="x64arm";
            else {std::size_t length=0;clGetPlatformInfo(platform,param,0,nullptr,&length);std::string host(length,'\0');clGetPlatformInfo(platform,param,length,host.data(),nullptr);
                text=host.c_str();if(text.find("cl_khr_icd")==std::string::npos)text+=text.empty()?"cl_khr_icd":" cl_khr_icd";}
            const auto bytes=std::vector<std::uint8_t>(text.begin(),text.end()+0);std::vector<std::uint8_t> data(bytes);data.push_back(0);
            if(arg(3)){if(arg(2)<data.size())return status(invalid_value);memory.write(GuestAddress{arg(3)},data);}
            store64(arg(4),data.size());return status(success);
        }
        return status(info([&](cl_uint p,std::size_t s,void* v,std::size_t* r){return clGetPlatformInfo(platform,p,s,v,r);},param,arg(2),arg(3),arg(4),Translate::None));
    }
    if(name=="clGetDeviceIDs"){
        const auto platform=static_cast<cl_platform_id>(lookup(arg(0),Kind::Platform));if(arg(0) && !platform)return status(invalid_platform);
        const auto count=static_cast<cl_uint>(arg(2));if((!count && arg(3)) || (!arg(3) && !arg(4)))return status(invalid_value);
        cl_uint available=0;auto error=clGetDeviceIDs(platform,arg(1),0,nullptr,&available);if(error!=success){store32(arg(4),0);return status(error);}
        std::vector<cl_device_id> devices(available);clGetDeviceIDs(platform,arg(1),available,devices.data(),nullptr);
        if(arg(3))for(cl_uint i=0;i<std::min(count,available);++i)store64(arg(3)+i*8,guest_handle(Kind::Device,devices[i]));
        store32(arg(4),available);return status(success);
    }
    if(name=="clGetDeviceInfo"){
        const auto device=static_cast<cl_device_id>(lookup(arg(0),Kind::Device));if(!device)return status(invalid_device);
        const auto param=static_cast<cl_uint>(arg(1));const auto translate=param==CL_DEVICE_PLATFORM?Translate::Platform:param==CL_DEVICE_PARENT_DEVICE?Translate::Device:Translate::None;
        return status(info([&](cl_uint p,std::size_t s,void* v,std::size_t* r){return clGetDeviceInfo(device,p,s,v,r);},param,arg(2),arg(3),arg(4),translate));
    }
    if(name=="clRetainDevice" || name=="clReleaseDevice")return status(lookup(arg(0),Kind::Device)?success:invalid_device);
    if(name=="clCreateSubDevices")return status(invalid_operation);
    // cl_context_properties：0 結尾的 (key,value) 對；CL_CONTEXT_PLATFORM 轉換為宿主平台。
    const auto context_properties=[&](std::uint64_t at,std::vector<cl_context_properties>& out)->bool{
        if(!at)return true;
        for(unsigned i=0;i<64;++i){
            const auto key=memory.load(GuestAddress{at+i*16},64);if(!key){out.push_back(0);return true;}
            const auto value=memory.load(GuestAddress{at+i*16+8},64);
            if(key==CL_CONTEXT_PLATFORM){const auto platform=lookup(value,Kind::Platform);if(!platform)return false;out.push_back(static_cast<cl_context_properties>(key));out.push_back(reinterpret_cast<cl_context_properties>(platform));}
            else if(key==0x1085){out.push_back(static_cast<cl_context_properties>(key));out.push_back(static_cast<cl_context_properties>(value));} // CL_CONTEXT_INTEROP_USER_SYNC。
            else return false;
        }
        return false;
    };
    if(name=="clCreateContext" || name=="clCreateContextFromType"){
        const bool typed=name=="clCreateContextFromType";const auto error_out=arg(typed?4:5);
        std::vector<cl_context_properties> properties;if(!context_properties(arg(0),properties)){store32(error_out,invalid_property);return pointer(0);}
        cl_int error=success;cl_context context=nullptr;
        if(typed)context=clCreateContextFromType(properties.empty()?nullptr:properties.data(),arg(1),nullptr,nullptr,&error);
        else {
            const auto count=static_cast<cl_uint>(arg(1));if(!count || !arg(2)){store32(error_out,invalid_value);return pointer(0);}
            std::vector<cl_device_id> devices;for(const auto handle:read_words(arg(2),count)){const auto device=static_cast<cl_device_id>(lookup(handle,Kind::Device));if(!device){store32(error_out,invalid_device);return pointer(0);}devices.push_back(device);}
            context=clCreateContext(properties.empty()?nullptr:properties.data(),count,devices.data(),nullptr,nullptr,&error);
        }
        store32(error_out,error);return pointer(context?guest_handle(Kind::Context,context):0);
    }
    if(name=="clRetainContext")return retain(Kind::Context,invalid_context,[](void* h){return clRetainContext(static_cast<cl_context>(h));});
    if(name=="clReleaseContext")return release(Kind::Context,invalid_context,[](void* h){return clReleaseContext(static_cast<cl_context>(h));});
    if(name=="clGetContextInfo"){
        const auto context=static_cast<cl_context>(lookup(arg(0),Kind::Context));if(!context)return status(invalid_context);
        const auto param=static_cast<cl_uint>(arg(1));const auto translate=param==CL_CONTEXT_DEVICES?Translate::Device:param==CL_CONTEXT_PROPERTIES?Translate::ContextProperties:Translate::None;
        return status(info([&](cl_uint p,std::size_t s,void* v,std::size_t* r){return clGetContextInfo(context,p,s,v,r);},param,arg(2),arg(3),arg(4),translate));
    }
    if(name=="clSetContextDestructorCallback")return status(invalid_operation);
    if(name=="clCreateCommandQueue" || name=="clCreateCommandQueueWithProperties"){
        const auto error_out=arg(3);const auto context=static_cast<cl_context>(lookup(arg(0),Kind::Context));if(!context){store32(error_out,invalid_context);return pointer(0);}
        const auto device=static_cast<cl_device_id>(lookup(arg(1),Kind::Device));if(!device){store32(error_out,invalid_device);return pointer(0);}
        cl_command_queue_properties properties=0;
        if(name=="clCreateCommandQueue")properties=arg(2);
        else if(arg(2))for(unsigned i=0;i<16;++i){
            const auto key=memory.load(GuestAddress{arg(2)+i*16},64);if(!key)break;const auto value=memory.load(GuestAddress{arg(2)+i*16+8},64);
            if(key==0x1093)properties=value;else if(key!=0x1094){store32(error_out,invalid_value);return pointer(0);} // CL_QUEUE_PROPERTIES／CL_QUEUE_SIZE。
        }
        // OpenCL 1.2 宿主：裝置端佇列（ON_DEVICE）不支援。
        if(properties&~static_cast<cl_command_queue_properties>(3)){store32(error_out,invalid_value);return pointer(0);}
        cl_int error=success;const auto queue=clCreateCommandQueue(context,device,properties,&error);
        store32(error_out,error);return pointer(queue?guest_handle(Kind::Queue,queue):0);
    }
    if(name=="clRetainCommandQueue")return retain(Kind::Queue,invalid_queue,[](void* h){return clRetainCommandQueue(static_cast<cl_command_queue>(h));});
    if(name=="clReleaseCommandQueue")return release(Kind::Queue,invalid_queue,[](void* h){return clReleaseCommandQueue(static_cast<cl_command_queue>(h));});
    if(name=="clGetCommandQueueInfo"){
        const auto queue=static_cast<cl_command_queue>(lookup(arg(0),Kind::Queue));if(!queue)return status(invalid_queue);
        const auto param=static_cast<cl_uint>(arg(1));const auto translate=param==CL_QUEUE_CONTEXT?Translate::Context:param==CL_QUEUE_DEVICE?Translate::Device:Translate::None;
        return status(info([&](cl_uint p,std::size_t s,void* v,std::size_t* r){return clGetCommandQueueInfo(queue,p,s,v,r);},param,arg(2),arg(3),arg(4),translate));
    }
    if(name=="clSetCommandQueueProperty")return status(invalid_operation);
    // 記憶體旗標：USE_HOST_PTR 改為 COPY_HOST_PTR，並記住客體位址供映射與 CL_MEM_HOST_PTR 使用。
    const auto memory_flags=[&](cl_mem_flags flags,std::uint64_t host,std::size_t size,std::vector<std::uint8_t>& copy)->cl_mem_flags{
        if(host && (flags&(CL_MEM_USE_HOST_PTR|CL_MEM_COPY_HOST_PTR))){copy=read_bytes(host,size);flags=(flags&~static_cast<cl_mem_flags>(CL_MEM_USE_HOST_PTR))|CL_MEM_COPY_HOST_PTR;}
        return flags;
    };
    if(name=="clCreateBuffer" || name=="clCreateBufferWithProperties"){
        const unsigned shift=name=="clCreateBuffer"?0:1;const auto error_out=arg(4+shift);
        const auto context=static_cast<cl_context>(lookup(arg(0),Kind::Context));if(!context){store32(error_out,invalid_context);return pointer(0);}
        if(shift && arg(1) && memory.load(GuestAddress{arg(1)},64)){store32(error_out,invalid_property);return pointer(0);}
        const auto flags=arg(1+shift);const auto size=static_cast<std::size_t>(arg(2+shift));const auto host=arg(3+shift);
        if(!size){store32(error_out,-61);return pointer(0);} // CL_INVALID_BUFFER_SIZE。
        if(!host && (flags&(CL_MEM_USE_HOST_PTR|CL_MEM_COPY_HOST_PTR))){store32(error_out,-37);return pointer(0);} // CL_INVALID_HOST_PTR。
        std::vector<std::uint8_t> copy;const auto host_flags=memory_flags(flags,host,size,copy);
        cl_int error=success;const auto buffer=clCreateBuffer(context,host_flags,size,copy.empty()?nullptr:copy.data(),&error);
        store32(error_out,error);if(!buffer)return pointer(0);
        const auto guest=guest_handle(Kind::Mem,buffer);if(flags&CL_MEM_USE_HOST_PTR)bridge.objects.at(guest).host_pointer=host;return pointer(guest);
    }
    if(name=="clCreateSubBuffer"){
        const auto error_out=arg(4);const auto buffer=static_cast<cl_mem>(lookup(arg(0),Kind::Mem));if(!buffer){store32(error_out,invalid_mem);return pointer(0);}
        if(arg(2)!=CL_BUFFER_CREATE_TYPE_REGION || !arg(3)){store32(error_out,invalid_value);return pointer(0);}
        cl_buffer_region region{memory.load(GuestAddress{arg(3)},64),memory.load(GuestAddress{arg(3)+8},64)};
        cl_int error=success;const auto sub=clCreateSubBuffer(buffer,arg(1),CL_BUFFER_CREATE_TYPE_REGION,&region,&error);
        store32(error_out,error);if(!sub)return pointer(0);
        const auto guest=guest_handle(Kind::Mem,sub);const auto parent=bridge.objects.at(arg(0)).host_pointer;if(parent)bridge.objects.at(guest).host_pointer=parent+region.origin;return pointer(guest);
    }
    // cl_image_format（2×uint）與 cl_image_desc 於 Windows x64 與宿主 LP64 版面相同；desc.buffer 需轉換。
    const auto image_size=[&](const cl_image_format& format,const cl_image_desc& desc)->std::size_t{
        const unsigned channels=format.image_channel_order==CL_R || format.image_channel_order==CL_A || format.image_channel_order==CL_INTENSITY || format.image_channel_order==CL_LUMINANCE?1:
            format.image_channel_order==CL_RG || format.image_channel_order==CL_RA?2:format.image_channel_order==CL_RGB?3:4;
        unsigned bytes=1;
        switch(format.image_channel_data_type){case CL_SNORM_INT16:case CL_UNORM_INT16:case CL_SIGNED_INT16:case CL_UNSIGNED_INT16:case CL_HALF_FLOAT:bytes=2;break;
            case CL_SIGNED_INT32:case CL_UNSIGNED_INT32:case CL_FLOAT:bytes=4;break;
            case CL_UNORM_SHORT_565:case CL_UNORM_SHORT_555:bytes=2;break;case CL_UNORM_INT_101010:bytes=4;break;default:bytes=1;break;}
        const std::size_t element=(format.image_channel_data_type==CL_UNORM_SHORT_565 || format.image_channel_data_type==CL_UNORM_SHORT_555 || format.image_channel_data_type==CL_UNORM_INT_101010)?bytes:bytes*channels;
        const auto row=desc.image_row_pitch?desc.image_row_pitch:desc.image_width*element;
        const auto layers=desc.image_type==CL_MEM_OBJECT_IMAGE3D?desc.image_depth:(desc.image_type==CL_MEM_OBJECT_IMAGE1D_ARRAY || desc.image_type==CL_MEM_OBJECT_IMAGE2D_ARRAY)?desc.image_array_size:1;
        const auto height=desc.image_type==CL_MEM_OBJECT_IMAGE1D || desc.image_type==CL_MEM_OBJECT_IMAGE1D_BUFFER || desc.image_type==CL_MEM_OBJECT_IMAGE1D_ARRAY?1:desc.image_height;
        const auto slice=desc.image_slice_pitch?desc.image_slice_pitch:row*height;
        return layers>1 || desc.image_type==CL_MEM_OBJECT_IMAGE3D?slice*layers:row*height;
    };
    if(name=="clCreateImage" || name=="clCreateImageWithProperties" || name=="clCreateImage2D" || name=="clCreateImage3D"){
        const bool legacy2=name=="clCreateImage2D",legacy3=name=="clCreateImage3D";const unsigned shift=name=="clCreateImageWithProperties"?1:0;
        const auto error_out=arg(legacy2?7:legacy3?9:5+shift);
        const auto context=static_cast<cl_context>(lookup(arg(0),Kind::Context));if(!context){store32(error_out,invalid_context);return pointer(0);}
        if(shift && arg(1) && memory.load(GuestAddress{arg(1)},64)){store32(error_out,invalid_property);return pointer(0);}
        const auto flags=arg(1+shift);const auto format_at=arg(2+shift);if(!format_at){store32(error_out,-39);return pointer(0);} // CL_INVALID_IMAGE_FORMAT_DESCRIPTOR。
        cl_image_format format{static_cast<cl_uint>(memory.load(GuestAddress{format_at},32)),static_cast<cl_uint>(memory.load(GuestAddress{format_at+4},32))};
        cl_image_desc desc{};std::uint64_t host=0;
        if(legacy2 || legacy3){
            desc.image_type=legacy2?CL_MEM_OBJECT_IMAGE2D:CL_MEM_OBJECT_IMAGE3D;desc.image_width=arg(3);desc.image_height=arg(4);
            if(legacy2){desc.image_row_pitch=arg(5);host=arg(6);}else {desc.image_depth=arg(5);desc.image_row_pitch=arg(6);desc.image_slice_pitch=arg(7);host=arg(8);}
        }else{
            const auto at=arg(3+shift);if(!at){store32(error_out,-65);return pointer(0);} // CL_INVALID_IMAGE_DESCRIPTOR。
            desc.image_type=static_cast<cl_mem_object_type>(memory.load(GuestAddress{at},32));desc.image_width=memory.load(GuestAddress{at+8},64);desc.image_height=memory.load(GuestAddress{at+16},64);
            desc.image_depth=memory.load(GuestAddress{at+24},64);desc.image_array_size=memory.load(GuestAddress{at+32},64);desc.image_row_pitch=memory.load(GuestAddress{at+40},64);
            desc.image_slice_pitch=memory.load(GuestAddress{at+48},64);desc.num_mip_levels=static_cast<cl_uint>(memory.load(GuestAddress{at+56},32));desc.num_samples=static_cast<cl_uint>(memory.load(GuestAddress{at+60},32));
            if(const auto source=memory.load(GuestAddress{at+64},64)){desc.buffer=static_cast<cl_mem>(lookup(source,Kind::Mem));if(!desc.buffer){store32(error_out,invalid_mem);return pointer(0);}}
            host=arg(4+shift);
        }
        std::vector<std::uint8_t> copy;const auto host_flags=memory_flags(flags,host,host?image_size(format,desc):0,copy);
        cl_int error=success;const auto image=clCreateImage(context,host_flags,&format,&desc,copy.empty()?nullptr:copy.data(),&error);
        store32(error_out,error);if(!image)return pointer(0);
        const auto guest=guest_handle(Kind::Mem,image);
        if(flags&CL_MEM_USE_HOST_PTR){
            auto& entry=bridge.objects.at(guest);entry.host_pointer=host;std::size_t element=0;clGetImageInfo(image,CL_IMAGE_ELEMENT_SIZE,sizeof element,&element,nullptr);
            entry.host_row=desc.image_row_pitch?desc.image_row_pitch:desc.image_width*element;
            const auto height=desc.image_type==CL_MEM_OBJECT_IMAGE1D || desc.image_type==CL_MEM_OBJECT_IMAGE1D_ARRAY || desc.image_type==CL_MEM_OBJECT_IMAGE1D_BUFFER?1:desc.image_height;
            entry.host_slice=desc.image_slice_pitch?desc.image_slice_pitch:entry.host_row*height;
        }
        return pointer(guest);
    }
    if(name=="clRetainMemObject")return retain(Kind::Mem,invalid_mem,[](void* h){return clRetainMemObject(static_cast<cl_mem>(h));});
    if(name=="clReleaseMemObject")return release(Kind::Mem,invalid_mem,[](void* h){return clReleaseMemObject(static_cast<cl_mem>(h));});
    if(name=="clSetMemObjectDestructorCallback"){
        const auto found=bridge.objects.find(arg(0));if(found==bridge.objects.end() || found->second.kind!=Kind::Mem)return status(invalid_mem);
        if(!arg(1))return status(invalid_value);
        if(!handles(GuestAddress{arg(1)}))memory.check(GuestAddress{arg(1)},1,Access::Execute);
        found->second.destructors.push_back({arg(1),arg(2)});return status(success);
    }
    if(name=="clGetSupportedImageFormats"){
        const auto context=static_cast<cl_context>(lookup(arg(0),Kind::Context));if(!context)return status(invalid_context);
        cl_uint available=0;auto error=clGetSupportedImageFormats(context,arg(1),static_cast<cl_mem_object_type>(arg(2)),0,nullptr,&available);if(error!=success)return status(error);
        std::vector<cl_image_format> formats(available);if(available)clGetSupportedImageFormats(context,arg(1),static_cast<cl_mem_object_type>(arg(2)),available,formats.data(),nullptr);
        const auto count=std::min<cl_uint>(available,static_cast<cl_uint>(arg(3)));
        for(cl_uint i=0;arg(4) && i<count;++i){memory.store(GuestAddress{arg(4)+i*8},32,formats[i].image_channel_order);memory.store(GuestAddress{arg(4)+i*8+4},32,formats[i].image_channel_data_type);}
        store32(arg(5),available);return status(success);
    }
    if(name=="clGetMemObjectInfo" || name=="clGetImageInfo"){
        const auto object=static_cast<cl_mem>(lookup(arg(0),Kind::Mem));if(!object)return status(invalid_mem);
        const auto param=static_cast<cl_uint>(arg(1));
        if(name=="clGetImageInfo")return status(info([&](cl_uint p,std::size_t s,void* v,std::size_t* r){return clGetImageInfo(object,p,s,v,r);},param,arg(2),arg(3),arg(4),param==CL_IMAGE_BUFFER?Translate::Mem:Translate::None));
        const auto translate=param==CL_MEM_CONTEXT?Translate::Context:param==CL_MEM_ASSOCIATED_MEMOBJECT?Translate::Mem:param==CL_MEM_HOST_PTR?Translate::HostPointer:Translate::None;
        if(param==CL_MEM_FLAGS){
            // 回報客體原本要求的 USE_HOST_PTR 語意。
            cl_mem_flags flags=0;if(const auto error=clGetMemObjectInfo(object,CL_MEM_FLAGS,sizeof flags,&flags,nullptr);error!=success)return status(error);
            if(bridge.objects.at(arg(0)).host_pointer)flags=(flags&~static_cast<cl_mem_flags>(CL_MEM_COPY_HOST_PTR))|CL_MEM_USE_HOST_PTR;
            if(arg(3)){if(arg(2)<8)return status(invalid_value);store64(arg(3),flags);}store64(arg(4),8);return status(success);
        }
        return status(info([&](cl_uint p,std::size_t s,void* v,std::size_t* r){return clGetMemObjectInfo(object,p,s,v,r);},param,arg(2),arg(3),arg(4),translate,arg(0)));
    }
    if(name=="clCreateSampler" || name=="clCreateSamplerWithProperties"){
        const bool props=name=="clCreateSamplerWithProperties";const auto error_out=arg(props?2:4);
        const auto context=static_cast<cl_context>(lookup(arg(0),Kind::Context));if(!context){store32(error_out,invalid_context);return pointer(0);}
        cl_bool normalized=CL_TRUE;cl_addressing_mode addressing=CL_ADDRESS_CLAMP;cl_filter_mode filter=CL_FILTER_NEAREST;
        if(props){
            if(arg(1))for(unsigned i=0;i<8;++i){const auto key=memory.load(GuestAddress{arg(1)+i*16},64);if(!key)break;const auto value=memory.load(GuestAddress{arg(1)+i*16+8},64);
                if(key==CL_SAMPLER_NORMALIZED_COORDS)normalized=static_cast<cl_bool>(value);else if(key==CL_SAMPLER_ADDRESSING_MODE)addressing=static_cast<cl_addressing_mode>(value);
                else if(key==CL_SAMPLER_FILTER_MODE)filter=static_cast<cl_filter_mode>(value);else {store32(error_out,invalid_value);return pointer(0);}}
        }else {normalized=static_cast<cl_bool>(arg(1));addressing=static_cast<cl_addressing_mode>(arg(2));filter=static_cast<cl_filter_mode>(arg(3));}
        cl_int error=success;const auto sampler=clCreateSampler(context,normalized,addressing,filter,&error);
        store32(error_out,error);return pointer(sampler?guest_handle(Kind::Sampler,sampler):0);
    }
    if(name=="clRetainSampler")return retain(Kind::Sampler,invalid_sampler,[](void* h){return clRetainSampler(static_cast<cl_sampler>(h));});
    if(name=="clReleaseSampler")return release(Kind::Sampler,invalid_sampler,[](void* h){return clReleaseSampler(static_cast<cl_sampler>(h));});
    if(name=="clGetSamplerInfo"){
        const auto sampler=static_cast<cl_sampler>(lookup(arg(0),Kind::Sampler));if(!sampler)return status(invalid_sampler);
        const auto param=static_cast<cl_uint>(arg(1));
        return status(info([&](cl_uint p,std::size_t s,void* v,std::size_t* r){return clGetSamplerInfo(sampler,p,s,v,r);},param,arg(2),arg(3),arg(4),param==CL_SAMPLER_CONTEXT?Translate::Context:Translate::None));
    }
    // 裝置清單：客體 cl_device_id 陣列轉為宿主。
    const auto devices_from=[&](std::uint64_t raw_count,std::uint64_t list,std::vector<cl_device_id>& out)->bool{
        const auto count=static_cast<std::uint32_t>(raw_count);
        if((count && !list) || (!count && list) || count>256)return false;
        for(const auto handle:read_words(list,static_cast<std::size_t>(count))){const auto device=static_cast<cl_device_id>(lookup(handle,Kind::Device));if(!device)return false;out.push_back(device);}
        return true;
    };
    if(name=="clCreateProgramWithSource"){
        const auto error_out=arg(4);const auto context=static_cast<cl_context>(lookup(arg(0),Kind::Context));if(!context){store32(error_out,invalid_context);return pointer(0);}
        const auto count=static_cast<cl_uint>(arg(1));if(!count || !arg(2) || count>65536){store32(error_out,invalid_value);return pointer(0);}
        std::vector<std::string> sources;std::vector<const char*> pointers;std::vector<std::size_t> lengths;
        const auto strings=read_words(arg(2),count);
        for(cl_uint i=0;i<count;++i){
            if(!strings[i]){store32(error_out,invalid_value);return pointer(0);}
            const auto length=arg(3)?memory.load(GuestAddress{arg(3)+i*8},64):0;
            if(length){const auto bytes=read_bytes(strings[i],static_cast<std::size_t>(length));sources.emplace_back(bytes.begin(),bytes.end());}
            else sources.push_back(read_string(strings[i]));
        }
        for(const auto& source:sources){pointers.push_back(source.data());lengths.push_back(source.size());}
        cl_int error=success;const auto program=clCreateProgramWithSource(context,count,pointers.data(),lengths.data(),&error);
        // X64ARM_TRACE_OPENCL_SOURCES=目錄：保存客體程式原始碼供工作負載診斷。
        if(const char* directory=std::getenv("X64ARM_TRACE_OPENCL_SOURCES");directory && *directory && program){
            static unsigned serial=0;const auto path=std::string(directory)+"/program-"+std::to_string(++serial)+".cl";
            if(auto* file=std::fopen(path.c_str(),"wb")){for(const auto& source:sources)std::fwrite(source.data(),1,source.size(),file);std::fclose(file);}
            if(trace)std::fprintf(stderr,"[opencl] source %s program=%p\n",path.c_str(),static_cast<void*>(program));
        }
        store32(error_out,error);return pointer(program?guest_handle(Kind::Program,program):0);
    }
    if(name=="clCreateProgramWithBinary"){
        const auto error_out=arg(6);const auto context=static_cast<cl_context>(lookup(arg(0),Kind::Context));if(!context){store32(error_out,invalid_context);return pointer(0);}
        std::vector<cl_device_id> devices;if(!devices_from(arg(1),arg(2),devices) || devices.empty() || !arg(3) || !arg(4)){store32(error_out,invalid_value);return pointer(0);}
        std::vector<std::vector<std::uint8_t>> binaries;std::vector<const unsigned char*> pointers;std::vector<std::size_t> lengths;
        const auto addresses=read_words(arg(4),devices.size());
        for(std::size_t i=0;i<devices.size();++i){const auto length=memory.load(GuestAddress{arg(3)+i*8},64);if(!length || !addresses[i] || length>(UINT64_C(1)<<30)){store32(error_out,invalid_value);return pointer(0);}binaries.push_back(read_bytes(addresses[i],static_cast<std::size_t>(length)));}
        for(const auto& binary:binaries){pointers.push_back(binary.data());lengths.push_back(binary.size());}
        std::vector<cl_int> statuses(devices.size());cl_int error=success;
        const auto program=clCreateProgramWithBinary(context,static_cast<cl_uint>(devices.size()),devices.data(),lengths.data(),pointers.data(),statuses.data(),&error);
        if(arg(5))for(std::size_t i=0;i<statuses.size();++i)store32(arg(5)+i*4,statuses[i]);
        store32(error_out,error);return pointer(program?guest_handle(Kind::Program,program):0);
    }
    // 宿主為 OpenCL 1.2：SPIR-V IL 與內建核心不可用。
    if(name=="clCreateProgramWithIL" || name=="clCreateProgramWithBuiltInKernels"){store32(arg(name=="clCreateProgramWithIL"?3:4),invalid_operation);return pointer(0);}
    if(name=="clRetainProgram")return retain(Kind::Program,invalid_program,[](void* h){return clRetainProgram(static_cast<cl_program>(h));});
    if(name=="clReleaseProgram")return release(Kind::Program,invalid_program,[](void* h){return clReleaseProgram(static_cast<cl_program>(h));});
    if(name=="clBuildProgram" || name=="clCompileProgram"){
        const auto program=static_cast<cl_program>(lookup(arg(0),Kind::Program));if(!program)return status(invalid_program);
        std::vector<cl_device_id> devices;if(!devices_from(arg(1),arg(2),devices))return status(invalid_value);
        auto options=read_string(arg(3));cl_int error=success;
        // 診斷開關：比較宿主 fast-math 與嚴格數學對工作負載驗證的影響；預設不改寫客體選項。
        static const bool strict_math=[]{const auto* value=std::getenv("X64ARM_OPENCL_STRIP_FAST_MATH");return value && std::string_view(value)=="1";}();
        if(strict_math)for(const std::string_view flag:{"-cl-fast-relaxed-math","-cl-unsafe-math-optimizations","-cl-finite-math-only","-cl-mad-enable"})
            for(auto at=options.find(flag);at!=std::string::npos;at=options.find(flag))options.erase(at,flag.size());
        if(trace)std::fprintf(stderr,"[opencl] %.*s options=\"%s\"\n",static_cast<int>(name.size()),name.data(),options.c_str());
        const bool build=name=="clBuildProgram";const auto notify=arg(build?4:7),data=arg(build?5:8);
        if(!notify && data)return status(invalid_value);
        if(build)error=clBuildProgram(program,static_cast<cl_uint>(devices.size()),devices.empty()?nullptr:devices.data(),options.c_str(),nullptr,nullptr);
        else {
            const auto headers=static_cast<cl_uint>(arg(4));std::vector<cl_program> inputs;std::vector<std::string> names;std::vector<const char*> name_pointers;
            if((headers && (!arg(5) || !arg(6))) || (!headers && (arg(5) || arg(6))))return status(invalid_value);
            const auto programs=read_words(arg(5),headers),header_names=read_words(arg(6),headers);
            for(cl_uint i=0;i<headers;++i){const auto input=static_cast<cl_program>(lookup(programs[i],Kind::Program));if(!input)return status(invalid_program);inputs.push_back(input);names.push_back(read_string(header_names[i]));}
            for(const auto& item:names)name_pointers.push_back(item.c_str());
            error=clCompileProgram(program,static_cast<cl_uint>(devices.size()),devices.empty()?nullptr:devices.data(),options.c_str(),headers,inputs.empty()?nullptr:inputs.data(),name_pointers.empty()?nullptr:name_pointers.data(),nullptr,nullptr);
        }
        // 宿主建置為同步完成；之後才通知客體。
        if(notify){
            if(!handles(GuestAddress{notify}))memory.check(GuestAddress{notify},1,Access::Execute);
            start_callbacks(state,memory,{{notify,{arg(0),data}}},Continuation::Return,static_cast<std::uint32_t>(error));return std::nullopt;
        }
        return status(error);
    }
    if(name=="clLinkProgram"){
        const auto error_out=arg(8);const auto context=static_cast<cl_context>(lookup(arg(0),Kind::Context));if(!context){store32(error_out,invalid_context);return pointer(0);}
        std::vector<cl_device_id> devices;if(!devices_from(arg(1),arg(2),devices)){store32(error_out,invalid_value);return pointer(0);}
        const auto count=static_cast<cl_uint>(arg(4));if(!count || !arg(5)){store32(error_out,invalid_value);return pointer(0);}
        std::vector<cl_program> inputs;for(const auto handle:read_words(arg(5),count)){const auto input=static_cast<cl_program>(lookup(handle,Kind::Program));if(!input){store32(error_out,invalid_program);return pointer(0);}inputs.push_back(input);}
        if(arg(6)){store32(error_out,invalid_operation);return pointer(0);} // 非同步連結回呼不支援。
        const auto options=read_string(arg(3));cl_int error=success;
        const auto program=clLinkProgram(context,static_cast<cl_uint>(devices.size()),devices.empty()?nullptr:devices.data(),options.c_str(),count,inputs.data(),nullptr,nullptr,&error);
        store32(error_out,error);return pointer(program?guest_handle(Kind::Program,program):0);
    }
    if(name=="clUnloadCompiler")return status(success);
    if(name=="clUnloadPlatformCompiler")return status(lookup(arg(0),Kind::Platform)?success:invalid_platform);
    if(name=="clGetProgramInfo"){
        const auto program=static_cast<cl_program>(lookup(arg(0),Kind::Program));if(!program)return status(invalid_program);
        const auto param=static_cast<cl_uint>(arg(1));const auto translate=param==CL_PROGRAM_CONTEXT?Translate::Context:param==CL_PROGRAM_DEVICES?Translate::Device:param==CL_PROGRAM_BINARIES?Translate::Binaries:Translate::None;
        return status(info([&](cl_uint p,std::size_t s,void* v,std::size_t* r){return clGetProgramInfo(program,p,s,v,r);},param,arg(2),arg(3),arg(4),translate));
    }
    if(name=="clGetProgramBuildInfo"){
        const auto program=static_cast<cl_program>(lookup(arg(0),Kind::Program));if(!program)return status(invalid_program);
        const auto device=static_cast<cl_device_id>(lookup(arg(1),Kind::Device));if(!device)return status(invalid_device);
        return status(info([&](cl_uint p,std::size_t s,void* v,std::size_t* r){return clGetProgramBuildInfo(program,device,p,s,v,r);},arg(2),arg(3),arg(4),arg(5),Translate::None));
    }
    if(name=="clSetProgramReleaseCallback" || name=="clSetProgramSpecializationConstant")return status(invalid_operation);
    if(name=="clCreateKernel"){
        const auto error_out=arg(2);const auto program=static_cast<cl_program>(lookup(arg(0),Kind::Program));if(!program){store32(error_out,invalid_program);return pointer(0);}
        cl_int error=success;const auto kernel=clCreateKernel(program,read_string(arg(1)).c_str(),&error);
        store32(error_out,error);return pointer(kernel?guest_handle(Kind::Kernel,kernel):0);
    }
    if(name=="clCreateKernelsInProgram"){
        const auto program=static_cast<cl_program>(lookup(arg(0),Kind::Program));if(!program)return status(invalid_program);
        cl_uint available=0;auto error=clCreateKernelsInProgram(program,0,nullptr,&available);if(error!=success)return status(error);
        if(arg(2)){
            if(static_cast<cl_uint>(arg(1))<available)return status(invalid_value);
            std::vector<cl_kernel> kernels(available);error=clCreateKernelsInProgram(program,available,kernels.data(),nullptr);if(error!=success)return status(error);
            for(cl_uint i=0;i<available;++i)store64(arg(2)+i*8,guest_handle(Kind::Kernel,kernels[i]));
        }
        store32(arg(3),available);return status(success);
    }
    if(name=="clCloneKernel"){store32(arg(1),invalid_operation);return pointer(0);}
    if(name=="clRetainKernel")return retain(Kind::Kernel,invalid_kernel,[](void* h){return clRetainKernel(static_cast<cl_kernel>(h));});
    if(name=="clReleaseKernel")return release(Kind::Kernel,invalid_kernel,[](void* h){return clReleaseKernel(static_cast<cl_kernel>(h));});
    if(name=="clSetKernelArg"){
        const auto kernel=static_cast<cl_kernel>(lookup(arg(0),Kind::Kernel));if(!kernel)return status(invalid_kernel);
        if(trace && std::getenv("X64ARM_TRACE_OPENCL_ARGS")){
            char function[128]{};clGetKernelInfo(kernel,CL_KERNEL_FUNCTION_NAME,sizeof function-1,function,nullptr);
            const auto size=static_cast<std::size_t>(arg(2));std::uint64_t first=0;if(arg(3) && size)first=memory.load(GuestAddress{arg(3)},static_cast<std::uint8_t>(std::min<std::size_t>(size,8)*8));
            const bool object=size==8 && bridge.objects.contains(first);
            std::fprintf(stderr,"[opencl] arg %s[%u] size=%zu value=%llx object=%d\n",function,static_cast<unsigned>(static_cast<std::uint32_t>(arg(1))),size,static_cast<unsigned long long>(first),object?1:0);
        }
        const auto index=static_cast<cl_uint>(arg(1));const auto size=static_cast<std::size_t>(arg(2));const auto value=arg(3);
        if(!value)return status(clSetKernelArg(kernel,index,size,nullptr)); // __local 記憶體。
        if(size>65536)return status(-51); // CL_INVALID_ARG_SIZE。
        // cl_mem／cl_sampler 參數以客體物件指標傳入；其餘為按值位元組。
        if(size==8){
            const auto handle=memory.load(GuestAddress{value},64);
            if(const auto found=bridge.objects.find(handle);found!=bridge.objects.end() && (found->second.kind==Kind::Mem || found->second.kind==Kind::Sampler || found->second.kind==Kind::Queue)){
                void* host=found->second.host;return status(clSetKernelArg(kernel,index,sizeof host,&host));
            }
            if(!handle){
                // NULL cl_mem 與值 0 的 64-bit 純量在位元上相同，宿主同樣接受。
                std::uint64_t zero=0;return status(clSetKernelArg(kernel,index,size,&zero));
            }
        }
        const auto bytes=read_bytes(value,size);return status(clSetKernelArg(kernel,index,size,bytes.data()));
    }
    if(name=="clGetKernelInfo"){
        const auto kernel=static_cast<cl_kernel>(lookup(arg(0),Kind::Kernel));if(!kernel)return status(invalid_kernel);
        const auto param=static_cast<cl_uint>(arg(1));const auto translate=param==CL_KERNEL_CONTEXT?Translate::Context:param==CL_KERNEL_PROGRAM?Translate::Program:Translate::None;
        return status(info([&](cl_uint p,std::size_t s,void* v,std::size_t* r){return clGetKernelInfo(kernel,p,s,v,r);},param,arg(2),arg(3),arg(4),translate));
    }
    if(name=="clGetKernelArgInfo"){
        const auto kernel=static_cast<cl_kernel>(lookup(arg(0),Kind::Kernel));if(!kernel)return status(invalid_kernel);
        const auto index=static_cast<cl_uint>(arg(1));
        return status(info([&](cl_uint p,std::size_t s,void* v,std::size_t* r){return clGetKernelArgInfo(kernel,index,p,s,v,r);},arg(2),arg(3),arg(4),arg(5),Translate::None));
    }
    if(name=="clGetKernelWorkGroupInfo"){
        const auto kernel=static_cast<cl_kernel>(lookup(arg(0),Kind::Kernel));if(!kernel)return status(invalid_kernel);
        const auto device=arg(1)?static_cast<cl_device_id>(lookup(arg(1),Kind::Device)):nullptr;if(arg(1) && !device)return status(invalid_device);
        return status(info([&](cl_uint p,std::size_t s,void* v,std::size_t* r){return clGetKernelWorkGroupInfo(kernel,device,p,s,v,r);},arg(2),arg(3),arg(4),arg(5),Translate::None));
    }
    if(name=="clGetKernelSubGroupInfo" || name=="clSetKernelArgSVMPointer" || name=="clSetKernelExecInfo")return status(invalid_operation);
    if(name=="clWaitForEvents"){
        std::vector<cl_event> events;if(!static_cast<cl_uint>(arg(0)) || !event_list(arg(0),arg(1),events))return status(invalid_event);
        return status(clWaitForEvents(static_cast<cl_uint>(events.size()),events.data()));
    }
    if(name=="clGetEventInfo"){
        const auto event=static_cast<cl_event>(lookup(arg(0),Kind::Event));if(!event)return status(invalid_event);
        const auto param=static_cast<cl_uint>(arg(1));const auto translate=param==CL_EVENT_COMMAND_QUEUE?Translate::Queue:param==CL_EVENT_CONTEXT?Translate::Context:Translate::None;
        return status(info([&](cl_uint p,std::size_t s,void* v,std::size_t* r){return clGetEventInfo(event,p,s,v,r);},param,arg(2),arg(3),arg(4),translate));
    }
    if(name=="clGetEventProfilingInfo"){
        const auto event=static_cast<cl_event>(lookup(arg(0),Kind::Event));if(!event)return status(invalid_event);
        return status(info([&](cl_uint p,std::size_t s,void* v,std::size_t* r){return clGetEventProfilingInfo(event,p,s,v,r);},arg(1),arg(2),arg(3),arg(4),Translate::None));
    }
    if(name=="clCreateUserEvent"){
        const auto context=static_cast<cl_context>(lookup(arg(0),Kind::Context));if(!context){store32(arg(1),invalid_context);return pointer(0);}
        cl_int error=success;const auto event=clCreateUserEvent(context,&error);store32(arg(1),error);return pointer(event?guest_handle(Kind::Event,event):0);
    }
    if(name=="clSetUserEventStatus"){
        const auto event=static_cast<cl_event>(lookup(arg(0),Kind::Event));if(!event)return status(invalid_event);
        return status(clSetUserEventStatus(event,static_cast<cl_int>(arg(1))));
    }
    if(name=="clSetEventCallback"){
        const auto event=static_cast<cl_event>(lookup(arg(0),Kind::Event));if(!event)return status(invalid_event);
        const auto type=static_cast<cl_int>(arg(1));if(!arg(2) || (type!=CL_COMPLETE && type!=CL_SUBMITTED && type!=CL_RUNNING))return status(invalid_value);
        if(!handles(GuestAddress{arg(2)}))memory.check(GuestAddress{arg(2)},1,Access::Execute);
        // 客體回呼不能從宿主驅動執行緒呼叫：等候事件到達要求狀態後，在呼叫端執行緒同步通知。
        cl_int execution=CL_COMPLETE;
        if(clWaitForEvents(1,&event)!=success || clGetEventInfo(event,CL_EVENT_COMMAND_EXECUTION_STATUS,sizeof execution,&execution,nullptr)!=success)execution=-1;
        start_callbacks(state,memory,{{arg(2),{arg(0),static_cast<std::uint32_t>(execution<0?execution:type),arg(3)}}},Continuation::Return,0);return std::nullopt;
    }
    if(name=="clRetainEvent")return retain(Kind::Event,invalid_event,[](void* h){return clRetainEvent(static_cast<cl_event>(h));});
    if(name=="clReleaseEvent")return release(Kind::Event,invalid_event,[](void* h){return clReleaseEvent(static_cast<cl_event>(h));});
    // 以下皆以 command queue 為首個參數。
    const auto queue=static_cast<cl_command_queue>(lookup(arg(0),Kind::Queue));
    if(name=="clFlush")return status(queue?clFlush(queue):invalid_queue);
    if(name=="clFinish")return status(queue?clFinish(queue):invalid_queue);
    if(name.starts_with("clEnqueue") && !queue){
        if(name=="clEnqueueMapBuffer" || name=="clEnqueueMapImage"){store32(arg(name=="clEnqueueMapBuffer"?9:11),invalid_queue);return pointer(0);}
        return status(invalid_queue);
    }
    // 讀寫客體記憶體的指令一律在宿主同步完成，回傳事件已完成。
    if(name=="clEnqueueReadBuffer" || name=="clEnqueueWriteBuffer"){
        const auto buffer=static_cast<cl_mem>(lookup(arg(1),Kind::Mem));if(!buffer)return status(invalid_mem);
        const auto offset=static_cast<std::size_t>(arg(3)),size=static_cast<std::size_t>(arg(4));if(!arg(5))return status(invalid_value);
        std::vector<cl_event> events;if(!event_list(arg(6),arg(7),events))return status(invalid_wait_list);
        cl_event event=nullptr;cl_int error;
        if(name=="clEnqueueReadBuffer"){
            std::vector<std::uint8_t> data(size);
            error=clEnqueueReadBuffer(queue,buffer,CL_TRUE,offset,size,data.data(),static_cast<cl_uint>(events.size()),events.empty()?nullptr:events.data(),arg(8)?&event:nullptr);
            if(error==success)memory.write(GuestAddress{arg(5)},data);
        }else{
            const auto data=read_bytes(arg(5),size);
            error=clEnqueueWriteBuffer(queue,buffer,CL_TRUE,offset,size,data.data(),static_cast<cl_uint>(events.size()),events.empty()?nullptr:events.data(),arg(8)?&event:nullptr);
        }
        if(error==success)give_event(arg(8),event);return status(error);
    }
    if(name=="clEnqueueReadBufferRect" || name=="clEnqueueWriteBufferRect"){
        const auto buffer=static_cast<cl_mem>(lookup(arg(1),Kind::Mem));if(!buffer)return status(invalid_mem);
        if(!arg(3) || !arg(4) || !arg(5) || !arg(10))return status(invalid_value);
        std::array<std::size_t,3> buffer_origin{},host_origin{},region{};
        for(unsigned i=0;i<3;++i){buffer_origin[i]=memory.load(GuestAddress{arg(3)+i*8},64);host_origin[i]=memory.load(GuestAddress{arg(4)+i*8},64);region[i]=memory.load(GuestAddress{arg(5)+i*8},64);}
        if(!region[0] || !region[1] || !region[2])return status(invalid_value);
        const auto host_row=arg(8)?static_cast<std::size_t>(arg(8)):region[0],host_slice=arg(9)?static_cast<std::size_t>(arg(9)):region[1]*host_row;
        // 宿主暫存區對應客體 [ptr, 最末位元組]，保留客體的 row／slice pitch。
        const auto extent=(host_origin[2]+region[2]-1)*host_slice+(host_origin[1]+region[1]-1)*host_row+host_origin[0]+region[0];
        std::vector<cl_event> events;if(!event_list(arg(11),arg(12),events))return status(invalid_wait_list);
        auto data=read_bytes(arg(10),extent);cl_event event=nullptr;cl_int error;
        if(name=="clEnqueueReadBufferRect"){
            error=clEnqueueReadBufferRect(queue,buffer,CL_TRUE,buffer_origin.data(),host_origin.data(),region.data(),arg(6),arg(7),host_row,host_slice,data.data(),static_cast<cl_uint>(events.size()),events.empty()?nullptr:events.data(),arg(13)?&event:nullptr);
            if(error==success)memory.write(GuestAddress{arg(10)},data);
        }else error=clEnqueueWriteBufferRect(queue,buffer,CL_TRUE,buffer_origin.data(),host_origin.data(),region.data(),arg(6),arg(7),host_row,host_slice,data.data(),static_cast<cl_uint>(events.size()),events.empty()?nullptr:events.data(),arg(13)?&event:nullptr);
        if(error==success)give_event(arg(13),event);return status(error);
    }
    const auto triple=[&](std::uint64_t at,std::array<std::size_t,3>& out){if(!at)return false;for(unsigned i=0;i<3;++i)out[i]=memory.load(GuestAddress{at+i*8},64);return true;};
    if(name=="clEnqueueReadImage" || name=="clEnqueueWriteImage"){
        const auto image=static_cast<cl_mem>(lookup(arg(1),Kind::Mem));if(!image)return status(invalid_mem);
        std::array<std::size_t,3> origin{},region{};if(!triple(arg(3),origin) || !triple(arg(4),region) || !arg(7))return status(invalid_value);
        std::size_t element=0;if(const auto error=clGetImageInfo(image,CL_IMAGE_ELEMENT_SIZE,sizeof element,&element,nullptr);error!=success)return status(error);
        const auto row=arg(5)?static_cast<std::size_t>(arg(5)):region[0]*element,slice=arg(6)?static_cast<std::size_t>(arg(6)):row*region[1];
        const auto extent=region[2]>1?slice*region[2]:row*region[1];
        std::vector<cl_event> events;if(!event_list(arg(8),arg(9),events))return status(invalid_wait_list);
        cl_event event=nullptr;cl_int error;
        if(name=="clEnqueueReadImage"){
            std::vector<std::uint8_t> data(extent);
            error=clEnqueueReadImage(queue,image,CL_TRUE,origin.data(),region.data(),row,region[2]>1?slice:0,data.data(),static_cast<cl_uint>(events.size()),events.empty()?nullptr:events.data(),arg(10)?&event:nullptr);
            if(error==success)memory.write(GuestAddress{arg(7)},data);
        }else{
            const auto data=read_bytes(arg(7),extent);
            error=clEnqueueWriteImage(queue,image,CL_TRUE,origin.data(),region.data(),row,region[2]>1?slice:0,data.data(),static_cast<cl_uint>(events.size()),events.empty()?nullptr:events.data(),arg(10)?&event:nullptr);
        }
        if(error==success)give_event(arg(10),event);return status(error);
    }
    if(name=="clEnqueueCopyBuffer"){
        const auto source=static_cast<cl_mem>(lookup(arg(1),Kind::Mem)),target=static_cast<cl_mem>(lookup(arg(2),Kind::Mem));if(!source || !target)return status(invalid_mem);
        std::vector<cl_event> events;if(!event_list(arg(6),arg(7),events))return status(invalid_wait_list);cl_event event=nullptr;
        const auto error=clEnqueueCopyBuffer(queue,source,target,arg(3),arg(4),arg(5),static_cast<cl_uint>(events.size()),events.empty()?nullptr:events.data(),arg(8)?&event:nullptr);
        if(error==success)give_event(arg(8),event);return status(error);
    }
    if(name=="clEnqueueCopyBufferRect"){
        const auto source=static_cast<cl_mem>(lookup(arg(1),Kind::Mem)),target=static_cast<cl_mem>(lookup(arg(2),Kind::Mem));if(!source || !target)return status(invalid_mem);
        std::array<std::size_t,3> src{},dst{},region{};if(!triple(arg(3),src) || !triple(arg(4),dst) || !triple(arg(5),region))return status(invalid_value);
        std::vector<cl_event> events;if(!event_list(arg(10),arg(11),events))return status(invalid_wait_list);cl_event event=nullptr;
        const auto error=clEnqueueCopyBufferRect(queue,source,target,src.data(),dst.data(),region.data(),arg(6),arg(7),arg(8),arg(9),static_cast<cl_uint>(events.size()),events.empty()?nullptr:events.data(),arg(12)?&event:nullptr);
        if(error==success)give_event(arg(12),event);return status(error);
    }
    if(name=="clEnqueueFillBuffer"){
        const auto buffer=static_cast<cl_mem>(lookup(arg(1),Kind::Mem));if(!buffer)return status(invalid_mem);
        const auto size=static_cast<std::size_t>(arg(3));if(!arg(2) || !size || size>128)return status(invalid_value);
        const auto pattern=read_bytes(arg(2),size);std::vector<cl_event> events;if(!event_list(arg(6),arg(7),events))return status(invalid_wait_list);cl_event event=nullptr;
        const auto error=clEnqueueFillBuffer(queue,buffer,pattern.data(),size,arg(4),arg(5),static_cast<cl_uint>(events.size()),events.empty()?nullptr:events.data(),arg(8)?&event:nullptr);
        if(error==success)give_event(arg(8),event);return status(error);
    }
    if(name=="clEnqueueCopyImage" || name=="clEnqueueCopyImageToBuffer" || name=="clEnqueueCopyBufferToImage"){
        const auto source=static_cast<cl_mem>(lookup(arg(1),Kind::Mem)),target=static_cast<cl_mem>(lookup(arg(2),Kind::Mem));if(!source || !target)return status(invalid_mem);
        std::vector<cl_event> events;if(!event_list(arg(6),arg(7),events))return status(invalid_wait_list);cl_event event=nullptr;
        const auto waits=static_cast<cl_uint>(events.size());const auto* list=events.empty()?nullptr:events.data();cl_event* out=arg(8)?&event:nullptr;cl_int error;
        if(name=="clEnqueueCopyImage"){std::array<std::size_t,3> src{},dst{},region{};if(!triple(arg(3),src) || !triple(arg(4),dst) || !triple(arg(5),region))return status(invalid_value);error=clEnqueueCopyImage(queue,source,target,src.data(),dst.data(),region.data(),waits,list,out);}
        else if(name=="clEnqueueCopyImageToBuffer"){std::array<std::size_t,3> src{},region{};if(!triple(arg(3),src) || !triple(arg(4),region))return status(invalid_value);error=clEnqueueCopyImageToBuffer(queue,source,target,src.data(),region.data(),arg(5),waits,list,out);}
        else {std::array<std::size_t,3> dst{},region{};if(!triple(arg(4),dst) || !triple(arg(5),region))return status(invalid_value);error=clEnqueueCopyBufferToImage(queue,source,target,arg(3),dst.data(),region.data(),waits,list,out);}
        if(error==success)give_event(arg(8),event);return status(error);
    }
    if(name=="clEnqueueFillImage"){
        const auto image=static_cast<cl_mem>(lookup(arg(1),Kind::Mem));if(!image)return status(invalid_mem);
        std::array<std::size_t,3> origin{},region{};if(!arg(2) || !triple(arg(3),origin) || !triple(arg(4),region))return status(invalid_value);
        const auto color=read_bytes(arg(2),16);std::vector<cl_event> events;if(!event_list(arg(5),arg(6),events))return status(invalid_wait_list);cl_event event=nullptr;
        const auto error=clEnqueueFillImage(queue,image,color.data(),origin.data(),region.data(),static_cast<cl_uint>(events.size()),events.empty()?nullptr:events.data(),arg(7)?&event:nullptr);
        if(error==success)give_event(arg(7),event);return status(error);
    }
    // 映射：宿主映射同步完成後複製到客體影子區（USE_HOST_PTR 直接使用客體原位址），解除映射時寫回。
    if(name=="clEnqueueMapBuffer" || name=="clEnqueueMapImage"){
        const bool image=name=="clEnqueueMapImage";const auto error_out=arg(image?11:9);
        const auto object=static_cast<cl_mem>(lookup(arg(1),Kind::Mem));if(!object){store32(error_out,invalid_mem);return pointer(0);}
        const auto flags=arg(3);std::vector<cl_event> events;if(!event_list(arg(image?8:6),arg(image?9:7),events)){store32(error_out,invalid_wait_list);return pointer(0);}
        cl_event event=nullptr;cl_int error=success;void* host=nullptr;std::size_t size=0,offset=0;OpenclState::Mapping mapping;mapping.memory=arg(1);mapping.flags=flags;mapping.image=image;
        if(image){
            std::array<std::size_t,3> origin{},region{};if(!triple(arg(4),origin) || !triple(arg(5),region) || !arg(6)){store32(error_out,invalid_value);return pointer(0);}
            std::size_t row=0,slice=0,element=0;
            if(const auto queried=clGetImageInfo(object,CL_IMAGE_ELEMENT_SIZE,sizeof element,&element,nullptr);queried!=success){store32(error_out,queried);return pointer(0);}
            host=clEnqueueMapImage(queue,object,CL_TRUE,flags,origin.data(),region.data(),&row,&slice,static_cast<cl_uint>(events.size()),events.empty()?nullptr:events.data(),arg(10)?&event:nullptr,&error);
            if(host){
                mapping.row=row;mapping.slice=slice;mapping.line=region[0]*element;mapping.rows=region[1];mapping.slices=region[2];
                const auto& entry=bridge.objects.at(arg(1));
                // USE_HOST_PTR：客體以建立時的 pitch 存取自己的記憶體；其餘以宿主 pitch 的影子區。
                mapping.guest_row=entry.host_pointer?entry.host_row:row;mapping.guest_slice=entry.host_pointer?entry.host_slice:slice;
                if(entry.host_pointer)offset=origin[2]*mapping.guest_slice+origin[1]*mapping.guest_row+origin[0]*element;
                store64(arg(6),mapping.guest_row);store64(arg(7),mapping.guest_slice);
                // 區域最後一個位元組的偏移＋1；不讀寫區域外的像素。
                size=(region[2]-1)*slice+(region[1]-1)*row+mapping.line;
            }
        }else{

            offset=static_cast<std::size_t>(arg(4));size=static_cast<std::size_t>(arg(5));
            host=clEnqueueMapBuffer(queue,object,CL_TRUE,flags,offset,size,static_cast<cl_uint>(events.size()),events.empty()?nullptr:events.data(),arg(8)?&event:nullptr,&error);
        }
        if(!host){store32(error_out,error);return pointer(0);}
        const auto use_host=bridge.objects.at(arg(1)).host_pointer;
        const auto guest_size=image?(mapping.slices-1)*mapping.guest_slice+(mapping.rows-1)*mapping.guest_row+mapping.line:size;
        const auto guest=use_host?use_host+offset:allocate_heap(memory,std::max<std::size_t>(guest_size,1));
        if(!guest){clEnqueueUnmapMemObject(queue,object,host,0,nullptr,nullptr);store32(error_out,out_of_host_memory);return pointer(0);}
        const auto lines=[&](auto&& copy){
            // 影像逐列複製區域內容；緩衝為單一連續範圍。
            if(!image){copy(0,0,size);return;}
            for(std::size_t z=0;z<mapping.slices;++z)for(std::size_t y=0;y<mapping.rows;++y)copy(z*mapping.slice+y*mapping.row,z*mapping.guest_slice+y*mapping.guest_row,mapping.line);
        };
        if(!(flags&CL_MAP_WRITE_INVALIDATE_REGION) && size)
            lines([&](std::size_t from,std::size_t to,std::size_t length){memory.write(GuestAddress{guest+to},std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(host)+from,length));});
        mapping.host=host;mapping.size=size;mapping.owned=!use_host;bridge.mappings[guest]=mapping;
        give_event(arg(image?10:8),event);store32(error_out,success);return pointer(guest);
    }
    if(name=="clEnqueueUnmapMemObject"){
        const auto object=static_cast<cl_mem>(lookup(arg(1),Kind::Mem));if(!object)return status(invalid_mem);
        const auto found=bridge.mappings.find(arg(2));if(found==bridge.mappings.end() || found->second.memory!=arg(1))return status(invalid_value);
        const auto mapping=found->second;bridge.mappings.erase(found);
        if((mapping.flags&(CL_MAP_WRITE|CL_MAP_WRITE_INVALIDATE_REGION)) && mapping.size){
            if(!mapping.image){const auto data=read_bytes(arg(2),mapping.size);std::memcpy(mapping.host,data.data(),data.size());}
            else for(std::size_t z=0;z<mapping.slices;++z)for(std::size_t y=0;y<mapping.rows;++y){
                const auto data=read_bytes(arg(2)+z*mapping.guest_slice+y*mapping.guest_row,mapping.line);
                std::memcpy(static_cast<std::uint8_t*>(mapping.host)+z*mapping.slice+y*mapping.row,data.data(),data.size());
            }
        }
        std::vector<cl_event> events;if(!event_list(arg(3),arg(4),events))return status(invalid_wait_list);cl_event event=nullptr;
        const auto error=clEnqueueUnmapMemObject(queue,object,mapping.host,static_cast<cl_uint>(events.size()),events.empty()?nullptr:events.data(),arg(5)?&event:nullptr);
        if(mapping.owned)(void)free_heap(memory,arg(2),state);
        if(error==success){if(event)clWaitForEvents(1,&event);give_event(arg(5),event);}return status(error);
    }
    if(name=="clEnqueueMigrateMemObjects"){
        const auto count=static_cast<cl_uint>(arg(1));if(!count || !arg(2))return status(invalid_value);
        std::vector<cl_mem> objects;for(const auto handle:read_words(arg(2),count)){const auto object=static_cast<cl_mem>(lookup(handle,Kind::Mem));if(!object)return status(invalid_mem);objects.push_back(object);}
        std::vector<cl_event> events;if(!event_list(arg(4),arg(5),events))return status(invalid_wait_list);cl_event event=nullptr;
        const auto error=clEnqueueMigrateMemObjects(queue,count,objects.data(),arg(3),static_cast<cl_uint>(events.size()),events.empty()?nullptr:events.data(),arg(6)?&event:nullptr);
        if(error==success)give_event(arg(6),event);return status(error);
    }
    if(name=="clEnqueueNDRangeKernel" || name=="clEnqueueTask"){
        const auto kernel=static_cast<cl_kernel>(lookup(arg(1),Kind::Kernel));if(!kernel)return status(invalid_kernel);
        const bool task=name=="clEnqueueTask";const auto dimensions=task?1U:static_cast<cl_uint>(arg(2));if(!dimensions || dimensions>3)return status(-53); // CL_INVALID_WORK_DIMENSION。
        std::array<std::size_t,3> offset{},global{1,1,1},local{1,1,1};bool has_offset=false,has_local=task;
        if(!task){
            if(!arg(4))return status(-63); // CL_INVALID_GLOBAL_WORK_SIZE。
            for(cl_uint i=0;i<dimensions;++i){global[i]=memory.load(GuestAddress{arg(4)+i*8},64);if(arg(3))offset[i]=memory.load(GuestAddress{arg(3)+i*8},64);if(arg(5))local[i]=memory.load(GuestAddress{arg(5)+i*8},64);}
            has_offset=arg(3)!=0;has_local=arg(5)!=0;
        }
        std::vector<cl_event> events;if(!event_list(arg(task?2:6),arg(task?3:7),events))return status(invalid_wait_list);cl_event event=nullptr;const auto out=arg(task?4:8);
        const auto started=std::chrono::steady_clock::now();
        const auto error=clEnqueueNDRangeKernel(queue,kernel,dimensions,has_offset?offset.data():nullptr,global.data(),has_local?local.data():nullptr,static_cast<cl_uint>(events.size()),events.empty()?nullptr:events.data(),out?&event:nullptr);
        if(trace){
            // 依核心名稱累計派送次數與宿主耗時（含等候完成），每 10 秒輸出一次。
            char function[256]{};clGetKernelInfo(kernel,CL_KERNEL_FUNCTION_NAME,sizeof function-1,function,nullptr);clFinish(queue);
            auto& item=kernel_stats[function];++item.count;item.global=global;if(error)++item.errors;
            item.micros+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count());
        }
        if(error==success)give_event(out,event);return status(error);
    }
    if(name=="clEnqueueMarker" || name=="clEnqueueMarkerWithWaitList" || name=="clEnqueueBarrierWithWaitList"){
        std::vector<cl_event> events;const bool listed=name!="clEnqueueMarker";
        if(listed && !event_list(arg(1),arg(2),events))return status(invalid_wait_list);
        const auto out=arg(listed?3:1);if(name=="clEnqueueMarker" && !out)return status(invalid_value);cl_event event=nullptr;
        const auto error=name=="clEnqueueBarrierWithWaitList"?clEnqueueBarrierWithWaitList(queue,static_cast<cl_uint>(events.size()),events.empty()?nullptr:events.data(),out?&event:nullptr):
            clEnqueueMarkerWithWaitList(queue,static_cast<cl_uint>(events.size()),events.empty()?nullptr:events.data(),out?&event:nullptr);
        if(error==success)give_event(out,event);return status(error);
    }
    if(name=="clEnqueueWaitForEvents"){
        std::vector<cl_event> events;if(!static_cast<cl_uint>(arg(1)) || !event_list(arg(1),arg(2),events))return status(invalid_event);
        return status(clEnqueueBarrierWithWaitList(queue,static_cast<cl_uint>(events.size()),events.data(),nullptr));
    }
    if(name=="clEnqueueBarrier")return status(clEnqueueBarrierWithWaitList(queue,0,nullptr,nullptr));
    // 客體函式在宿主核心中執行、SVM 與管線物件需 OpenCL 2.x；宿主平台不提供。
    if(name=="clSVMAlloc")return pointer(0);
    if(name=="clSVMFree")return status(success);
    if(name=="clCreatePipe"){store32(arg(5),invalid_operation);return pointer(0);}
    if(name=="clGetDeviceAndHostTimer" || name=="clGetHostTimer" || name=="clSetDefaultDeviceCommandQueue" || name=="clGetPipeInfo" || name=="clEnqueueNativeKernel" || name.starts_with("clEnqueueSVM"))return status(invalid_operation);
    return status(invalid_operation);
#endif
}
}

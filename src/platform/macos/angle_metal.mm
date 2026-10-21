#include "x64arm/angle.hpp"
#include "metal_gpu_counter.hpp"
#include <AppKit/AppKit.h>
#include <QuartzCore/CAMetalLayer.h>
#include <Metal/Metal.h>
#include <ffi/ffi.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <cstdlib>
#include <map>
#include <mutex>
#include <sstream>

namespace x64arm {namespace {
#include "angle_extensions_generated.inc"
constexpr std::size_t transfer_limit=256U*1024U*1024U;
std::mutex angle_mutex;
using Proc=void*(*)(const char*);
struct AngleLibrary {
    void* egl{},*gles{};Proc proc{};std::filesystem::path directory;
    explicit AngleLibrary(const std::filesystem::path& path):directory(path){
        gles=dlopen((path/"libGLESv2.dylib").c_str(),RTLD_NOW|RTLD_LOCAL);
        if(!gles)throw std::runtime_error("ANGLE GLES 載入失敗："+std::string(dlerror()));
        proc=reinterpret_cast<Proc>(dlsym(gles,"EGL_GetProcAddress"));
        if(!proc){egl=dlopen((path/"libEGL.dylib").c_str(),RTLD_NOW|RTLD_LOCAL);if(egl)proc=reinterpret_cast<Proc>(dlsym(egl,"eglGetProcAddress"));}
        if(!proc)throw std::runtime_error("ANGLE 缺少 EGL proc resolver");
        auto query=reinterpret_cast<const char*(*)(void*,int)>(proc("eglQueryString"));
        const auto* extensions=query?query(nullptr,0x3055):nullptr;
        if(!extensions || std::string_view(extensions).find("EGL_ANGLE_platform_angle_metal")==std::string_view::npos)throw std::runtime_error("ANGLE 函式庫沒有 Metal 後端");
    }
    // ANGLE 註冊 Objective-C classes；函式庫在程序生命週期內保持載入。
};
std::shared_ptr<AngleLibrary> library_at(const std::filesystem::path& path){
    static std::shared_ptr<AngleLibrary> library;
    std::lock_guard lock(angle_mutex);
    const auto canonical=std::filesystem::canonical(path);
    if(library && library->directory!=canonical)throw std::runtime_error("同一程序不可混用不同 ANGLE 函式庫");
    if(!library)library=std::make_shared<AngleLibrary>(canonical);
    return library;
}
ffi_type* native_type(AngleType type){switch(type){
case AngleType::Void:return &ffi_type_void;case AngleType::U8:return &ffi_type_uint8;
case AngleType::Float:return &ffi_type_float;case AngleType::I32:return &ffi_type_sint32;
case AngleType::U32:return &ffi_type_uint32;case AngleType::U64:return &ffi_type_uint64;
case AngleType::I64:case AngleType::Size:case AngleType::Attrib:return &ffi_type_sint64;
default:return &ffi_type_pointer;}}
unsigned element_size(AngleType type,bool guest){switch(type){case AngleType::Void:case AngleType::U8:return 1;case AngleType::U32:case AngleType::I32:case AngleType::Float:return 4;case AngleType::Size:case AngleType::Attrib:case AngleType::Handle:case AngleType::Pointer:return guest?4:8;default:return 8;}}
std::size_t bounded_product(std::uint64_t a,std::uint64_t b){if(a>transfer_limit || (b && a>transfer_limit/b))throw HostCallFault("ANGLE 資料超過傳輸上限");return static_cast<std::size_t>(a*b);}
struct Current {std::uint32_t display{},draw{},read{},context{},egl_error{0x3000},gl_error{};unsigned api{0x30a0};};
struct Context {std::uint32_t display{},group{};bool pending{};};
struct Surface {std::uint32_t display{};NSView* view;CALayer* previous;BOOL wanted{};CAMetalLayer* layer;bool pending{};};
struct BufferMapping {std::uint64_t guest{};void* host{};std::size_t length{},offset{};unsigned access{},target{};};
struct VertexPointer {std::uint64_t value{};unsigned buffer{};};
struct HostBindingScope {std::function<void()> finish;~HostBindingScope(){finish();}};
class AngleMetal final:public NativeAngle {
    std::shared_ptr<AngleLibrary> library_;
    std::map<std::uint32_t,void*> handles_;
    std::map<std::uint32_t,std::string> handle_kinds_;
    std::map<std::uint64_t,Current> current_;
    std::map<std::uint32_t,Context> contexts_;
    std::map<std::uint32_t,Surface> surfaces_;
    std::map<std::pair<std::uint32_t,unsigned>,BufferMapping> mappings_;
    std::map<std::tuple<std::uint32_t,unsigned,unsigned>,VertexPointer> vertex_pointers_;
    std::map<std::string,std::uint64_t> strings_;
    struct CounterDevice {id<MTLDevice> device;id<MTLCommandQueue> queue;};
    std::map<std::uint32_t,CounterDevice> counters_;
    std::map<std::pair<std::uint32_t,unsigned>,std::uint64_t> timestamps_;
    std::uint32_t next_handle_{0xa1000000};std::uint64_t calls_{},bytes_in_{},bytes_out_{};
    template<class R,class... A>R host(const char* name,A... args)const{auto f=reinterpret_cast<R(*)(A...)>(library_->proc(name));if(!f)throw HostCallFault("ANGLE 宿主缺少入口："+std::string(name));return f(args...);}
    void* handle(std::uint64_t token,std::string_view kind){if(!token)return nullptr;const auto i=handles_.find(static_cast<std::uint32_t>(token));if(token>UINT32_MAX || i==handles_.end() || handle_kinds_.at(i->first)!=kind)throw HostCallFault("ANGLE 物件 handle 無效");return i->second;}
    std::uint32_t token(void* value,std::string_view kind){if(!value)return 0;for(const auto& [id,p]:handles_)if(p==value && handle_kinds_.at(id)==kind)return id;if(handles_.size()>=65536)throw std::bad_alloc{};const auto id=next_handle_++;handles_[id]=value;handle_kinds_[id]=kind;return id;}
    static std::string_view handle_kind(std::string_view name){if(name=="dpy" || name=="display")return "display";if(name=="config")return "config";if(name=="ctx" || name=="share_context")return "context";if(name=="sync")return "sync";if(name=="image")return "image";return "surface";}
    unsigned integer(unsigned pname)const{int result=0;host<void>("glGetIntegerv",pname,&result);return static_cast<unsigned>(std::max(result,0));}
    bool es3()const{const auto* version=host<const char*>("glGetString",0x1f02U);return version && std::string_view(version).find("OpenGL ES 3")!=std::string_view::npos;}
    unsigned binding(unsigned target)const{if(target!=0x8892 && target!=0x8893 && !es3())return 0;switch(target){case 0x8892:return integer(0x8894);case 0x8893:return integer(0x8895);case 0x88eb:return integer(0x88ed);case 0x88ec:return integer(0x88ef);case 0x8f36:case 0x8f37:return integer(target);case 0x8a11:return integer(0x8a28);case 0x8c8e:return integer(0x8c8f);default:return 0;}}
    void bind(Current& c){host<unsigned>("eglBindAPI",c.api);if(!host<unsigned>("eglMakeCurrent",handle(c.display,"display"),handle(c.draw,"surface"),handle(c.read,"surface"),handle(c.context,"context")))throw HostCallFault("ANGLE 客體執行緒 context 還原失敗");}
    void gl_error(Current& c,unsigned error){if(!c.gl_error)c.gl_error=error;}
    bool timestamp_supported(std::uint32_t display){
        if(!counters_.contains(display)){CounterDevice counter;auto display_query=reinterpret_cast<unsigned(*)(void*,int,std::intptr_t*)>(library_->proc("eglQueryDisplayAttribEXT"));auto device_query=reinterpret_cast<unsigned(*)(void*,int,std::intptr_t*)>(library_->proc("eglQueryDeviceAttribEXT"));std::intptr_t native_device=0,metal_device=0;if(display_query && device_query && display_query(handle(display,"display"),0x322c,&native_device) && device_query(reinterpret_cast<void*>(native_device),0x34a6,&metal_device))counter.device=(__bridge id<MTLDevice>)reinterpret_cast<void*>(metal_device);counters_[display]=counter;}
        return metal_timestamp_supported(counters_.at(display).device);
    }
    std::uint64_t timestamp(std::uint32_t display){host<void>("glFinish");auto& counter=counters_.at(display);if(!counter.queue)counter.queue=[counter.device newCommandQueue];auto commands=[counter.queue commandBuffer];if(!commands)throw std::bad_alloc{};return metal_timestamp_sample(counter.device,commands,[&]{[commands commit];[commands waitUntilCompleted];}).ticks;}
    std::vector<std::string> extensions(unsigned selector=0x1f03)const{
        const auto* text=host<const char*>("glGetString",selector);std::istringstream stream(text?text:"");std::vector<std::string> result;std::string name;
        while(stream>>name){const auto contract=std::find_if(std::begin(extension_contracts),std::end(extension_contracts),[&](const auto& c){return c.name==name;});if(contract==std::end(extension_contracts))continue;
            if(std::all_of(contract->commands.begin(),contract->commands.end(),[&](auto entry){return std::any_of(angle_commands().begin(),angle_commands().end(),[&](const auto& c){return c.name==entry;}) && supports(entry);}))result.push_back(name);
        }return result;
    }
    std::size_t query_count(std::string_view name,unsigned pname,const std::vector<std::uint64_t>& values)const;
    std::size_t pixel_size(const AngleCommand&,const std::vector<std::uint64_t>&,bool)const;
    void release_surface(std::uint32_t id){const auto it=surfaces_.find(id);if(it==surfaces_.end())return;auto& s=it->second;if(s.view && s.view.layer==s.layer){s.view.layer=s.previous;s.view.wantsLayer=s.wanted;[s.view setNeedsDisplay:YES];}surfaces_.erase(it);}
public:
    explicit AngleMetal(std::shared_ptr<AngleLibrary> library):library_(std::move(library)){}
    ~AngleMetal()override{std::lock_guard lock(angle_mutex);for(auto& [thread,c]:current_){(void)thread;if(c.context)host<unsigned>("eglMakeCurrent",handle(c.display,"display"),nullptr,nullptr,nullptr);}for(const auto& [id,c]:contexts_)host<unsigned>("eglDestroyContext",handle(c.display,"display"),handle(id,"context"));while(!surfaces_.empty()){const auto id=surfaces_.begin()->first;host<unsigned>("eglDestroySurface",handle(surfaces_.begin()->second.display,"display"),handle(id,"surface"));release_surface(id);}}
    bool supports(std::string_view name)const override{
        // 原生 pixmap／Windows D3D client buffer 不能當成 macOS 指標。只公開可完整轉接的入口。
        if(name=="eglCopyBuffers" || name=="eglCreatePixmapSurface" || name=="eglCreatePlatformPixmapSurface" || name=="eglCreatePbufferFromClientBuffer" || name=="eglCreateImage" || name=="eglDestroyImage")return false;
        return library_->proc(std::string(name).c_str())!=nullptr;
    }
    std::string diagnostics()const override{std::ostringstream s;s<<"angle_backend=metal angle_native_calls="<<calls_<<" angle_input_bytes="<<bytes_in_<<" angle_output_bytes="<<bytes_out_<<" angle_library_directory="<<library_->directory.string()<<'\n';return s.str();}
    std::uint64_t invoke(GuestState&,GuestMemory&,const WindowsCall&,const AngleCommand&,std::uint64_t,
        const std::function<std::uint64_t(std::span<const std::uint8_t>)>&,
        const std::function<void(std::uint64_t)>&,
        const std::function<std::uint64_t(std::string_view)>&,
        const std::function<AngleWindow(std::uint64_t)>&)override;
};
std::size_t AngleMetal::query_count(std::string_view name,unsigned pname,const std::vector<std::uint64_t>& v)const{
    if(name=="glGetUniformfv" || name=="glGetUniformiv" || name=="glGetUniformuiv"){
        int count=0,max_name=0;host<void>("glGetProgramiv",static_cast<unsigned>(v[0]),0x8b86U,&count);host<void>("glGetProgramiv",static_cast<unsigned>(v[0]),0x8b87U,&max_name);
        std::vector<char> text(static_cast<std::size_t>(std::max(max_name,1)));for(int i=0;i<count;++i){int length=0,size=0;unsigned type=0;host<void>("glGetActiveUniform",static_cast<unsigned>(v[0]),static_cast<unsigned>(i),max_name,&length,&size,&type,text.data());
            std::string base(text.data(),static_cast<std::size_t>(std::max(length,0)));if(base.ends_with("[0]"))base.resize(base.size()-3);
            for(int e=0;e<std::max(size,1);++e){const auto key=size>1?base+"["+std::to_string(e)+"]":base;const auto at=host<int>("glGetUniformLocation",static_cast<unsigned>(v[0]),key.c_str());if(at!=static_cast<int>(v[1]))continue;
                switch(type){case 0x8b50:case 0x8b53:case 0x8b57:case 0x8dc6:return 2;case 0x8b51:case 0x8b54:case 0x8b58:case 0x8dc7:return 3;case 0x8b52:case 0x8b55:case 0x8b59:case 0x8dc8:case 0x8b5a:return 4;case 0x8b5b:return 9;case 0x8b5c:return 16;case 0x8b65:case 0x8b67:return 6;case 0x8b66:case 0x8b69:return 8;case 0x8b68:case 0x8b6a:return 12;default:return 1;}}
        }return 0;
    }
    if(name=="glGetActiveUniformsiv" || name=="glGetUniformIndices")return static_cast<unsigned>(v[name=="glGetActiveUniformsiv"?1:1]);
    if(name=="glGetActiveUniformBlockiv" && pname==0x8a43){int count=0;host<void>("glGetActiveUniformBlockiv",static_cast<unsigned>(v[0]),static_cast<unsigned>(v[1]),0x8a42U,&count);return static_cast<unsigned>(std::max(count,0));}
    if(name=="glClearBufferfv" || name=="glClearBufferiv" || name=="glClearBufferuiv")return v[0]==0x1800?4:1;
    if(name.starts_with("glGetVertexAttrib"))return pname==0x8626?4:1;
    if(name.find("SamplerParameter")!=name.npos || name.find("TexParameter")!=name.npos)return pname==0x1004?4:1;
    if(name=="glGetIntegerv" || name=="glGetBooleanv" || name=="glGetFloatv" || name=="glGetInteger64v"){
        if(pname==0x86a3)return integer(0x86a2);if(pname==0x8df8)return integer(0x8df9);if(pname==0x87ff)return integer(0x87fe);
        switch(pname){case 0xba2:case 0xc10:case 0xc22:case 0xc23:case 0x8005:return 4;case 0xb70:case 0xd3a:case 0x846d:case 0x846e:return 2;default:return 1;}
    }return 1;
}
std::size_t AngleMetal::pixel_size(const AngleCommand& command,const std::vector<std::uint64_t>& values,bool pack)const{
    const auto arg=[&](std::string_view name)->unsigned{for(unsigned i=0;i<command.parameters.size();++i)if(command.parameters[i].name==name)return static_cast<unsigned>(values[i]);return 1;};
    const auto width=arg("width"),height=arg("height"),depth=arg("depth");if(width>INT32_MAX || height>INT32_MAX || depth>INT32_MAX || !width || !height || !depth)return 0;
    const auto format=arg("format"),type=arg("type");unsigned channels=0;switch(format){case 0x1902:case 0x1903:case 0x1906:case 0x1909:case 0x8d94:channels=1;break;case 0x8227:case 0x190a:case 0x8228:channels=2;break;case 0x1907:case 0x8d98:channels=3;break;case 0x1908:case 0x80e1:case 0x8d99:case 0x84f9:channels=4;break;default:return 0;}
    unsigned bytes=0;switch(type){case 0x1400:case 0x1401:bytes=channels;break;case 0x1402:case 0x1403:case 0x140b:case 0x8d61:bytes=channels*2;break;case 0x1404:case 0x1405:case 0x1406:bytes=channels*4;break;case 0x8033:case 0x8034:case 0x8363:bytes=2;break;case 0x8035:case 0x8368:case 0x8c3b:case 0x8c3e:case 0x84fa:bytes=4;break;case 0x8dad:bytes=8;break;default:return 0;}
    const auto* version=host<const unsigned char*>("glGetString",0x1f02U);const bool es3=version && std::string_view(reinterpret_cast<const char*>(version)).find("OpenGL ES 3")!=std::string_view::npos;
    const auto align=integer(pack?0xd05:0xcf5),row=es3?integer(pack?0xd02:0xcf2):0,skip_rows=es3?integer(pack?0xd03:0xcf3):0,skip_pixels=es3?integer(pack?0xd04:0xcf4):0;
    const auto stride=(bounded_product(row?row:width,bytes)+align-1)/align*align;
    const bool volume=command.name.find("3D")!=std::string_view::npos;const auto image_height=pack || !volume || !es3?height:integer(0x806e),skip_images=pack || !volume || !es3?0:integer(0x806d);
    const auto images=bounded_product(static_cast<std::uint64_t>(skip_images)+depth-1,image_height?image_height:height);
    return bounded_product(images+skip_rows+height-1,stride)+bounded_product(static_cast<std::uint64_t>(skip_pixels)+width,bytes);
}
std::string guest_string(GuestMemory& memory,std::uint64_t pointer,std::size_t limit=16U*1024U*1024U){std::string value;while(value.size()<limit){const auto c=static_cast<char>(memory.load(GuestAddress{pointer+value.size()},8));if(!c)return value;value.push_back(c);}throw HostCallFault("ANGLE 字串超過長度上限");}
std::uint64_t AngleMetal::invoke(GuestState& state,GuestMemory& memory,const WindowsCall& call,const AngleCommand& command,std::uint64_t thread,
    const std::function<std::uint64_t(std::span<const std::uint8_t>)>& allocate,
    const std::function<void(std::uint64_t)>& release,
    const std::function<std::uint64_t(std::string_view)>& resolve,
    const std::function<AngleWindow(std::uint64_t)>& window){@autoreleasepool{
    std::lock_guard lock(angle_mutex);auto& current=current_[thread];const auto name=command.name;const bool egl=name.starts_with("egl"),x86=memory.execution_mode()==ExecutionMode::X86;const unsigned guest_pointer=x86?4:8;
    ++calls_;const auto group=current.context?contexts_.at(current.context).group:0U;
    if(name=="eglGetError" && current.egl_error!=0x3000){const auto value=current.egl_error;current.egl_error=0x3000;return value;}
    if(name=="eglGetProcAddress")return resolve(guest_string(memory,call(0),4096));
    if(name=="eglGetCurrentDisplay")return current.display;
    if(name=="eglGetCurrentContext")return current.context;
    if(name=="eglGetCurrentSurface"){if(static_cast<unsigned>(call(0))!=0x3059 && static_cast<unsigned>(call(0))!=0x305a){current.egl_error=0x300c;return 0;}return static_cast<unsigned>(call(0))==0x3059?current.draw:current.read;}
    if(name=="eglQueryAPI")return current.api;
    // EGL 的原生 TLS 不可讓不同客體執行緒借用；每次在鎖內還原、結束時解除。
    if(current.context)bind(current);else host<unsigned>("eglBindAPI",current.api);
    HostBindingScope host_binding{[&]{if(egl && name!="eglGetError"){const auto error=host<unsigned>("eglGetError");if(error!=0x3000)current.egl_error=error;}if(auto display=host<void*>("eglGetCurrentDisplay"))host<unsigned>("eglMakeCurrent",display,nullptr,nullptr,nullptr);(void)host<unsigned>("eglGetError");}};
    if(!egl){if(!current.context){if(name=="glGetError")return 0;return 0;}if(name=="glGetError" && current.gl_error){const auto result=current.gl_error;current.gl_error=0;return result;}}
    const auto count=command.parameters.size();std::vector<std::uint64_t> values(count),original(count);std::vector<ffi_type*> types(count);std::vector<void*> arguments(count);
    struct Block {std::vector<std::uint8_t> data;std::uint64_t guest{};std::size_t guest_bytes{};AngleType element{};bool output{},configs{},attribs{};};
    std::vector<Block> blocks(count);std::vector<std::vector<std::string>> strings(count);std::vector<std::vector<const char*>> string_arrays(count);
    for(unsigned i=0;i<count;++i){auto value=call(i);const auto kind=command.parameters[i].type;
        if(kind==AngleType::Float && !x86 && i<4){std::uint32_t bits;std::memcpy(&bits,state.xmm[i].data(),4);value=bits;}
        if(x86 && (kind==AngleType::Size || kind==AngleType::Attrib))value=static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
        if(kind==AngleType::U32 || kind==AngleType::I32 || kind==AngleType::Float)value=static_cast<std::uint32_t>(value);else if(kind==AngleType::U8)value=static_cast<std::uint8_t>(value);
        original[i]=values[i]=value;types[i]=native_type(kind);arguments[i]=&values[i];
    }
    for(unsigned i=0;i<count;++i){const auto& parameter=command.parameters[i];const auto key=parameter.name;
        const bool extent=key=="n" || key=="count" || key=="drawcount" || key=="instanceCount" || key=="paramCount" || key=="uniformCount" || key=="bufSize" || key=="imageSize" || key=="width" || key=="height" || key=="depth" || key=="samples" || key=="stride" || key=="config_size" || key=="maxCount";
        if((extent && parameter.type==AngleType::I32 && static_cast<std::int32_t>(original[i])<0) || (parameter.type==AngleType::Size && static_cast<std::int64_t>(original[i])<0)){if(egl)current.egl_error=0x300c;else gl_error(current,0x501);return 0;}
        if(parameter.type==AngleType::Handle && original[i] && key!="display_id" && key!="win" && key!="pixmap"){
            const auto kind=key=="sync"?(egl?"eglsync":"glsync"):handle_kind(key);const auto token_value=static_cast<std::uint32_t>(original[i]);const auto found=handles_.find(token_value);
            if(original[i]>UINT32_MAX || found==handles_.end() || handle_kinds_.at(token_value)!=kind){if(egl)current.egl_error=kind=="display"?0x3008:kind=="config"?0x3005:kind=="context"?0x3006:kind=="surface"?0x300d:0x300c;else if(name!="glIsSync")gl_error(current,0x501);return name=="glClientWaitSync"?0x911d:0;}
        }
    }
    if(name=="eglMakeCurrent" && original[3] && std::any_of(current_.begin(),current_.end(),[&](const auto& owner){return owner.first!=thread && owner.second.context==original[3];})){current.egl_error=0x3002;return 0;}
    if(name=="eglDestroyContext"){const auto id=static_cast<unsigned>(original[1]);if(id && contexts_.contains(id) && std::any_of(current_.begin(),current_.end(),[&](const auto& owner){return owner.second.context==id;})){if(contexts_.at(id).pending){current.egl_error=0x3006;return 0;}contexts_.at(id).pending=true;return 1;}}
    if(name=="eglDestroySurface"){const auto id=static_cast<unsigned>(original[1]);if(id && surfaces_.contains(id) && std::any_of(current_.begin(),current_.end(),[&](const auto& owner){return owner.second.draw==id || owner.second.read==id;})){if(surfaces_.at(id).pending){current.egl_error=0x300d;return 0;}surfaces_.at(id).pending=true;return 1;}}
    const bool ignored_uniform=name.starts_with("glUniform") && !name.starts_with("glUniformBlock") && static_cast<std::int32_t>(original[0])==-1;
    const auto named=[&](std::string_view key)->std::uint64_t{for(unsigned i=0;i<count;++i)if(command.parameters[i].name==key)return original[i];throw HostCallFault("ANGLE API 長度欄位不存在："+std::string(key));};
    const auto numeric=[&](std::string_view text)->std::size_t{
        if(const auto slash=text.find('/');slash!=text.npos){auto key=text.substr(0,slash);while(!key.empty() && key.back()==' ')key.remove_suffix(1);return bounded_product(named(key),1)/std::stoull(std::string(text.substr(slash+1)));}
        std::size_t size=1,start=0;while(start<text.size()){const auto end=text.find('*',start);const auto part=text.substr(start,end==text.npos?text.size()-start:end-start);std::uint64_t n=0;if(!part.empty() && part[0]>='0' && part[0]<='9')n=std::stoull(std::string(part));else n=named(part);size=bounded_product(size,n);if(end==text.npos)break;start=end+1;}return size;
    };
    const auto string_result=[&](const std::string& value){const auto found=strings_.find(value);if(found!=strings_.end())return found->second;const auto pointer=allocate({reinterpret_cast<const std::uint8_t*>(value.c_str()),value.size()+1});strings_[value]=pointer;return pointer;};
    if(name=="glGetString" && (original[0]==0x1f03 || original[0]==0x93a8)){std::string value;for(const auto& extension:extensions(static_cast<unsigned>(original[0]))){if(!value.empty())value+=' ';value+=extension;}return string_result(value);}
    if(name=="glGetStringi" && (original[0]==0x1f03 || original[0]==0x93a8)){const auto names=extensions(static_cast<unsigned>(original[0]));if(original[1]>=names.size()){gl_error(current,0x501);return 0;}return string_result(names[static_cast<std::size_t>(original[1])]);}
    if((name=="glGetIntegerv" || name=="glGetInteger64v" || name=="glGetBooleanv" || name=="glGetFloatv" || name=="glGetIntegervRobustANGLE" || name=="glGetInteger64vRobustANGLE" || name=="glGetBooleanvRobustANGLE" || name=="glGetFloatvRobustANGLE") && (original[0]==0x821d || original[0]==0x93a9)){const bool robust=name.ends_with("RobustANGLE");if(robust && !original[1]){gl_error(current,0x502);return 0;}std::uint64_t value=extensions(original[0]==0x821d?0x1f03:0x93a8).size();const auto type=command.parameters.back().element;if(type==AngleType::Float)value=std::bit_cast<std::uint32_t>(static_cast<float>(value));else if(type==AngleType::U8)value=value!=0;memory.store(GuestAddress{original.back()},static_cast<std::uint8_t>(element_size(type,false)*8),value);if(robust && original[2])memory.store(GuestAddress{original[2]},32,1);return 0;}
    // ANGLE Metal 的 QueryCounter 尚未提供取樣；共用原生 Metal counter 與記憶體配額。
    // Finish 建立跨 queue 的完成順序，counter 本身取自 GPU，沒有 CPU 時鐘替代值。
    if(!egl && timestamp_supported(current.display)){
        if((name=="glGetQueryiv" || name=="glGetQueryivEXT" || name=="glGetQueryivRobustANGLE") && original[0]==0x8e28 && original[1]==0x8864){const bool robust=name.ends_with("RobustANGLE");if(robust && !original[2]){gl_error(current,0x502);return 0;}memory.store(GuestAddress{original[robust?4:2]},32,64);if(robust && original[3])memory.store(GuestAddress{original[3]},32,1);return 0;}
        if((name=="glGetInteger64v" || name=="glGetInteger64vEXT" || name=="glGetInteger64vRobustANGLE") && original[0]==0x8e28){const bool robust=name.ends_with("RobustANGLE");if(robust && !original[1]){gl_error(current,0x502);return 0;}memory.store(GuestAddress{original[robust?3:1]},64,timestamp(current.display));if(robust && original[2])memory.store(GuestAddress{original[2]},32,1);return 0;}
        if(name=="glQueryCounterEXT"){
            gl_error(current,host<unsigned>("glGetError"));host<void>("glQueryCounterEXT",static_cast<unsigned>(original[0]),static_cast<unsigned>(original[1]));const auto error=host<unsigned>("glGetError");if(error){gl_error(current,error);return 0;}timestamps_[{current.context,static_cast<unsigned>(original[0])}]=timestamp(current.display);return 0;
        }
        if(name.starts_with("glGetQueryObject")){
            const auto found=timestamps_.find({current.context,static_cast<unsigned>(original[0])});if(found!=timestamps_.end()){
                if(original[1]!=0x8866 && original[1]!=0x8867){gl_error(current,0x500);return 0;}const bool robust=name.ends_with("RobustANGLE");if(robust && !original[2]){gl_error(current,0x502);return 0;}const auto type=command.parameters.back().element;const auto maximum=type==AngleType::I32?std::uint64_t{INT32_MAX}:type==AngleType::U32?std::uint64_t{UINT32_MAX}:type==AngleType::I64?std::uint64_t{INT64_MAX}:UINT64_MAX;const auto value=original[1]==0x8867?1:std::min(found->second,maximum);memory.store(GuestAddress{original.back()},static_cast<std::uint8_t>(element_size(type,false)*8),value);if(robust && original[3])memory.store(GuestAddress{original[3]},32,1);return 0;
            }
        }
    }
    if(name=="eglGetDisplay" || name=="eglGetPlatformDisplay" || name=="eglGetPlatformDisplayEXT"){
        // 統一選定原生 Metal；保留 Metal 可理解的共用平台屬性，不把 Windows HDC 當成 host 指標。
        if(name!="eglGetDisplay" && original[0]!=0x3202){current.egl_error=0x300c;return 0;}
        std::vector<int> attributes{0x3203,0x3489};
        if(name!="eglGetDisplay" && original[2]){const auto step=name=="eglGetPlatformDisplay"?guest_pointer:4U;for(unsigned n=0;n<256;++n){const auto key=static_cast<int>(memory.load(GuestAddress{original[2]+n*2*step},static_cast<std::uint8_t>(step*8)));if(key==0x3038)break;const auto value=static_cast<int>(memory.load(GuestAddress{original[2]+(n*2+1)*step},static_cast<std::uint8_t>(step*8)));if(key!=0x3203 && key!=0x3204 && key!=0x3205 && key!=0x3209 && key!=0x320f && key!=0x34a0 && key!=0x34a1){attributes.push_back(key);attributes.push_back(value);}if(n==255)throw HostCallFault("EGL 平台屬性未終止");}}
        attributes.push_back(0x3038);return token(host<void*>("eglGetPlatformDisplayEXT",0x3202U,static_cast<void*>(nullptr),attributes.data()),"display");
    }
    if(name=="eglCreateWindowSurface" || name=="eglCreatePlatformWindowSurface" || name=="eglCreatePlatformWindowSurfaceEXT"){
        const auto w=window(original[2]);if(!w.width || !w.height){current.egl_error=0x300b;return 0;}
        std::vector<int> attributes;if(original[3]){const auto width=name=="eglCreatePlatformWindowSurface"?guest_pointer:4U;for(unsigned n=0;n<512;++n){const auto value=static_cast<int>(memory.load(GuestAddress{original[3]+n*width},static_cast<std::uint8_t>(width*8)));attributes.push_back(value);if(n%2==0 && value==0x3038)break;if(n==511)throw HostCallFault("EGL surface 屬性未終止");}}
        if(attributes.empty())attributes.push_back(0x3038);Surface surface;surface.display=static_cast<unsigned>(original[0]);void* native=nullptr;
        if(w.view){surface.view=(__bridge NSView*)w.view;surface.previous=surface.view.layer;surface.wanted=surface.view.wantsLayer;surface.layer=[CAMetalLayer layer];surface.layer.device=MTLCreateSystemDefaultDevice();surface.layer.pixelFormat=MTLPixelFormatBGRA8Unorm;surface.layer.drawableSize=CGSizeMake(w.width,w.height);surface.view.wantsLayer=YES;surface.view.layer=surface.layer;native=host<void*>("eglCreateWindowSurface",handle(original[0],"display"),handle(original[1],"config"),(__bridge void*)surface.layer,attributes.data());}
        else{const int offscreen[]{0x3057,static_cast<int>(w.width),0x3056,static_cast<int>(w.height),0x3038};native=host<void*>("eglCreatePbufferSurface",handle(original[0],"display"),handle(original[1],"config"),offscreen);}
        const auto id=token(native,"surface");if(id)surfaces_[id]=surface;else if(surface.view){surface.view.layer=surface.previous;surface.view.wantsLayer=surface.wanted;}return id;
    }
    if(name=="glGetBufferPointerv" || name=="glGetBufferPointervRobustANGLE"){
        void* native=nullptr;int length=0;const bool robust=name.ends_with("RobustANGLE");if(robust)host<void>("glGetBufferPointervRobustANGLE",static_cast<unsigned>(original[0]),static_cast<unsigned>(original[1]),static_cast<int>(original[2]),&length,&native);else host<void>("glGetBufferPointerv",static_cast<unsigned>(original[0]),static_cast<unsigned>(original[1]),&native);
        if(robust && original[3] && length)memory.store(GuestAddress{original[3]},32,static_cast<unsigned>(length));if(robust && !length)return 0;const auto found=mappings_.find({group,binding(static_cast<unsigned>(original[0]))});memory.store(GuestAddress{original[robust?4:2]},static_cast<std::uint8_t>(guest_pointer*8),!native || found==mappings_.end()?0:found->second.guest);return 0;
    }
    if(name=="glGetVertexAttribPointerv" || name=="glGetVertexAttribPointervRobustANGLE"){
        void* native=nullptr;int length=0;const bool robust=name.ends_with("RobustANGLE");if(robust)host<void>("glGetVertexAttribPointervRobustANGLE",static_cast<unsigned>(original[0]),static_cast<unsigned>(original[1]),static_cast<int>(original[2]),&length,&native);else host<void>("glGetVertexAttribPointerv",static_cast<unsigned>(original[0]),static_cast<unsigned>(original[1]),&native);
        if(robust && original[3] && length)memory.store(GuestAddress{original[3]},32,static_cast<unsigned>(length));if(robust && !length)return 0;if(static_cast<unsigned>(original[1])!=0x8645)return 0;const auto it=vertex_pointers_.find({current.context,integer(0x85b5),static_cast<unsigned>(original[0])});memory.store(GuestAddress{original[robust?4:2]},static_cast<std::uint8_t>(guest_pointer*8),it==vertex_pointers_.end()?0:it->second.value);return 0;
    }
    if(name=="glVertexAttribPointer" || name=="glVertexAttribIPointer"){
        const auto buffer=binding(0x8892);if(!buffer){gl_error(current,0x502);return 0;}vertex_pointers_[{current.context,integer(0x85b5),static_cast<unsigned>(original[0])}]={original.back(),buffer};
    }
    if(name=="glUnmapBuffer" || name=="glFlushMappedBufferRange"){
        const auto target=static_cast<unsigned>(original[0]);const auto found=mappings_.find({group,binding(target)});
        if(found!=mappings_.end() && (found->second.access&2)){auto& mapped=found->second;const bool flush=name=="glFlushMappedBufferRange";const auto offset=flush?original[1]:0,length=flush?original[2]:mapped.length;
            if(offset>mapped.length || length>mapped.length-offset){gl_error(current,0x501);return 0;}if(flush || !(mapped.access&0x10)){memory.read(GuestAddress{mapped.guest+offset},{static_cast<std::uint8_t*>(mapped.host)+offset,static_cast<std::size_t>(length)});bytes_in_+=length;}}
    }
    for(unsigned i=0;i<count;++i){const auto& parameter=command.parameters[i];auto& block=blocks[i];const auto pointer=original[i];
        if(parameter.type==AngleType::Handle){if(parameter.name=="display_id" || parameter.name=="win" || parameter.name=="pixmap")values[i]=0;else values[i]=reinterpret_cast<std::uintptr_t>(handle(pointer,parameter.name=="sync"?(egl?"eglsync":"glsync"):handle_kind(parameter.name)));continue;}
        if(parameter.type!=AngleType::Pointer)continue;
        if((parameter.name=="indices" && name.starts_with("glDraw") && binding(0x8893)) || (parameter.name=="pointer" && name.starts_with("glVertexAttrib")) || ((parameter.name=="pixels" || parameter.name=="data") && (name.find("TexImage")!=name.npos || name.find("TexSubImage")!=name.npos || name.starts_with("glRead")) && binding(name.starts_with("glRead")?0x88eb:0x88ec)))continue;
        if(ignored_uniform){block.data.resize(64);values[i]=reinterpret_cast<std::uintptr_t>(block.data.data());continue;}
        if(!pointer){
            const bool nullable=egl || parameter.name=="length" || (name=="glGetAttachedShaders" && parameter.name=="count") || name=="glBufferData" || name.find("TexImage")!=name.npos || (parameter.name=="indices" && name.starts_with("glDraw"));
            if(!nullable){std::size_t entries=1;if(!parameter.length.empty() && !parameter.length.starts_with("COMPSIZE"))entries=numeric(parameter.length);else if(parameter.name=="pixels")entries=pixel_size(command,original,name.starts_with("glRead"));else if(parameter.length.starts_with("COMPSIZE")){const auto pname=std::find_if(command.parameters.begin(),command.parameters.end(),[](const auto& p){return p.name=="pname";});entries=query_count(name,pname==command.parameters.end()?0:static_cast<unsigned>(original[static_cast<std::size_t>(pname-command.parameters.begin())]),original);}if(entries)memory.check(GuestAddress{0},1,parameter.input?Access::Read:Access::Write);}
            values[i]=0;continue;
        }
        block.guest=pointer;block.element=parameter.element;block.output=!parameter.input;
        const bool text=parameter.element==AngleType::U8 && (parameter.name=="name" || parameter.name=="uniformBlockName" || parameter.name=="procname");
        if(text && parameter.input){auto value=guest_string(memory,pointer);block.data.assign(value.begin(),value.end());block.data.push_back(0);}
        else if(parameter.element==AngleType::Pointer && parameter.name=="indices" && name.starts_with("glMultiDraw")){
            if(!binding(0x8893)){gl_error(current,0x502);return 0;}const auto entries=numeric(parameter.length);block.data.resize(bounded_product(entries,8));for(std::size_t n=0;n<entries;++n){const auto offset=memory.load(GuestAddress{pointer+n*guest_pointer},static_cast<std::uint8_t>(guest_pointer*8));std::memcpy(block.data.data()+n*8,&offset,8);}
        }else if(parameter.name=="attrib_list"){
            const auto width=parameter.element==AngleType::Attrib?guest_pointer:4U;std::vector<std::uint64_t> attributes;for(unsigned n=0;n<512;++n){const auto value=memory.load(GuestAddress{pointer+n*width},static_cast<std::uint8_t>(width*8));attributes.push_back(value);if(n%2==0 && value==0x3038)break;if(n==511)throw HostCallFault("EGL 屬性未終止");}
            const auto native_width=parameter.element==AngleType::Attrib?8U:4U;block.data.resize(attributes.size()*native_width);for(unsigned n=0;n<attributes.size();++n)std::memcpy(block.data.data()+n*native_width,&attributes[n],native_width);
        }else if(parameter.name=="string" || parameter.name=="varyings" || parameter.name=="uniformNames"){
            const auto entries=static_cast<unsigned>(named(parameter.name=="uniformNames"?"uniformCount":"count"));if(entries>4096)throw HostCallFault("ANGLE 字串陣列太大");strings[i].reserve(entries);string_arrays[i].reserve(entries);
            for(unsigned n=0;n<entries;++n){const auto address=memory.load(GuestAddress{pointer+n*guest_pointer},static_cast<std::uint8_t>(guest_pointer*8));int length=-1;if(parameter.name=="string" && named("length"))length=static_cast<int>(memory.load(GuestAddress{named("length")+n*4},32));std::string value;if(length<0)value=guest_string(memory,address);else{value.resize(bounded_product(static_cast<unsigned>(length),1));memory.read(GuestAddress{address},{reinterpret_cast<std::uint8_t*>(value.data()),value.size()});}strings[i].push_back(std::move(value));}
            for(const auto& value:strings[i])string_arrays[i].push_back(value.c_str());values[i]=reinterpret_cast<std::uintptr_t>(string_arrays[i].data());continue;
        }else{
            std::size_t entries=1,bytes=0;bool byte_sized=false;block.configs=parameter.name=="configs";block.attribs=parameter.element==AngleType::Attrib;
            if(block.configs)entries=numeric("config_size");
            else if(name.ends_with("RobustANGLE") && parameter.length=="bufSize"){byte_sized=true;bytes=numeric("bufSize");}
            else if(name=="glReadPixels" || parameter.name=="pixels" || (parameter.name=="data" && name.find("CompressedTex")!=name.npos)){byte_sized=true;bytes=name.find("CompressedTex")!=name.npos?numeric("imageSize"):pixel_size(command,original,name.starts_with("glRead"));}
            else if(parameter.name=="indices" && name.starts_with("glDraw")){const auto type=named("type");const auto size=type==0x1401?1U:type==0x1403?2U:type==0x1405?4U:0U;byte_sized=true;bytes=bounded_product(named("count"),size);}
            else if(name=="glGetUniformIndices")entries=numeric("uniformCount");
            else if(parameter.length.starts_with("COMPSIZE")){const auto pname=std::find_if(command.parameters.begin(),command.parameters.end(),[](const auto& p){return p.name=="pname";});entries=query_count(name,pname==command.parameters.end()?0:static_cast<unsigned>(original[static_cast<std::size_t>(pname-command.parameters.begin())]),original);}
            else if(!parameter.length.empty())entries=numeric(parameter.length);
            const auto native_width=element_size(parameter.element,false);const auto guest_width=(parameter.element==AngleType::Attrib || block.configs)?guest_pointer:native_width;
            if(!byte_sized)bytes=bounded_product(entries,native_width);block.guest_bytes=block.configs || block.attribs?bounded_product(entries,guest_width):bytes;
            block.data.resize(std::max<std::size_t>(bytes,1));memory.check(GuestAddress{pointer},block.guest_bytes,block.output?Access::Write:Access::Read);
            if(!block.configs){if(block.attribs && x86){for(std::size_t n=0;n<entries;++n){const auto value=static_cast<std::int64_t>(static_cast<std::int32_t>(memory.load(GuestAddress{pointer+n*4},32)));std::memcpy(block.data.data()+n*8,&value,8);}}else if(block.output){const auto mapped=memory.query(GuestAddress{pointer});if(mapped && (mapped->permissions&permission(Access::Read)))memory.read(GuestAddress{pointer},{block.data.data(),block.guest_bytes});}else memory.read(GuestAddress{pointer},{block.data.data(),block.guest_bytes});}
            if(!block.output)bytes_in_+=block.guest_bytes;
        }values[i]=reinterpret_cast<std::uintptr_t>(block.data.data());
    }
    if(name=="eglBindAPI")values[0]=static_cast<unsigned>(original[0]);
    ffi_cif cif;const auto prepared=ffi_prep_cif(&cif,FFI_DEFAULT_ABI,static_cast<unsigned>(count),native_type(command.result),types.data());if(prepared!=FFI_OK)throw HostCallFault("ANGLE ABI 準備失敗");
    std::uint64_t result=0;auto function=library_->proc(std::string(name).c_str());if(!function)throw HostCallFault("ANGLE 入口不存在");if(std::getenv("X64ARM_ANGLE_TRACE")){std::fprintf(stderr,"[angle] %.*s",static_cast<int>(name.size()),name.data());for(auto value:values)std::fprintf(stderr," %llx",static_cast<unsigned long long>(value));std::fprintf(stderr,"\n");}
    ffi_call(&cif,FFI_FN(function),&result,arguments.data());
    if(std::getenv("X64ARM_ANGLE_TRACE"))for(const auto& block:blocks)if(block.output && block.guest){std::fprintf(stderr,"[angle-output]");for(std::size_t at=0;at+4<=std::min<std::size_t>(block.data.size(),64);at+=4){std::uint32_t word;std::memcpy(&word,block.data.data()+at,4);std::fprintf(stderr," %08x",word);}std::fprintf(stderr,"\n");}
    for(auto& block:blocks)if(block.output && block.guest){
        if(block.configs){int actual=0;for(unsigned i=0;i<count;++i)if(command.parameters[i].name=="num_config")std::memcpy(&actual,blocks[i].data.data(),4);const auto entries=std::min<std::size_t>(static_cast<unsigned>(std::max(actual,0)),block.guest_bytes/guest_pointer);for(std::size_t n=0;n<entries;++n){void* p;std::memcpy(&p,block.data.data()+n*8,8);memory.store(GuestAddress{block.guest+n*guest_pointer},static_cast<std::uint8_t>(guest_pointer*8),token(p,"config"));}}
        else if(block.attribs && x86){for(std::size_t n=0;n<block.guest_bytes/4;++n){std::uint64_t value;std::memcpy(&value,block.data.data()+n*8,8);memory.store(GuestAddress{block.guest+n*4},32,value);}}
        else memory.write(GuestAddress{block.guest},{block.data.data(),block.guest_bytes});bytes_out_+=block.guest_bytes;
    }
    if(name=="glMapBufferRange" && result){const auto length=bounded_product(original[2],1);BufferMapping mapping{0,reinterpret_cast<void*>(result),length,static_cast<std::size_t>(original[1]),static_cast<unsigned>(original[3]),static_cast<unsigned>(original[0])};std::vector<std::uint8_t> bytes(length);if(mapping.access&1)std::memcpy(bytes.data(),mapping.host,length);try{mapping.guest=allocate(bytes);}catch(const std::bad_alloc&){host<unsigned>("glUnmapBuffer",mapping.target);gl_error(current,0x505);return 0;}mappings_[{group,binding(mapping.target)}]=mapping;return mapping.guest;}
    if(name=="glDeleteBuffers" && original[1]){for(unsigned i=0;i<static_cast<unsigned>(original[0]);++i){const auto buffer=static_cast<unsigned>(memory.load(GuestAddress{original[1]+i*4},32));const auto key=std::pair{group,buffer};if(const auto found=mappings_.find(key);found!=mappings_.end()){release(found->second.guest);mappings_.erase(found);}}}
    if((name=="glDeleteQueries" || name=="glDeleteQueriesEXT") && original[1])for(unsigned i=0;i<static_cast<unsigned>(original[0]);++i)timestamps_.erase({current.context,static_cast<unsigned>(memory.load(GuestAddress{original[1]+i*4},32))});
    if(name=="glUnmapBuffer"){const auto key=std::pair{group,binding(static_cast<unsigned>(original[0]))};if(const auto found=mappings_.find(key);found!=mappings_.end()){release(found->second.guest);mappings_.erase(found);}}
    if(command.result==AngleType::String){if(!result)return 0;const auto* value=reinterpret_cast<const char*>(result);const auto key=std::string(value);auto found=strings_.find(key);if(found!=strings_.end())return found->second;const auto id=allocate({reinterpret_cast<const std::uint8_t*>(value),key.size()+1});strings_[key]=id;return id;}
    if(command.result==AngleType::Handle){std::string_view kind=name=="eglCreateContext"?"context":name=="glFenceSync"?"glsync":name.starts_with("eglCreateSync")?"eglsync":"surface";const auto id=token(reinterpret_cast<void*>(result),kind);if(id && kind=="context")contexts_[id]={static_cast<unsigned>(original[0]),original[2]?contexts_.at(static_cast<unsigned>(original[2])).group:id};if(id && kind=="surface")surfaces_[id].display=static_cast<unsigned>(original[0]);return id;}
    if(name=="eglMakeCurrent" && result){current.display=static_cast<unsigned>(original[0]);current.draw=static_cast<unsigned>(original[1]);current.read=static_cast<unsigned>(original[2]);current.context=static_cast<unsigned>(original[3]);}
    if(name=="eglBindAPI" && result)current.api=static_cast<unsigned>(original[0]);
    if(name=="eglReleaseThread" && result)current={};
    if(name=="eglTerminate" && result)counters_.erase(static_cast<unsigned>(original[0]));
    if(name=="eglDestroyContext" && result)contexts_.at(static_cast<unsigned>(original[1])).pending=true;
    if(name=="eglDestroySurface" && result)surfaces_.at(static_cast<unsigned>(original[1])).pending=true;
    if(name=="eglDestroyContext" || name=="eglDestroySurface" || name=="eglMakeCurrent" || name=="eglReleaseThread"){
        for(auto it=contexts_.begin();it!=contexts_.end();){const auto id=it->first;if(it->second.pending && std::none_of(current_.begin(),current_.end(),[&](const auto& c){return c.second.context==id;})){if(name!="eglDestroyContext" || original[1]!=id)host<unsigned>("eglDestroyContext",handle(it->second.display,"display"),handle(id,"context"));handles_.erase(id);handle_kinds_.erase(id);it=contexts_.erase(it);}else ++it;}
        for(auto it=surfaces_.begin();it!=surfaces_.end();){const auto id=it->first;++it;if(surfaces_.at(id).pending && std::none_of(current_.begin(),current_.end(),[&](const auto& c){return c.second.draw==id || c.second.read==id;})){if(name!="eglDestroySurface" || original[1]!=id)host<unsigned>("eglDestroySurface",handle(surfaces_.at(id).display,"display"),handle(id,"surface"));release_surface(id);handles_.erase(id);handle_kinds_.erase(id);}}
    }
    if(name=="glDeleteSync" || (name.starts_with("eglDestroySync") && result)){const auto id=static_cast<unsigned>(original[name=="glDeleteSync"?0:1]);handles_.erase(id);handle_kinds_.erase(id);}
    return result;
}}
}
std::unique_ptr<NativeAngle> make_native_angle(){
    const auto* choice=std::getenv("X64ARM_ANGLE_BACKEND");if(choice && std::string_view(choice)=="d3d11")return {};
    if(choice && std::string_view(choice)!="metal" && std::string_view(choice)!="auto")throw std::invalid_argument("ANGLE 後端須為 auto、metal 或 d3d11");
    std::filesystem::path path;if(const auto* directory=std::getenv("X64ARM_ANGLE_LIBRARY_DIR"))path=directory;
    else{std::uint32_t size=0;_NSGetExecutablePath(nullptr,&size);std::vector<char> executable(size);_NSGetExecutablePath(executable.data(),&size);path=std::filesystem::path(executable.data()).parent_path()/"angle";}
    if(!std::filesystem::exists(path/"libGLESv2.dylib")){if(choice && std::string_view(choice)=="metal")throw std::runtime_error("缺少 ANGLE Metal 函式庫，請設定 X64ARM_ANGLE_LIBRARY_DIR 或安裝 engine/angle");return {};}
    return std::make_unique<AngleMetal>(library_at(path));
}
}

#include "x64arm/windows_services.hpp"
#include "x64arm/process_identity.hpp"
#include "x64arm/windows_transform.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <type_traits>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace x64arm {
namespace {
constexpr std::size_t raster_limit=128U*1024U*1024U;
// 只傳遞已解析的裝置狀態與像素，不傳客體位址、GDI handle 或宿主物件。
struct RasterPacket {
    std::uint32_t magic{0x31524758U},version{1},width{},height{},pattern_width{},pattern_height{},excludes{};
    std::uint32_t clipped{},stretch{},blend{},operation{},transparent{};
    GuiRect destination{},source{},clip{};
    GuiTransform transform{};
    std::uint32_t color{},background{},pattern_color{},opaque{},monochrome{};
    std::int32_t hatch{};
    std::array<std::int32_t,2> brush_origin{};
};
static_assert(std::is_trivially_copyable_v<RasterPacket>);
std::uint64_t pixel_count(std::uint32_t width,std::uint32_t height){
    if(width>16384 || height>16384 || (!width!=!height))throw std::invalid_argument("遠端 raster 尺寸不合法");
    const auto count=std::uint64_t{width}*height;
    if(count>raster_limit/4)throw std::length_error("遠端 raster 像素超限");
    return count;
}
#if !defined(_WIN32)
struct RasterFile {
    int fd{-1};
    RasterFile(){
        auto path=(std::filesystem::temp_directory_path()/"x64arm-raster-XXXXXX").string();
        std::vector<char> name(path.begin(),path.end());name.push_back(0);
        fd=::mkstemp(name.data());
        if(fd>=0){::unlink(name.data());::fcntl(fd,F_SETFD,FD_CLOEXEC);}
    }
    ~RasterFile(){if(fd>=0)::close(fd);}
};
bool transfer_bytes(int fd,void* data,std::size_t size,bool writing){
    auto* bytes=static_cast<std::uint8_t*>(data);std::size_t done=0;
    while(done<size){
        const auto count=writing?::pwrite(fd,bytes+done,size-done,static_cast<off_t>(done)):
            ::pread(fd,bytes+done,size-done,static_cast<off_t>(done));
        if(count<0 && errno==EINTR)continue;
        if(count<=0)return false;done+=static_cast<std::size_t>(count);
    }
    return true;
}
void trace_raster_source(std::uint32_t pid,std::uint64_t hwnd,const GuiBitmap& image){
    static const auto prefix=[] {const auto* value=std::getenv("X64ARM_TRACE_REMOTE_RASTER");return value?std::string(value):std::string{};}();
    if(prefix.empty() || image.pixels.empty())return;
    try{
        const auto path=prefix+"-"+std::to_string(pid)+"-"+std::to_string(hwnd)+".ppm";
        const auto header="P6\n"+std::to_string(image.width)+" "+std::to_string(image.height)+"\n255\n";
        std::vector<std::uint8_t> bytes(header.begin(),header.end());bytes.reserve(header.size()+image.pixels.size()*3);
        std::size_t transparent=0,partial=0;
        for(auto pixel:image.pixels){for(unsigned channel=0;channel<3;++channel)bytes.push_back(static_cast<std::uint8_t>(pixel>>(channel*8)));transparent+=(pixel>>24)==0;partial+=(pixel>>24)>0 && (pixel>>24)<255;}
        const auto fd=::open(path.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
        const bool saved=fd>=0 && transfer_bytes(fd,bytes.data(),bytes.size(),true);if(fd>=0)::close(fd);
        std::fprintf(stderr,"[remote-raster-source] pid=%u hwnd=%llu size=%ux%u alpha_zero=%zu alpha_partial=%zu saved=%u\n",pid,static_cast<unsigned long long>(hwnd),image.width,image.height,transparent,partial,saved);
    }catch(const std::exception& error){std::fprintf(stderr,"[remote-raster-source] failed=%s\n",error.what());}
}
template<class T> void append(std::vector<std::uint8_t>& bytes,std::span<const T> data){
    const auto size=data.size_bytes();if(size>raster_limit-bytes.size())throw std::length_error("遠端 raster 封包超限");
    const auto begin=bytes.size();bytes.resize(begin+size);if(size)std::memcpy(bytes.data()+begin,data.data(),size);
}
std::shared_ptr<RasterSurface> image_surface(std::uint32_t width,std::uint32_t height,const std::uint8_t*& data){
    const auto count=static_cast<std::size_t>(pixel_count(width,height));if(!count)return {};
    std::vector<std::uint32_t> pixels(count);std::memcpy(pixels.data(),data,count*4);data+=count*4;
    auto surface=make_hardware_raster(width,height);surface->upload(pixels,true);surface->set_dib_alpha(true);return surface;
}
#endif
}

bool WindowsServices::blit_raster(GuestMemory& memory,std::uint64_t hdc,RasterSurface* source,
    const GuiRect& destination,const GuiRect& source_rect,const GuiDc& dc,std::uint32_t blend,
    const RasterBlit& options,RasterSurface* pattern){
    if(auto target=raster_surface(memory,hdc)){
        target->blit(source?*source:*target,destination,source_rect,dc,blend,options,pattern);return true;
    }
#if !defined(_WIN32)
    if(hdc<0x10000000 || !gui_.dc_objects.contains(hdc)){last_error_=gui_.contexts.contains(hdc)?50:6;return false;}
    const auto window=shared_gui_window(hdc-0x10000000,false);
    if(!window || window->process==process_id()){last_error_=1400;return false;}
    if(!allowed_gui_window(*window)){last_error_=5;return false;}
    const auto identity=process_identity::query(window->process);
    if(identity.error || identity.exited || !identity.identity){last_error_=1400;return false;}
    try{
        RasterPacket packet;packet.destination=destination;packet.source=source_rect;packet.transform=gui_device_transform(dc);
        packet.clip=dc.clip;packet.clipped=dc.clipped;packet.stretch=dc.stretch_mode;packet.blend=blend;
        packet.operation=options.operation;packet.transparent=options.transparent;
        packet.color=options.brush.color;packet.background=options.brush.pattern_background;packet.pattern_color=options.brush.pattern_color;
        packet.opaque=options.brush.pattern_opaque;packet.monochrome=options.brush.pattern_monochrome;
        packet.hatch=options.brush.hatch;packet.brush_origin=options.brush.brush_origin;
        if(dc.excluded.size()>65536){last_error_=8;return false;}packet.excludes=static_cast<std::uint32_t>(dc.excluded.size());
        GuiBitmap image,brush;if(source){image=source->snapshot();packet.width=image.width;packet.height=image.height;}
        trace_raster_source(process_id(),window->handle,image);
        if(pattern){brush=pattern->snapshot();packet.pattern_width=brush.width;packet.pattern_height=brush.height;}
        std::vector<std::uint8_t> bytes;append(bytes,std::span<const RasterPacket>(&packet,1));
        append(bytes,std::span<const GuiRect>(dc.excluded));append(bytes,std::span<const std::uint32_t>(image.pixels));append(bytes,std::span<const std::uint32_t>(brush.pixels));
        RasterFile file;if(file.fd<0 || !transfer_bytes(file.fd,bytes.data(),bytes.size(),true)){last_error_=8;return false;}
        std::array<std::uint64_t,5> values{window->handle,bytes.size(),0,0,process_identity_birth()};std::uint32_t mode=0;
        const auto error=remote_process_request(memory,window->process,identity.identity.birth,23,1,values,mode,file.fd);
        if(error){last_error_=error;return false;}return true;
    }catch(const std::bad_alloc&){last_error_=8;}catch(const std::length_error&){last_error_=8;}
#else
    (void)memory;(void)source;(void)destination;(void)source_rect;(void)dc;(void)blend;(void)options;(void)pattern;last_error_=50;
#endif
    return false;
}

std::shared_ptr<RasterSurface> WindowsServices::remote_raster_snapshot(GuestMemory& memory,std::uint64_t hdc){
#if !defined(_WIN32)
    if(hdc<0x10000000 || !gui_.dc_objects.contains(hdc))return {};
    const auto window=shared_gui_window(hdc-0x10000000,false);
    if(!window || window->process==process_id()){last_error_=1400;return {};}
    if(!allowed_gui_window(*window)){last_error_=5;return {};}
    const auto identity=process_identity::query(window->process);
    if(identity.error || identity.exited || !identity.identity){last_error_=1400;return {};}
    RasterFile file;if(file.fd<0){last_error_=8;return {};}
    std::array<std::uint64_t,5> values{window->handle,0,0,0,process_identity_birth()};std::uint32_t mode=0;
    const auto error=remote_process_request(memory,window->process,identity.identity.birth,23,2,values,mode,file.fd);
    if(error){last_error_=error;return {};}
    if(!values[2] || !values[3] || values[2]>16384 || values[3]>16384 || values[1]>raster_limit || values[1]!=values[2]*values[3]*4){last_error_=13;return {};}
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(values[1]));
    if(!transfer_bytes(file.fd,bytes.data(),bytes.size(),false)){last_error_=13;return {};}
    const auto* data=bytes.data();return image_surface(static_cast<std::uint32_t>(values[2]),static_cast<std::uint32_t>(values[3]),data);
#else
    (void)memory;(void)hdc;return {};
#endif
}

std::uint32_t WindowsServices::receive_remote_raster(GuestMemory& memory,std::uint32_t caller,std::uint32_t operation,
    std::array<std::uint64_t,5>& values,int descriptor){
#if !defined(_WIN32)
    if(!process_identity::query(caller).live({caller,values[4]}))return 87;
    const auto window=gui_.windows.find(values[0]);if(window==gui_.windows.end())return 1400;
    struct stat status{};
    if(descriptor<0 || values[1]>raster_limit || ::fstat(descriptor,&status) || !S_ISREG(status.st_mode) ||
       status.st_uid!=::geteuid() || (status.st_mode&077) || status.st_nlink || status.st_size<0 ||
       static_cast<std::uint64_t>(status.st_size)!=values[1])return 13;
    if(operation!=1 && operation!=2)return 87;
    if(operation==2 && window->second.display_affinity==0x11)return 5;
    try{
        // 合成與讀回都在 HWND owner 執行；局部更新不會覆蓋其他程序剛畫出的像素。
        poll_remote_frames();const auto hdc=0x10000000+values[0];gui_.contexts.try_emplace(hdc);
        const auto target=raster_surface(memory,hdc);if(!target)return 50;
        if(operation==2){
            auto image=target->snapshot();if(window->second.display_affinity==1)std::fill(image.pixels.begin(),image.pixels.end(),0xff000000U);
            values[1]=image.pixels.size()*4;values[2]=image.width;values[3]=image.height;
            if(values[1]>raster_limit || ::ftruncate(descriptor,static_cast<off_t>(values[1])) || !transfer_bytes(descriptor,image.pixels.data(),static_cast<std::size_t>(values[1]),true))return 8;
            return 0;
        }
        if(values[1]<sizeof(RasterPacket))return 13;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(values[1]));if(!transfer_bytes(descriptor,bytes.data(),bytes.size(),false))return 13;
        RasterPacket packet;std::memcpy(&packet,bytes.data(),sizeof(packet));
        if(packet.magic!=RasterPacket{}.magic || packet.version!=1 || packet.excludes>65536 || packet.clipped>1 ||
           packet.opaque>1 || packet.monochrome>1 || packet.operation>255 || packet.stretch<1 || packet.stretch>4 || !gui_valid_transform(packet.transform))return 13;
        const auto pixels=pixel_count(packet.width,packet.height),pattern_pixels=pixel_count(packet.pattern_width,packet.pattern_height);
        if(sizeof(packet)+std::uint64_t{packet.excludes}*sizeof(GuiRect)+(pixels+pattern_pixels)*4!=bytes.size())return 13;
        if(((packet.operation^(packet.operation>>2))&0x33U) && !pixels)return 13;
        GuiDc dc;dc.world=packet.transform;dc.graphics_mode=2;dc.clipped=packet.clipped;dc.clip=packet.clip;dc.stretch_mode=packet.stretch;
        dc.excluded.resize(packet.excludes);const auto* data=bytes.data()+sizeof(packet);
        if(packet.excludes)std::memcpy(dc.excluded.data(),data,packet.excludes*sizeof(GuiRect));data+=packet.excludes*sizeof(GuiRect);
        const auto source=image_surface(packet.width,packet.height,data),pattern=image_surface(packet.pattern_width,packet.pattern_height,data);
        RasterBlit options;options.operation=packet.operation;options.transparent=packet.transparent;
        auto& brush=options.brush;brush.color=packet.color;brush.pattern_background=packet.background;brush.pattern_color=packet.pattern_color;
        brush.pattern_opaque=packet.opaque;brush.pattern_monochrome=packet.monochrome;brush.hatch=packet.hatch;brush.brush_origin=packet.brush_origin;
        target->blit(source?*source:*target,packet.destination,packet.source,dc,packet.blend,options,pattern.get());
        target->flush(true);return 0;
    }catch(const std::bad_alloc&){return 8;}catch(const std::length_error&){return 8;}catch(const std::invalid_argument&){return 87;}catch(const std::runtime_error&){return 50;}
#else
    (void)memory;(void)caller;(void)operation;(void)values;(void)descriptor;return 50;
#endif
}
}

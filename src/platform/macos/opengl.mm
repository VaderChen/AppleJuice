#include "opengl_backend.hpp"
#include "x64arm/windows_opengl.hpp"
#include "x64arm/gpu_memory.hpp"
#include "x64arm/resource_usage.hpp"
#import <AppKit/AppKit.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl.h>
#import <OpenGL/glext.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
namespace x64arm {
namespace {
std::map<void*,std::size_t> view_users;
void retain_view(NSView* view){if(!view)return;auto& count=view_users[(__bridge void*)view];if(count++==0 && [view respondsToSelector:NSSelectorFromString(@"setOpenGLManaged:")])[view setValue:@YES forKey:@"openGLManaged"];}
void release_view(NSView* view){if(!view)return;const auto it=view_users.find((__bridge void*)view);if(it!=view_users.end() && --it->second==0){view_users.erase(it);if([view respondsToSelector:NSSelectorFromString(@"setOpenGLManaged:")]){[view setValue:@NO forKey:@"openGLManaged"];[view setNeedsDisplay:YES];}}}
class MacOpenGl final:public NativeOpenGlContext {
    NSOpenGLContext* context_;
    NSView* view_{};
    GLuint framebuffer_{},color_{},depth_{};
    std::uint32_t width_{},height_{};
    OpenGlRendererInfo info_;
    struct Upload {GLuint buffer{};std::size_t capacity{};std::shared_ptr<GpuMemoryReservation> memory;};
    std::array<Upload,4> arrays_;
    std::shared_ptr<GpuMemoryReservation> surface_memory_;
    std::map<GLuint,std::map<GLint,std::shared_ptr<GpuMemoryReservation>>> texture_memory_;
    std::array<GLenum,8> saved_errors_{};std::size_t saved_error_count_{};
    bool list_definition_{},compiling_list_{},primitive_open_{},pending_end_{},primitive_batchable_{};
    const bool list_batch_enabled_=std::getenv("X64ARM_GL_DISABLE_LIST_BATCHING")==nullptr;
    GLenum primitive_mode_{};
    std::uint64_t primitive_vertices_{},list_groups_{},merged_boundaries_{};
    void remember_error(GLenum value){if(value!=GL_NO_ERROR && std::find(saved_errors_.begin(),saved_errors_.begin()+static_cast<std::ptrdiff_t>(saved_error_count_),value)==saved_errors_.begin()+static_cast<std::ptrdiff_t>(saved_error_count_) && saved_error_count_<saved_errors_.size())saved_errors_[saved_error_count_++]=value;}
    bool preserve_driver_errors(){bool failed=false;for(auto value=glGetError();value!=GL_NO_ERROR;value=glGetError()){remember_error(value);failed=true;}return failed;}
    GLenum next_error(){if(!saved_error_count_)return glGetError();const auto value=saved_errors_[0];for(std::size_t i=1;i<saved_error_count_;++i)saved_errors_[i-1]=saved_errors_[i];--saved_error_count_;return value;}
    static unsigned texture_pixel_bytes(GLint format){switch(format){
        case 1:case 2:case 3:case 4:case GL_RGB:case GL_RGBA:return 4;
        case GL_ALPHA:case GL_LUMINANCE:case GL_INTENSITY:case GL_ALPHA4:case GL_ALPHA8:case GL_LUMINANCE4:case GL_LUMINANCE8:case GL_INTENSITY4:case GL_INTENSITY8:case GL_LUMINANCE4_ALPHA4:case GL_LUMINANCE6_ALPHA2:case GL_R3_G3_B2:case GL_RGBA2:return 1;
        case GL_LUMINANCE_ALPHA:case GL_ALPHA12:case GL_ALPHA16:case GL_LUMINANCE12:case GL_LUMINANCE16:case GL_INTENSITY12:case GL_INTENSITY16:case GL_LUMINANCE8_ALPHA8:case GL_LUMINANCE12_ALPHA4:case GL_RGB4:case GL_RGB5:case GL_RGBA4:case GL_RGB5_A1:case GL_DEPTH_COMPONENT16:return 2;
        case GL_LUMINANCE12_ALPHA12:case GL_LUMINANCE16_ALPHA16:case GL_RGB8:case GL_RGB10:case GL_RGBA8:case GL_RGB10_A2:case GL_DEPTH_COMPONENT:case GL_DEPTH_COMPONENT24:case GL_DEPTH_COMPONENT32:return 4;
        case GL_RGB12:case GL_RGB16:case GL_RGBA12:case GL_RGBA16:return 8;
        case GL_ALPHA16F_ARB:case GL_INTENSITY16F_ARB:case GL_LUMINANCE16F_ARB:return 2;
        case GL_LUMINANCE_ALPHA16F_ARB:case GL_ALPHA32F_ARB:case GL_INTENSITY32F_ARB:case GL_LUMINANCE32F_ARB:return 4;
        case GL_RGB16F_ARB:case GL_RGBA16F_ARB:case GL_LUMINANCE_ALPHA32F_ARB:return 8;
        case GL_RGB32F_ARB:case GL_RGBA32F_ARB:return 16;
        case GL_SRGB:case GL_SRGB8:case GL_SRGB_ALPHA:case GL_SRGB8_ALPHA8:return 4;
        case GL_SLUMINANCE:case GL_SLUMINANCE8:return 1;case GL_SLUMINANCE_ALPHA:case GL_SLUMINANCE8_ALPHA8:return 2;
        default:return 0;}}
    static unsigned primitive_size(GLenum mode){switch(mode){case GL_POINTS:return 1;case GL_LINES:return 2;case GL_TRIANGLES:return 3;case GL_QUADS:return 4;default:return 0;}}
    static bool primitive_attribute(GlOperation op){switch(op){
        case GlOperation::Normal3f:case GlOperation::Normal3fv:case GlOperation::Normal3dv:
        case GlOperation::Color3f:case GlOperation::Color4f:case GlOperation::Color4fv:case GlOperation::Color4dv:
        case GlOperation::TexCoord2f:case GlOperation::TexCoord2fv:case GlOperation::TexCoord4fv:case GlOperation::TexCoord4dv:return true;
        default:return false;}}
    static bool primitive_vertex(GlOperation op){switch(op){case GlOperation::Vertex2f:case GlOperation::Vertex2i:case GlOperation::Vertex3f:case GlOperation::Vertex3fv:case GlOperation::Vertex4fv:case GlOperation::Vertex4dv:return true;default:return false;}}
    void finish_pending_primitive(){if(pending_end_){glEnd();pending_end_=false;primitive_open_=false;}}
    void release_surface(){if(framebuffer_)glDeleteFramebuffersEXT(1,&framebuffer_);if(color_)glDeleteRenderbuffersEXT(1,&color_);if(depth_)glDeleteRenderbuffersEXT(1,&depth_);framebuffer_=color_=depth_=0;surface_memory_.reset();}
public:
    std::string fallback_reason_;
    MacOpenGl(void* view,std::uint32_t width,std::uint32_t height,std::string reason={}):fallback_reason_(std::move(reason)){
        @autoreleasepool {
            const NSOpenGLPixelFormatAttribute attributes[]{NSOpenGLPFAOpenGLProfile,NSOpenGLProfileVersionLegacy,
                NSOpenGLPFAAccelerated,NSOpenGLPFANoRecovery,NSOpenGLPFADoubleBuffer,
                NSOpenGLPFAColorSize,32,NSOpenGLPFAAlphaSize,8,NSOpenGLPFADepthSize,24,NSOpenGLPFAStencilSize,8,0};
            auto* format=[[NSOpenGLPixelFormat alloc] initWithAttributes:attributes];
            if(!format)throw std::runtime_error("找不到硬體加速 OpenGL legacy pixel format");
            GLint accelerated=0;[format getValues:&accelerated forAttribute:NSOpenGLPFAAccelerated forVirtualScreen:0];
            if(!accelerated)throw std::runtime_error("OpenGL renderer 未使用硬體加速");
            context_=[[NSOpenGLContext alloc] initWithFormat:format shareContext:nil];
            if(!context_)throw std::runtime_error("無法建立硬體 OpenGL context");
            if(!swap_interval(1))throw std::runtime_error("無法設定 OpenGL 預設 swap interval");
            bind();info_={string(GL_VENDOR),string(GL_RENDERER),string(GL_VERSION),true};
            drawable(view,width,height);
            mark_gpu_timing_unavailable();
        }
    }
    ~MacOpenGl()override{@autoreleasepool{bind();finish_pending_primitive();glFinish();for(const auto& a:arrays_)if(a.buffer)glDeleteBuffers(1,&a.buffer);release_surface();release_view(view_);[context_ clearDrawable];[NSOpenGLContext clearCurrentContext];}}
    void bind()override{if(CGLGetCurrentContext()!=context_.CGLContextObj && CGLSetCurrentContext(context_.CGLContextObj)!=kCGLNoError)throw std::runtime_error("無法切換 OpenGL context");}
    void drawable(void* opaque,std::uint32_t width,std::uint32_t height)override{
        if(!width || !height || width>16384 || height>16384)throw std::runtime_error("OpenGL drawable 尺寸超出支援範圍");
        @autoreleasepool {
            bind();finish_pending_primitive();NSView* view=(__bridge NSView*)opaque;
            if(view){
                if(view_!=view || width_!=width || height_!=height){
                    // 雙 RGBA drawable + depth/stencil 的保守容量；driver metadata 另有不可量測開銷。
                    auto reservation=reserve_gpu_memory(static_cast<std::size_t>(width)*height*12);
                    glFinish();
                    if(view_!=view){release_surface();release_view(view_);retain_view(view);[context_ setView:view];view_=view;glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,0);glDrawBuffer(GL_BACK);glReadBuffer(GL_BACK);}
                    surface_memory_=std::move(reservation);[context_ update];
                }
            }
            else if(view_ || width_!=width || height_!=height){
                auto reservation=reserve_gpu_memory(static_cast<std::size_t>(width)*height*8);
                [context_ clearDrawable];release_view(view_);view_=nil;glFinish();release_surface();
                surface_memory_=std::move(reservation);
                glGenFramebuffersEXT(1,&framebuffer_);glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,framebuffer_);
                glGenRenderbuffersEXT(1,&color_);glBindRenderbufferEXT(GL_RENDERBUFFER_EXT,color_);glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT,GL_RGBA8,static_cast<GLsizei>(width),static_cast<GLsizei>(height));
                glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_RENDERBUFFER_EXT,color_);
                glGenRenderbuffersEXT(1,&depth_);glBindRenderbufferEXT(GL_RENDERBUFFER_EXT,depth_);glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT,GL_DEPTH24_STENCIL8,static_cast<GLsizei>(width),static_cast<GLsizei>(height));
                glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT,GL_DEPTH_ATTACHMENT_EXT,GL_RENDERBUFFER_EXT,depth_);glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT,GL_STENCIL_ATTACHMENT_EXT,GL_RENDERBUFFER_EXT,depth_);
                if(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT)!=GL_FRAMEBUFFER_COMPLETE_EXT)throw std::runtime_error("硬體 OpenGL framebuffer 不完整");
                glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
            }
            if(!width_ || !height_)glViewport(0,0,static_cast<GLsizei>(width),static_cast<GLsizei>(height));width_=width;height_=height;
        }
    }
    bool swap()override{bind();finish_pending_primitive();if(view_)[context_ flushBuffer];else glFlush();return true;}
    bool swap_interval(std::int32_t interval)override{if(interval<0 || interval>1)return false;GLint value=interval;return CGLSetParameter(context_.CGLContextObj,kCGLCPSwapInterval,&value)==kCGLNoError;}
    OpenGlRendererInfo info()const override{return info_;}
    std::string statistics()const override{return "opengl_backend=legacy opengl_fallback="+std::string(fallback_reason_.empty()?"0":"1")+(fallback_reason_.empty()?"\n":" opengl_fallback_reason="+fallback_reason_+"\n")+"opengl_list_primitive_groups="+std::to_string(list_groups_)+" opengl_list_merged_boundaries="+std::to_string(merged_boundaries_)+" opengl_list_batching="+(list_batch_enabled_?"1":"0")+"\n";}
    std::string capture(const std::filesystem::path& path)override{
        @autoreleasepool {
            bind();finish_pending_primitive();GLint previous=0;glGetIntegerv(GL_READ_BUFFER,&previous);glReadBuffer(view_?GL_FRONT:GL_COLOR_ATTACHMENT0_EXT);
            glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);glPixelStorei(GL_PACK_ALIGNMENT,1);glPixelStorei(GL_PACK_ROW_LENGTH,0);glPixelStorei(GL_PACK_SKIP_PIXELS,0);glPixelStorei(GL_PACK_SKIP_ROWS,0);glPixelStorei(GL_PACK_SWAP_BYTES,0);
            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width_)*height_*4);glReadPixels(0,0,static_cast<GLsizei>(width_),static_cast<GLsizei>(height_),GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
            glPopClientAttrib();glReadBuffer(static_cast<GLenum>(previous));
            std::uint64_t hash=1469598103934665603ULL,nonblack=0;for(std::size_t i=0;i<pixels.size();++i){hash^=pixels[i];hash*=1099511628211ULL;if(i%4==0&&(pixels[i]||pixels[i+1]||pixels[i+2]))++nonblack;}
            auto* bitmap=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:width_ pixelsHigh:height_ bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:NSBitmapFormatAlphaNonpremultiplied bytesPerRow:static_cast<NSInteger>(width_)*4 bitsPerPixel:32];
            for(std::uint32_t y=0;y<height_;++y)std::memcpy(bitmap.bitmapData+static_cast<std::size_t>(y)*width_*4,pixels.data()+static_cast<std::size_t>(height_-y-1)*width_*4,static_cast<std::size_t>(width_)*4);
            auto* png=[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];std::filesystem::create_directories(path.parent_path());
            if(![png writeToFile:[NSString stringWithUTF8String:path.c_str()] atomically:YES])throw std::runtime_error("OpenGL GPU readback PNG 寫入失敗");
            return "opengl_capture="+path.string()+" nonblack_pixels="+std::to_string(nonblack)+" fnv1a="+std::to_string(hash)+"\n";
        }
    }
    std::string string(std::uint32_t name)override{bind();finish_pending_primitive();const auto* p=glGetString(name);return p?reinterpret_cast<const char*>(p):"";}
    void array(unsigned index,std::int32_t size,std::uint32_t type,std::span<const std::uint8_t> bytes,bool enabled)override{
        bind();finish_pending_primitive();constexpr GLenum names[]{GL_VERTEX_ARRAY,GL_NORMAL_ARRAY,GL_COLOR_ARRAY,GL_TEXTURE_COORD_ARRAY};
        if(index>=4)throw std::runtime_error("OpenGL client array index 不合法");
        if(!enabled){glDisableClientState(names[index]);return;}
        auto& buffer=arrays_[index];
        if(!buffer.buffer)glGenBuffers(1,&buffer.buffer);
        glBindBuffer(GL_ARRAY_BUFFER,buffer.buffer);
        if(bytes.size()>buffer.capacity){glFinish();const auto capacity=(bytes.size()+65535U)/65536U*65536U;auto reservation=reserve_gpu_memory(capacity);glBufferData(GL_ARRAY_BUFFER,static_cast<GLsizeiptr>(capacity),nullptr,GL_STREAM_DRAW);buffer.memory=std::move(reservation);buffer.capacity=capacity;}
        if(!bytes.empty())glBufferSubData(GL_ARRAY_BUFFER,0,static_cast<GLsizeiptr>(bytes.size()),bytes.data());
        const void* data=nullptr;
        switch(index){case 0:glVertexPointer(size,type,0,data);break;case 1:glNormalPointer(type,0,data);break;case 2:glColorPointer(size,type,0,data);break;case 3:glTexCoordPointer(size,type,0,data);break;default:break;}
        glEnableClientState(names[index]);glBindBuffer(GL_ARRAY_BUFFER,0);
    }
    std::uint64_t execute(GlOperation op,std::span<const std::uint64_t> a,std::span<const std::uint8_t> data,std::span<std::uint8_t> output)override{
        bind();const auto u=[&](unsigned i){return static_cast<GLuint>(a[i]);};const auto n=[&](unsigned i){return std::bit_cast<GLint>(u(i));};
        // 只在 GL_COMPILE 中延後完整獨立圖元的 End；狀態、查詢、未知內容與不完整尾端保留原界線。
        if(pending_end_){
            if(op==GlOperation::Begin && u(0)==primitive_mode_){pending_end_=false;primitive_vertices_=0;++merged_boundaries_;return 0;}
            if(!primitive_attribute(op))finish_pending_primitive();
        }
        if(primitive_open_ && !pending_end_ && primitive_vertex(op))++primitive_vertices_;
        if(primitive_open_ && op==GlOperation::CallList)primitive_batchable_=false;
        const auto f=[&](unsigned i){return std::bit_cast<GLfloat>(u(i));};const auto d=[&](unsigned i){return std::bit_cast<GLdouble>(a[i]);};
        const auto* fv=reinterpret_cast<const GLfloat*>(data.data());const auto* dv=reinterpret_cast<const GLdouble*>(data.data());
        switch(op){
        case GlOperation::Begin:glBegin(u(0));primitive_open_=true;primitive_mode_=u(0);primitive_vertices_=0;primitive_batchable_=list_batch_enabled_ && compiling_list_ && primitive_size(primitive_mode_)!=0;break;
        case GlOperation::End:
            if(compiling_list_)++list_groups_;
            if(primitive_batchable_ && primitive_vertices_%primitive_size(primitive_mode_)==0)pending_end_=true;
            else {glEnd();primitive_open_=false;}break;
        case GlOperation::Vertex2f:glVertex2f(f(0),f(1));break;case GlOperation::Vertex2i:glVertex2i(n(0),n(1));break;
        case GlOperation::Vertex3f:glVertex3f(f(0),f(1),f(2));break;case GlOperation::Vertex3fv:glVertex3fv(fv);break;case GlOperation::Vertex4fv:glVertex4fv(fv);break;
        case GlOperation::Normal3f:glNormal3f(f(0),f(1),f(2));break;case GlOperation::Normal3fv:glNormal3fv(fv);break;
        case GlOperation::Color3f:glColor3f(f(0),f(1),f(2));break;case GlOperation::Color4f:glColor4f(f(0),f(1),f(2),f(3));break;case GlOperation::Color4fv:glColor4fv(fv);break;
        case GlOperation::TexCoord2f:glTexCoord2f(f(0),f(1));break;case GlOperation::TexCoord2fv:glTexCoord2fv(fv);break;case GlOperation::TexCoord4fv:glTexCoord4fv(fv);break;
        case GlOperation::Color4dv:glColor4dv(dv);break;case GlOperation::Normal3dv:glNormal3dv(dv);break;case GlOperation::TexCoord4dv:glTexCoord4dv(dv);break;case GlOperation::Vertex4dv:glVertex4dv(dv);break;
        case GlOperation::Clear:glClear(u(0));break;case GlOperation::ClearColor:glClearColor(f(0),f(1),f(2),f(3));break;case GlOperation::ClearDepth:glClearDepth(d(0));break;
        case GlOperation::Enable:glEnable(u(0));break;case GlOperation::Disable:glDisable(u(0));break;case GlOperation::CullFace:glCullFace(u(0));break;case GlOperation::FrontFace:glFrontFace(u(0));break;case GlOperation::BlendFunc:glBlendFunc(u(0),u(1));break;
        case GlOperation::StencilFunc:glStencilFunc(u(0),n(1),u(2));break;
        case GlOperation::StencilMask:glStencilMask(u(0));break;
        case GlOperation::StencilOp:glStencilOp(u(0),u(1),u(2));break;
        case GlOperation::ClearStencil:glClearStencil(n(0));break;
        case GlOperation::AlphaFunc:glAlphaFunc(u(0),f(1));break;
        case GlOperation::DepthFunc:glDepthFunc(u(0));break;case GlOperation::DepthRange:glDepthRange(d(0),d(1));break;case GlOperation::DepthMask:glDepthMask(static_cast<GLboolean>(u(0)));break;
        case GlOperation::ColorMask:glColorMask(static_cast<GLboolean>(u(0)!=0),static_cast<GLboolean>(u(1)!=0),static_cast<GLboolean>(u(2)!=0),static_cast<GLboolean>(u(3)!=0));break;
        case GlOperation::LineWidth:glLineWidth(f(0));break;case GlOperation::PixelZoom:glPixelZoom(f(0),f(1));break;
        case GlOperation::RenderMode:return static_cast<std::uint32_t>(glRenderMode(u(0)));
        case GlOperation::ShadeModel:glShadeModel(u(0));break;case GlOperation::Hint:glHint(u(0),u(1));break;
        case GlOperation::Viewport:glViewport(n(0),n(1),n(2),n(3));break;case GlOperation::Scissor:glScissor(n(0),n(1),n(2),n(3));break;
        case GlOperation::MatrixMode:glMatrixMode(u(0));break;case GlOperation::LoadIdentity:glLoadIdentity();break;
        case GlOperation::LoadMatrixf:glLoadMatrixf(fv);break;case GlOperation::MultMatrixf:glMultMatrixf(fv);break;
        case GlOperation::PushMatrix:glPushMatrix();break;case GlOperation::PopMatrix:glPopMatrix();break;
        case GlOperation::Translatef:glTranslatef(f(0),f(1),f(2));break;case GlOperation::Rotatef:glRotatef(f(0),f(1),f(2),f(3));break;case GlOperation::Scalef:glScalef(f(0),f(1),f(2));break;
        case GlOperation::Ortho:glOrtho(d(0),d(1),d(2),d(3),d(4),d(5));break;case GlOperation::Frustum:glFrustum(d(0),d(1),d(2),d(3),d(4),d(5));break;
        case GlOperation::Perspective:{const auto h=d(2)*std::tan(d(0)*3.14159265358979323846/360.0);glFrustum(-h*d(1),h*d(1),-h,h,d(2),d(3));break;}
        case GlOperation::Lightfv:glLightfv(u(0),u(1),fv);break;case GlOperation::Materialf:glMaterialf(u(0),u(1),f(2));break;case GlOperation::Materialfv:glMaterialfv(u(0),u(1),fv);break;
        case GlOperation::Fogf:glFogf(u(0),f(1));break;case GlOperation::Fogi:glFogi(u(0),n(1));break;case GlOperation::Fogfv:glFogfv(u(0),fv);break;
        case GlOperation::PushAttrib:glPushAttrib(u(0));break;case GlOperation::PopAttrib:glPopAttrib();break;
        case GlOperation::PushClientAttrib:glPushClientAttrib(u(0));break;case GlOperation::PopClientAttrib:glPopClientAttrib();break;
        case GlOperation::TexEnvi:glTexEnvi(u(0),u(1),n(2));break;
        case GlOperation::TexEnvf:glTexEnvf(u(0),u(1),f(2));break;
        case GlOperation::TexEnviv:glTexEnviv(u(0),u(1),reinterpret_cast<const GLint*>(data.data()));break;
        case GlOperation::TexEnvfv:glTexEnvfv(u(0),u(1),fv);break;
        case GlOperation::GetTexEnviv:glGetTexEnviv(u(0),u(1),reinterpret_cast<GLint*>(output.data()));break;
        case GlOperation::GetTexEnvfv:glGetTexEnvfv(u(0),u(1),reinterpret_cast<GLfloat*>(output.data()));break;
        case GlOperation::TextureCombine:throw std::invalid_argument("D3D texture combine 僅提供 Metal 後端");
        case GlOperation::GetTexLevelParameteriv:glGetTexLevelParameteriv(u(0),n(1),u(2),reinterpret_cast<GLint*>(output.data()));break;
        case GlOperation::GetTexLevelParameterfv:glGetTexLevelParameterfv(u(0),n(1),u(2),reinterpret_cast<GLfloat*>(output.data()));break;
        case GlOperation::BindTexture:glBindTexture(u(0),u(1));break;
        case GlOperation::IsTexture:return glIsTexture(u(0));
        case GlOperation::GenTextures:glGenTextures(n(0),reinterpret_cast<GLuint*>(output.data()));break;
        case GlOperation::DeleteTextures:{glFinish();const auto* ids=reinterpret_cast<const GLuint*>(data.data());glDeleteTextures(n(0),ids);for(GLint i=0;i<n(0);++i)if(ids[i])texture_memory_.erase(ids[i]);break;}
        case GlOperation::TexImage2D:{
            preserve_driver_errors();
            // List 內的 TexImage 會在 replay 才修改物件，尚不能追蹤此資源生命週期；明確拒絕，不假記帳。
            if(list_definition_){remember_error(GL_INVALID_OPERATION);break;}
            if(u(0)==GL_PROXY_TEXTURE_2D){glTexImage2D(u(0),n(1),n(2),n(3),n(4),n(5),u(6),u(7),nullptr);preserve_driver_errors();break;}
            if(u(0)!=GL_TEXTURE_2D){remember_error(GL_INVALID_ENUM);break;}
            GLint maximum=0;glGetIntegerv(GL_MAX_TEXTURE_SIZE,&maximum);const auto pixel_bytes=texture_pixel_bytes(n(2));
            if(!pixel_bytes || n(1)<0 || n(1)>static_cast<GLint>(std::bit_width(static_cast<unsigned>(maximum))-1) || n(5)<0 || n(5)>1 || n(3)<0 || n(4)<0 || n(3)>maximum+2*n(5) || n(4)>maximum+2*n(5)){remember_error(GL_INVALID_VALUE);break;}
            GLint binding=0;glGetIntegerv(GL_TEXTURE_BINDING_2D,&binding);
            std::shared_ptr<GpuMemoryReservation> lease;
            if(n(3)>0 && n(4)>0)lease=reserve_gpu_memory(static_cast<std::size_t>(n(3))*static_cast<std::size_t>(n(4))*pixel_bytes);
            auto& levels=texture_memory_[static_cast<GLuint>(binding)];const auto [slot,inserted]=levels.try_emplace(n(1),nullptr);
            glTexImage2D(u(0),n(1),n(2),n(3),n(4),n(5),u(6),u(7),data.empty()?nullptr:data.data());
            if(!preserve_driver_errors()){
                if(slot->second)glFinish();slot->second=std::move(lease);if(!slot->second)levels.erase(slot);
            }else if(inserted)levels.erase(slot);
            if(levels.empty())texture_memory_.erase(static_cast<GLuint>(binding));break;}
        case GlOperation::TexSubImage2D:
            if(list_definition_){remember_error(GL_INVALID_OPERATION);break;}
            if(data.empty() && n(4)>0 && n(5)>0){remember_error(GL_INVALID_OPERATION);break;}
            glTexSubImage2D(u(0),n(1),n(2),n(3),n(4),n(5),u(6),u(7),data.empty()?nullptr:data.data());break;
        case GlOperation::TexParameterf:glTexParameterf(u(0),u(1),f(2));break;case GlOperation::TexParameteri:glTexParameteri(u(0),u(1),n(2));break;
        case GlOperation::PixelStorei:glPixelStorei(u(0),n(1));break;
        case GlOperation::NewList:{glNewList(u(0),u(1));GLint index=0,mode=0;glGetIntegerv(GL_LIST_INDEX,&index);glGetIntegerv(GL_LIST_MODE,&mode);list_definition_=index!=0;compiling_list_=list_definition_ && mode==GL_COMPILE;break;}
        case GlOperation::EndList:glEndList();list_definition_=false;compiling_list_=false;break;case GlOperation::GenLists:return glGenLists(n(0));
        case GlOperation::DeleteLists:glDeleteLists(u(0),n(1));break;case GlOperation::CallList:glCallList(u(0));break;
        case GlOperation::DrawArrays:glDrawArrays(u(0),n(1),n(2));break;case GlOperation::DrawElements:glDrawElements(u(0),n(1),u(2),data.data());break;
        case GlOperation::Bitmap:glBitmap(n(0),n(1),f(2),f(3),f(4),f(5),data.empty()?nullptr:data.data());break;
        case GlOperation::RasterPos2i:glRasterPos2i(n(0),n(1));break;case GlOperation::RasterPos3f:glRasterPos3f(f(0),f(1),f(2));break;
        case GlOperation::DrawBuffer:glDrawBuffer(!view_&&(u(0)==GL_FRONT||u(0)==GL_BACK||u(0)==GL_FRONT_AND_BACK)?GL_COLOR_ATTACHMENT0_EXT:u(0));break;
        case GlOperation::ReadBuffer:glReadBuffer(!view_&&(u(0)==GL_FRONT||u(0)==GL_BACK)?GL_COLOR_ATTACHMENT0_EXT:u(0));break;
        case GlOperation::GetError:return next_error();
        case GlOperation::GetIntegerv:glGetIntegerv(u(0),reinterpret_cast<GLint*>(output.data()));break;
        case GlOperation::GetFloatv:glGetFloatv(u(0),reinterpret_cast<GLfloat*>(output.data()));break;
        case GlOperation::GetDoublev:glGetDoublev(u(0),reinterpret_cast<GLdouble*>(output.data()));break;
        case GlOperation::GetBooleanv:glGetBooleanv(u(0),output.data());break;
        case GlOperation::IsEnabled:return glIsEnabled(u(0));
        case GlOperation::ReadPixels:glReadPixels(n(0),n(1),n(2),n(3),u(4),u(5),output.data());break;
        case GlOperation::PolygonMode:glPolygonMode(u(0),u(1));break;
        case GlOperation::Flush:glFlush();break;case GlOperation::Finish:glFinish();break;
        default:remember_error(GL_INVALID_OPERATION);break;
        }return 0;
    }
};
}
std::shared_ptr<NativeOpenGlContext> make_native_opengl(void* view,std::uint32_t width,std::uint32_t height){
    const auto* mode=std::getenv("X64ARM_GL_BACKEND");
    return detail::select_opengl_backend(mode?mode:"auto",
        [&]{return make_metal_opengl(view,width,height);},
        [&](std::string reason){return std::make_shared<MacOpenGl>(view,width,height,std::move(reason));});
}
}

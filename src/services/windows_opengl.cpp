#include "x64arm/windows_services.hpp"
#include "x64arm/windows_opengl.hpp"
#include "windows_wgl_formats.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <unordered_map>
namespace x64arm {
namespace {
struct Command {std::string_view name;GlOperation operation;unsigned arguments,float_mask,double_mask;};
constexpr Command commands[]{
    {"glGenVertexArrays",GlOperation::GenVertexArrays,2,0,0},
    {"glBindVertexArray",GlOperation::BindVertexArray,1,0,0},
    {"glDeleteVertexArrays",GlOperation::DeleteVertexArrays,2,0,0},
    {"glIsVertexArray",GlOperation::IsVertexArray,1,0,0},
    {"glIsBuffer",GlOperation::IsBuffer,1,0,0},
    {"glGetBufferParameteriv",GlOperation::GetBufferParameteriv,3,0,0},
    {"glGetBufferSubData",GlOperation::GetBufferSubData,4,0,0},
    {"glCopyBufferSubData",GlOperation::CopyBufferSubData,5,0,0},
    {"glGetVertexAttribiv",GlOperation::GetVertexAttribiv,3,0,0},
    {"glGetVertexAttribfv",GlOperation::GetVertexAttribfv,3,0,0},
    {"glGetVertexAttribPointerv",GlOperation::GetVertexAttribPointerv,3,0,0},
    {"glVertexAttrib2f",GlOperation::VertexAttrib2f,3,6,0},
    {"glCullFace",GlOperation::CullFace,1,0,0},
    {"glFrontFace",GlOperation::FrontFace,1,0,0},
    {"glPolygonMode",GlOperation::PolygonMode,2,0,0},
    {"glCreateShader",GlOperation::CreateShader,1,0,0},
    {"glShaderSource",GlOperation::ShaderSource,4,0,0},
    {"glCompileShader",GlOperation::CompileShader,1,0,0},
    {"glGetShaderiv",GlOperation::GetShaderiv,3,0,0},
    {"glGetShaderInfoLog",GlOperation::GetShaderInfoLog,4,0,0},
    {"glDeleteShader",GlOperation::DeleteShader,1,0,0},
    {"glCreateProgram",GlOperation::CreateProgram,0,0,0},
    {"glAttachShader",GlOperation::AttachShader,2,0,0},
    {"glDetachShader",GlOperation::DetachShader,2,0,0},
    {"glBindAttribLocation",GlOperation::BindAttribLocation,3,0,0},
    {"glLinkProgram",GlOperation::LinkProgram,1,0,0},
    {"glGetProgramiv",GlOperation::GetProgramiv,3,0,0},
    {"glGetProgramInfoLog",GlOperation::GetProgramInfoLog,4,0,0},
    {"glDeleteProgram",GlOperation::DeleteProgram,1,0,0},
    {"glUseProgram",GlOperation::UseProgram,1,0,0},
    {"glGetUniformLocation",GlOperation::GetUniformLocation,2,0,0},
    {"glUniformMatrix4fv",GlOperation::UniformMatrix4fv,4,0,0},
    {"glGenBuffers",GlOperation::GenBuffers,2,0,0},
    {"glBindBuffer",GlOperation::BindBuffer,2,0,0},
    {"glBufferData",GlOperation::BufferData,4,0,0},
    {"glBufferSubData",GlOperation::BufferSubData,4,0,0},
    {"glDeleteBuffers",GlOperation::DeleteBuffers,2,0,0},
    {"glBindBufferBase",GlOperation::BindBufferBase,3,0,0},
    {"glEnableVertexAttribArray",GlOperation::EnableVertexAttribArray,1,0,0},
    {"glDisableVertexAttribArray",GlOperation::DisableVertexAttribArray,1,0,0},
    {"glVertexAttribPointer",GlOperation::VertexAttribPointer,6,0,0},
    {"glVertexAttrib1f",GlOperation::VertexAttrib1f,2,2,0},
    {"glVertexAttrib3f",GlOperation::VertexAttrib3f,4,14,0},
    {"glVertexAttrib4f",GlOperation::VertexAttrib4f,5,30,0},
    {"glVertexAttribDivisor",GlOperation::VertexAttribDivisor,2,0,0},
    {"glDrawArraysInstanced",GlOperation::DrawArraysInstanced,4,0,0},
    {"glDrawElementsInstanced",GlOperation::DrawElementsInstanced,5,0,0},
    {"glGenFramebuffers",GlOperation::GenFramebuffers,2,0,0},
    {"glBindFramebuffer",GlOperation::BindFramebuffer,2,0,0},
    {"glDeleteFramebuffers",GlOperation::DeleteFramebuffers,2,0,0},
    {"glFramebufferTexture2D",GlOperation::FramebufferTexture2D,5,0,0},
    {"glCheckFramebufferStatus",GlOperation::CheckFramebufferStatus,1,0,0},
    {"glDrawBuffers",GlOperation::DrawBuffers,2,0,0},
    {"glActiveTexture",GlOperation::ActiveTexture,1,0,0},
    {"glGenerateMipmap",GlOperation::GenerateMipmap,1,0,0},
    {"glBlendEquationSeparate",GlOperation::BlendEquationSeparate,2,0,0},
    {"glBlendFuncSeparate",GlOperation::BlendFuncSeparate,4,0,0},
    {"glUniform1fv",GlOperation::Uniform1fv,3,0,0},
    {"glUniform1iv",GlOperation::Uniform1iv,3,0,0},
    {"glUniform2fv",GlOperation::Uniform2fv,3,0,0},
    {"glUniform2iv",GlOperation::Uniform2iv,3,0,0},
    {"glUniform3fv",GlOperation::Uniform3fv,3,0,0},
    {"glUniform3iv",GlOperation::Uniform3iv,3,0,0},
    {"glUniform4fv",GlOperation::Uniform4fv,3,0,0},
    {"glUniform4iv",GlOperation::Uniform4iv,3,0,0},

    {"glBegin",GlOperation::Begin,1,0,0},
    {"glEnd",GlOperation::End,0,0,0},
    {"glVertex2f",GlOperation::Vertex2f,2,3,0},
    {"glVertex2i",GlOperation::Vertex2i,2,0,0},
    {"glVertex3f",GlOperation::Vertex3f,3,7,0},
    {"glVertex3fv",GlOperation::Vertex3fv,1,0,0},
    {"glVertex4fv",GlOperation::Vertex4fv,1,0,0},
    {"glNormal3f",GlOperation::Normal3f,3,7,0},
    {"glNormal3fv",GlOperation::Normal3fv,1,0,0},
    {"glGetTexLevelParameteriv",GlOperation::GetTexLevelParameteriv,4,0,0},
    {"glGetTexLevelParameterfv",GlOperation::GetTexLevelParameterfv,4,0,0},
    {"glColor3f",GlOperation::Color3f,3,7,0},
    {"glColor4f",GlOperation::Color4f,4,15,0},
    {"glColor4fv",GlOperation::Color4fv,1,0,0},
    {"glColor3ub",GlOperation::Color4f,3,0,0},
    {"glColor3ubv",GlOperation::Color4f,1,0,0},
    {"glColor3us",GlOperation::Color4f,3,0,0},
    {"glColor3usv",GlOperation::Color4f,1,0,0},
    {"glColor3ui",GlOperation::Color4f,3,0,0},
    {"glColor3uiv",GlOperation::Color4f,1,0,0},
    {"glColor4ub",GlOperation::Color4f,4,0,0},
    {"glColor4ubv",GlOperation::Color4f,1,0,0},
    {"glColor4us",GlOperation::Color4f,4,0,0},
    {"glColor4usv",GlOperation::Color4f,1,0,0},
    {"glColor4ui",GlOperation::Color4f,4,0,0},
    {"glColor4uiv",GlOperation::Color4f,1,0,0},
    {"glTexCoord2f",GlOperation::TexCoord2f,2,3,0},
    {"glTexCoord2fv",GlOperation::TexCoord2fv,1,0,0},
    {"glTexCoord2d",GlOperation::TexCoord2f,2,0,3},
    {"glTexCoord2i",GlOperation::TexCoord2f,2,0,0},
    {"glClear",GlOperation::Clear,1,0,0},
    {"glClearColor",GlOperation::ClearColor,4,15,0},
    {"glClearDepth",GlOperation::ClearDepth,1,0,1},
    {"glEnable",GlOperation::Enable,1,0,0},
    {"glDisable",GlOperation::Disable,1,0,0},
    {"glBlendFunc",GlOperation::BlendFunc,2,0,0},
    {"glAlphaFunc",GlOperation::AlphaFunc,2,2,0},
    {"glStencilFunc",GlOperation::StencilFunc,3,0,0},
    {"glStencilMask",GlOperation::StencilMask,1,0,0},
    {"glStencilOp",GlOperation::StencilOp,3,0,0},
    {"glClearStencil",GlOperation::ClearStencil,1,0,0},
    {"glDepthFunc",GlOperation::DepthFunc,1,0,0},
    {"glDepthMask",GlOperation::DepthMask,1,0,0},
    {"glDepthRange",GlOperation::DepthRange,2,0,3},
    {"glColorMask",GlOperation::ColorMask,4,0,0},
    {"glLineWidth",GlOperation::LineWidth,1,1,0},
    {"glPixelZoom",GlOperation::PixelZoom,2,3,0},
    {"glRenderMode",GlOperation::RenderMode,1,0,0},
    {"glShadeModel",GlOperation::ShadeModel,1,0,0},
    {"glHint",GlOperation::Hint,2,0,0},
    {"glViewport",GlOperation::Viewport,4,0,0},
    {"glScissor",GlOperation::Scissor,4,0,0},
    {"glMatrixMode",GlOperation::MatrixMode,1,0,0},
    {"glLoadIdentity",GlOperation::LoadIdentity,0,0,0},
    {"glLoadMatrixf",GlOperation::LoadMatrixf,1,0,0},
    {"glMultMatrixf",GlOperation::MultMatrixf,1,0,0},
    {"glPushMatrix",GlOperation::PushMatrix,0,0,0},
    {"glPopMatrix",GlOperation::PopMatrix,0,0,0},
    {"glTranslatef",GlOperation::Translatef,3,7,0},
    {"glRotatef",GlOperation::Rotatef,4,15,0},
    {"glScalef",GlOperation::Scalef,3,7,0},
    {"glOrtho",GlOperation::Ortho,6,0,63},
    {"glFrustum",GlOperation::Frustum,6,0,63},
    {"gluPerspective",GlOperation::Perspective,4,0,15},
    {"glLightfv",GlOperation::Lightfv,3,0,0},
    {"glMaterialf",GlOperation::Materialf,3,4,0},
    {"glMaterialfv",GlOperation::Materialfv,3,0,0},
    {"glFogf",GlOperation::Fogf,2,2,0},
    {"glFogi",GlOperation::Fogi,2,0,0},
    {"glFogfv",GlOperation::Fogfv,2,0,0},
    {"glPushAttrib",GlOperation::PushAttrib,1,0,0},
    {"glPopAttrib",GlOperation::PopAttrib,0,0,0},
    {"glPushClientAttrib",GlOperation::PushClientAttrib,1,0,0},
    {"glPopClientAttrib",GlOperation::PopClientAttrib,0,0,0},
    {"glBindTexture",GlOperation::BindTexture,2,0,0},
    {"glIsTexture",GlOperation::IsTexture,1,0,0},
    {"glGenTextures",GlOperation::GenTextures,2,0,0},
    {"glDeleteTextures",GlOperation::DeleteTextures,2,0,0},
    {"glTexImage2D",GlOperation::TexImage2D,9,0,0},
    {"glTexSubImage2D",GlOperation::TexSubImage2D,9,0,0},
    {"glTexParameterf",GlOperation::TexParameterf,3,4,0},
    {"glTexParameteri",GlOperation::TexParameteri,3,0,0},
    {"glTexEnvi",GlOperation::TexEnvi,3,0,0},
    {"glTexEnvf",GlOperation::TexEnvf,3,4,0},
    {"glTexEnviv",GlOperation::TexEnviv,3,0,0},
    {"glTexEnvfv",GlOperation::TexEnvfv,3,0,0},
    {"glGetTexEnviv",GlOperation::GetTexEnviv,3,0,0},
    {"glGetTexEnvfv",GlOperation::GetTexEnvfv,3,0,0},
    {"glPixelStorei",GlOperation::PixelStorei,2,0,0},
    {"glNewList",GlOperation::NewList,2,0,0},
    {"glEndList",GlOperation::EndList,0,0,0},
    {"glGenLists",GlOperation::GenLists,1,0,0},
    {"glDeleteLists",GlOperation::DeleteLists,2,0,0},
    {"glCallList",GlOperation::CallList,1,0,0},
    {"glDrawArrays",GlOperation::DrawArrays,3,0,0},
    {"glDrawElements",GlOperation::DrawElements,4,0,0},
    {"glBitmap",GlOperation::Bitmap,7,60,0},
    {"glRasterPos2i",GlOperation::RasterPos2i,2,0,0},
    {"glRasterPos3f",GlOperation::RasterPos3f,3,7,0},
    {"glDrawBuffer",GlOperation::DrawBuffer,1,0,0},
    {"glReadBuffer",GlOperation::ReadBuffer,1,0,0},
    {"glGetError",GlOperation::GetError,0,0,0},
    {"glGetIntegerv",GlOperation::GetIntegerv,2,0,0},
    {"glGetFloatv",GlOperation::GetFloatv,2,0,0},
    {"glGetDoublev",GlOperation::GetDoublev,2,0,0},
    {"glGetBooleanv",GlOperation::GetBooleanv,2,0,0},
    {"glIsEnabled",GlOperation::IsEnabled,1,0,0},
    {"glReadPixels",GlOperation::ReadPixels,7,0,0},
    {"glFlush",GlOperation::Flush,0,0,0},
    {"glFinish",GlOperation::Finish,0,0,0},
};
const Command* find_command(std::string_view name){
    static const auto index=[](){std::unordered_map<std::string_view,const Command*> result;result.reserve(std::size(commands));for(const auto& command:commands)result.emplace(command.name,&command);return result;}();
    const auto it=index.find(name);return it==index.end()?nullptr:it->second;
}
constexpr std::uint32_t invalid_enum=0x500,invalid_value=0x501,invalid_operation=0x502,out_of_memory=0x505;
constexpr std::size_t transfer_limit=256U*1024U*1024U;
void error(OpenGlContext& c,std::uint32_t e){if(!c.error)c.error=e;}
std::string context_diagnostics(std::uint64_t handle,const OpenGlContext& c){
    const auto info=c.native->info();std::ostringstream stream;
    stream<<"opengl_context=0x"<<std::hex<<handle<<std::dec<<" accelerated="<<info.accelerated<<" renderer="<<info.renderer<<" vendor="<<info.vendor<<" version="<<info.version<<" calls="<<c.calls<<" draws="<<c.draws<<" frames="<<c.frames<<'\n';
    stream<<c.native->statistics();
    if(const auto* directory=std::getenv("X64ARM_GL_CAPTURE_DIR");directory&&*directory&&!c.beginning&&c.draws){try{stream<<c.native->capture(std::filesystem::path(directory)/("context-"+std::to_string(handle)+"-frame-"+std::to_string(c.frames)+".png"));}catch(const std::exception& e){stream<<"opengl_capture_error="<<e.what()<<'\n';}}
    return stream.str();
}
std::uint32_t type_size(std::uint32_t type){switch(type){case 0x1400:case 0x1401:return 1;case 0x1402:case 0x1403:case 0x140b:return 2;case 0x1404:case 0x1405:case 0x1406:return 4;case 0x140a:return 8;default:return 0;}}
std::vector<std::uint8_t> read_bytes(GuestMemory& memory,std::uint64_t address,std::uint64_t length){
    if(length>transfer_limit)throw HostCallFault("OpenGL 資料超過傳輸上限");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));if(length)memory.read(GuestAddress{address},bytes);return bytes;
}
void read_reusable(GuestMemory& memory,std::uint64_t address,std::uint64_t length,std::vector<std::uint8_t>& bytes){
    if(length>transfer_limit)throw HostCallFault("OpenGL 資料超過傳輸上限");
    bytes.resize(static_cast<std::size_t>(length));if(length)memory.read(GuestAddress{address},bytes);
}
unsigned query_size(std::uint32_t p){
    if(p>=0x4000 && p<=0x4007)return 1;
    switch(p){case 0xba6:case 0xba7:case 0xba8:return 16;
    case 0xb00:case 0xb03:case 0xb04:case 0xb06:case 0xb07:case 0xba2:case 0xc10:case 0xc22:case 0xc23:case 0xb53:case 0xb66:case 0xb80:return 4;
    case 0xb02:return 3;
    case 0xb12:case 0xb22:case 0xb40:case 0xb70:case 0xd3a:case 0x846d:case 0x846e:return 2;
    default:break;}
    // 只對已知 scalar query 提供儲存空間，避免驅動寫出未知長度。
    switch(p){
    case 0xd16:case 0xd17:
    case 0xb01:case 0xb08:case 0xb10:case 0xb11:case 0xb13:case 0xb20:case 0xb21:case 0xb23:case 0xb24:case 0xb25:case 0xb30:case 0xb31:case 0xb32:
    case 0xb41:case 0xb42:case 0xb43:case 0xb44:case 0xb45:case 0xb46:case 0xb50:case 0xb51:case 0xb52:case 0xb54:case 0xb55:case 0xb56:case 0xb57:
    case 0xb60:case 0xb61:case 0xb62:case 0xb63:case 0xb64:case 0xb65:case 0xb71:case 0xb72:case 0xb73:case 0xb74:case 0xb90:case 0xb91:case 0xb92:case 0xb93:case 0xb94:case 0xb95:case 0xb96:case 0xb97:case 0xb98:
    case 0xba0:case 0xba1:case 0xba3:case 0xba4:case 0xba5:case 0xbc0:case 0xbc1:case 0xbc2:case 0xbd0:case 0xbe0:case 0xbe1:case 0xbe2:case 0xbf0:case 0xbf1:case 0xbf2:
    case 0xc00:case 0xc01:case 0xc02:case 0xc11:case 0xc20:case 0xc21:case 0xc30:case 0xc31:case 0xc32:case 0xc33:case 0xc40:case 0xc50:case 0xc51:case 0xc52:case 0xc53:case 0xc54:
    case 0xcf0:case 0xcf1:case 0xcf2:case 0xcf3:case 0xcf4:case 0xcf5:case 0xd00:case 0xd01:case 0xd02:case 0xd03:case 0xd04:case 0xd05:
    case 0xd30:case 0xd31:case 0xd32:case 0xd33:case 0xd34:case 0xd35:case 0xd36:case 0xd37:case 0xd38:case 0xd39:case 0xd3b:case 0xd50:case 0xd51:case 0xd52:case 0xd53:case 0xd54:case 0xd55:case 0xd56:case 0xd57:case 0xd58:case 0xd59:
    case 0x85b5:case 0x8f36:case 0x8f37:case 0x8a28:
    case 0x84ff:case 0x8b4d:case 0x8872:case 0x8b4c:case 0x8869:case 0x8cdf:case 0x8824:case 0x8b8d:case 0x8894:case 0x8895:case 0x84e0:case 0x821b:case 0x821c:case 0x821d:
    case 0xde0:case 0xde1:case 0x8068:case 0x8069:case 0x8074:case 0x8075:case 0x8076:case 0x8078:case 0x8086:case 0x8087:case 0x8088:case 0x808a:case 0x808b:case 0x808c:case 0x808d:case 0x808e:case 0x808f:case 0x8090:case 0x8091:case 0x8092:case 0x8093:return 1;
    default:return 0;}
}
std::optional<std::size_t> pixel_bytes(const OpenGlClientState& client,std::int32_t width,std::int32_t height,std::uint32_t format,std::uint32_t type,bool pack){
    if(width<0 || height<0)return std::nullopt;if(!width || !height)return 0;
    unsigned channels=0;switch(format){case 0x1900:case 0x1901:case 0x1902:case 0x1903:case 0x1904:case 0x1905:case 0x1906:case 0x1909:channels=1;break;case 0x190a:case 0x8227:channels=2;break;case 0x1907:case 0x80e0:channels=3;break;case 0x1908:case 0x80e1:channels=4;break;default:return std::nullopt;}
    const auto base=pack?0xd00U:0xcf0U;const auto value=[&](unsigned index){const auto it=client.pixel_store.find(base+index);return it==client.pixel_store.end()?0:it->second;};
    const auto alignment=value(5);if(alignment!=1&&alignment!=2&&alignment!=4&&alignment!=8)return std::nullopt;
    const auto row=static_cast<std::uint64_t>(value(2)?value(2):width),skip_rows=static_cast<std::uint64_t>(value(3)),skip_pixels=static_cast<std::uint64_t>(value(4));
    const auto unit=type_size(type);std::uint64_t bits=0;
    if(type==0x1a00){if(channels!=1)return std::nullopt;bits=1;}
    else if(unit)bits=unit*channels*8U;
    else {switch(type){case 0x8c3b:if(format!=0x1907)return std::nullopt;bits=32;break;case 0x8032:case 0x8362:if(channels!=3)return std::nullopt;bits=8;break;case 0x8033:case 0x8034:case 0x8363:case 0x8364:case 0x8365:case 0x8366:bits=16;break;case 0x8035:case 0x8036:case 0x8367:case 0x8368:bits=32;break;default:return std::nullopt;}}
    const auto align=static_cast<std::uint64_t>(alignment);const auto stride=((row*bits+7)/8+align-1)/align*align;
    const auto size=(skip_rows+static_cast<std::uint64_t>(height)-1)*stride+((skip_pixels+static_cast<std::uint64_t>(width))*bits+7)/8;
    if(size>transfer_limit)return std::nullopt;return static_cast<std::size_t>(size);
}
}
std::span<const WindowsServiceDescriptor> opengl_service_descriptors(){
    static const auto table=[](){std::vector<WindowsServiceDescriptor> result;
        for(const auto& c:commands)result.push_back({c.name.starts_with("glu")?"glu32.dll":"opengl32.dll",c.name,c.arguments,c.double_mask});
        const WindowsServiceDescriptor extra[]{
            {"opengl32.dll","wglCreateContext",1},{"opengl32.dll","wglDeleteContext",1},{"opengl32.dll","wglShareLists",2},{"opengl32.dll","wglMakeCurrent",2},
            {"opengl32.dll","wglGetCurrentContext",0},{"opengl32.dll","wglGetCurrentDC",0},{"opengl32.dll","wglGetProcAddress",1},
            {"opengl32.dll","wglSwapIntervalEXT",1},{"opengl32.dll","wglGetSwapIntervalEXT",0},
            {"opengl32.dll","wglGetExtensionsStringARB",1},{"opengl32.dll","wglChoosePixelFormatARB",6},
            {"opengl32.dll","wglGetPixelFormatAttribivARB",6},{"opengl32.dll","wglGetPixelFormatAttribfvARB",6},{"opengl32.dll","wglCreateContextAttribsARB",3},
            {"opengl32.dll","glGetStringi",2},{"opengl32.dll","glGetPointerv",2},{"opengl32.dll","glGetString",1},{"opengl32.dll","glVertexPointer",4},{"opengl32.dll","glColorPointer",4},{"opengl32.dll","glNormalPointer",3},{"opengl32.dll","glTexCoordPointer",4},
            {"opengl32.dll","glEnableClientState",1},{"opengl32.dll","glDisableClientState",1},{"opengl32.dll","glArrayElement",1},
            {"gdi32.dll","ChoosePixelFormat",2},{"gdi32.dll","DescribePixelFormat",4},{"gdi32.dll","SetPixelFormat",3},{"gdi32.dll","GetPixelFormat",1},{"gdi32.dll","SwapBuffers",1}};
        result.insert(result.end(),std::begin(extra),std::end(extra));return result;}();return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_opengl(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
    const auto d=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};const auto n=[&](unsigned i){return std::bit_cast<std::int32_t>(d(i));};
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto dc_window=[&](std::uint64_t dc)->std::uint64_t{const auto h=dc>=0x10000000?dc-0x10000000:0;return gui_.windows.contains(h)?h:0;};
    const auto current_handle=[&](){const auto it=opengl_.current.find(threads_.current);return it==opengl_.current.end()?UINT64_C(0):it->second;};
    const auto window_interval=[&](std::uint64_t window){const auto it=opengl_.swap_intervals.find(window);return it==opengl_.swap_intervals.end()?1:it->second;};
#include "windows_wgl_formats.inc"
    if(symbol=="ChoosePixelFormat" || symbol=="DescribePixelFormat" || symbol=="SetPixelFormat" || symbol=="GetPixelFormat"){
        if(!dc_window(arg(0))){last_error_=6;return finish(0);}
        const auto descriptor=wgl_pixel_format();
        if(symbol=="GetPixelFormat")return finish(opengl_.pixel_formats.contains(arg(0))?opengl_.pixel_formats.at(arg(0)):0);
        if(symbol=="DescribePixelFormat"){
            if(d(1)!=1){last_error_=2000;return finish(0);}if(arg(3) && d(2)){memory.write(GuestAddress{arg(3)},std::span(descriptor).first(std::min<std::size_t>(d(2),descriptor.size())));}return finish(1);
        }
        if(symbol=="ChoosePixelFormat"){
            const auto bytes=read_bytes(memory,arg(1),40);const auto flags_value=std::uint32_t(bytes[4])|(std::uint32_t(bytes[5])<<8)|(std::uint32_t(bytes[6])<<16)|(std::uint32_t(bytes[7])<<24);
            if(bytes[0]!=40 || bytes[1] || bytes[2]!=1 || bytes[3] || bytes[8]!=0 || (flags_value&0x18U) || !(flags_value&0x20U)){last_error_=2000;return finish(0);}return finish(1);
        }
        if(d(1)!=1){last_error_=2000;return finish(0);}
        if(opengl_.pixel_formats.contains(arg(0))){last_error_=2000;return finish(0);}
        if(arg(2))memory.check(GuestAddress{arg(2)},40,Access::Read);
        opengl_.pixel_formats[arg(0)]=1;opengl_.swap_intervals[dc_window(arg(0))]=1;return finish(1);
    }
    if(symbol=="wglGetCurrentContext")return finish(current_handle());
    if(symbol=="wglGetCurrentDC"){const auto h=current_handle();return finish(h?opengl_.contexts.at(h).dc:0);}
    if(symbol=="wglGetProcAddress"){
        if(!current_handle())return finish(0);const auto requested=read_ansi(memory,arg(0),256);
        if(requested=="wglSwapIntervalEXT" || requested=="wglGetSwapIntervalEXT" || requested=="wglGetExtensionsStringARB" || requested=="wglChoosePixelFormatARB" || requested=="wglGetPixelFormatAttribivARB" || requested=="wglGetPixelFormatAttribfvARB" || requested=="wglCreateContextAttribsARB")return finish(resolve("opengl32.dll",requested).value);
        if(opengl_.contexts.at(current_handle()).native->programmable() && (find_command(requested) || requested=="glGetStringi"))return finish(resolve("opengl32.dll",requested).value);
        // 未提供的 extension 必須回 NULL；不能讓客體誤以為完整 host GL ABI 可直接呼叫。
        return finish(0);
    }
    if(symbol=="wglCreateContext" || symbol=="wglCreateContextAttribsARB"){
        if(!runtime_.network.external_io)throw HostCallFault("GPU context 不支援 deterministic compare");
        const auto h=dc_window(arg(0));if(!h || !opengl_.pixel_formats.contains(arg(0))){last_error_=2000;return finish(0);}
        int requested_major=1,requested_minor=0;
        if(symbol=="wglCreateContextAttribsARB"){
            int major=1,minor=0,flags=0,layer=0;
            if(arg(2)){bool terminated=false;for(unsigned i=0;i<64;++i){
                const auto at=arg(2)+static_cast<std::uint64_t>(i)*8;const auto name=static_cast<std::uint32_t>(memory.load(GuestAddress{at},32));
                if(!name){terminated=true;break;}const auto value=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{at+4},32)));
                switch(name){case 0x2091:major=value;break;case 0x2092:minor=value;break;case 0x2093:layer=value;break;case 0x2094:flags=value;break;
                case 0x9126:break;default:last_error_=87;return finish(0);}
            }if(!terminated){last_error_=87;return finish(0);}}
            if(major<1 || major>4 || minor<0 || minor>(major==1?5:major==2?1:major==3?3:6) || (flags&2)){last_error_=0x2095;return finish(0);}
            if(layer || (flags&~1)){last_error_=87;return finish(0);}
            // WGL_ARB_create_context：hShareContext 不是有效 context 或無法共用時為 ERROR_INVALID_OPERATION。
            if(arg(1) && !opengl_.contexts.contains(arg(1))){last_error_=4317;return finish(0);}
            requested_major=major;requested_minor=minor;
        }
        if(opengl_.contexts.size()>=64){last_error_=8;return finish(0);}
        auto& w=gui_.windows.at(h);OpenGlContext context;context.dc=arg(0);context.window=h;
        try{context.native=make_native_opengl(native_gui_?native_gui_->graphics_view(h):nullptr,std::max(1U,w.width),std::max(1U,w.height));}
        catch(const std::bad_alloc&){last_error_=8;return finish(0);}
        catch(const std::exception& ex){throw HostCallFault(std::string("硬體 OpenGL context 建立失敗：")+ex.what());}
        if(symbol=="wglCreateContextAttribsARB"){
            // 依選定後端公開的版本驗證；GLSL 編譯器版本不等於 GL 功能版本。
            std::istringstream version(context.native->info().version);int major=0,minor=0;char dot=0;version>>major>>dot>>minor;
            if(!version || dot!='.' || requested_major>major || (requested_major==major && requested_minor>minor)){last_error_=0x2095;return finish(0);}
            if(arg(1) && context.native->share_objects(*opengl_.contexts.at(arg(1)).native)!=OpenGlShareResult::Shared){last_error_=4317;return finish(0);}
        }
        context.client.arrays[1].size=3;const auto handle=opengl_.next_context++;opengl_.contexts.emplace(handle,std::move(context));return finish(handle);
    }
    if(symbol=="wglShareLists"){
        // 第二個 context 加入第一個 context 的物件群組；第二個 context 不可已有自己的共用物件。
        const auto source=opengl_.contexts.find(arg(0)),target=opengl_.contexts.find(arg(1));
        if(source==opengl_.contexts.end() || target==opengl_.contexts.end()){last_error_=6;return finish(0);}
        switch(target->second.native->share_objects(*source->second.native)){
        case OpenGlShareResult::Shared:return finish(1);
        case OpenGlShareResult::Busy:last_error_=170;return finish(0);
        case OpenGlShareResult::Unsupported:break;
        }
        last_error_=50;return finish(0);
    }
    if(symbol=="wglDeleteContext"){
        const auto it=opengl_.contexts.find(arg(0));if(it==opengl_.contexts.end()){last_error_=6;return finish(0);}
        if(it->second.thread && it->second.thread!=threads_.current){last_error_=170;return finish(0);}
        if(it->second.thread)opengl_.current.erase(it->second.thread);
        if(opengl_.completed.size()<64)opengl_.completed.push_back(context_diagnostics(it->first,it->second));opengl_.contexts.erase(it);return finish(1);
    }
    if(symbol=="wglMakeCurrent"){
        const auto previous=current_handle();
        if(previous){auto& old=opengl_.contexts.at(previous);old.native->execute(GlOperation::Flush);old.thread=0;opengl_.current.erase(threads_.current);}
        if(!arg(1))return finish(1);
        const auto it=opengl_.contexts.find(arg(1));const auto window=dc_window(arg(0));
        if(it==opengl_.contexts.end() || !window || !opengl_.pixel_formats.contains(arg(0))){last_error_=6;return finish(0);}
        auto& c=it->second;if(c.thread && c.thread!=threads_.current){last_error_=170;return finish(0);}
        const auto& w=gui_.windows.at(window);try{c.native->drawable(native_gui_?native_gui_->graphics_view(window):nullptr,std::max(1U,w.width),std::max(1U,w.height));}catch(const std::bad_alloc&){last_error_=8;return finish(0);}
        c.native->bind();const auto interval=window_interval(window);
        if(c.interval!=interval && !c.native->swap_interval(interval)){last_error_=31;return finish(0);}
        c.interval=interval;c.dc=arg(0);c.window=window;c.thread=threads_.current;opengl_.current[threads_.current]=arg(1);return finish(1);
    }
    const auto h=current_handle();
    if(!h){if(symbol=="wglSwapIntervalEXT" || symbol=="wglGetSwapIntervalEXT")last_error_=1425;else if(symbol=="SwapBuffers" || symbol.starts_with("wgl"))last_error_=6;return finish(0);}
    auto& c=opengl_.contexts.at(h);++c.calls;
    const auto upload=[&](unsigned index,std::int32_t size,std::uint32_t type,std::span<const std::uint8_t> bytes,bool enabled){try{c.native->array(index,size,type,bytes,enabled);return true;}catch(const std::bad_alloc&){error(c,out_of_memory);return false;}};
    if(symbol=="SwapBuffers"){
        if(arg(0)!=c.dc || !gui_.windows.contains(c.window)){last_error_=6;return finish(0);}
        if(c.beginning){error(c,invalid_operation);last_error_=87;return finish(0);}
        const auto& w=gui_.windows.at(c.window);try{c.native->drawable(native_gui_?native_gui_->graphics_view(c.window):nullptr,std::max(1U,w.width),std::max(1U,w.height));}catch(const std::bad_alloc&){last_error_=8;return finish(0);}
        const auto interval=window_interval(c.window);if(c.interval!=interval && !c.native->swap_interval(interval)){last_error_=31;return finish(0);}c.interval=interval;
        const auto ok=c.native->swap();if(ok){++c.frames;if(const auto* directory=std::getenv("X64ARM_GL_CAPTURE_DIR");directory&&*directory){
            const auto now=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            if(c.frames==1 || c.frames==32 || c.frames==256 || now>=c.capture_next_milliseconds){c.capture_next_milliseconds=now+4000;try{const auto report=c.native->capture(std::filesystem::path(directory)/("context-"+std::to_string(h)+"-frame-"+std::to_string(c.frames)+".png"));(void)report;}catch(const std::exception&){}}
        }}return finish(ok?1:0);
    }
    if(symbol=="wglSwapIntervalEXT" || symbol=="wglGetSwapIntervalEXT"){
        if(!dc_window(c.dc) || !gui_.windows.contains(c.window)){last_error_=1425;return finish(0);}
        if(symbol=="wglGetSwapIntervalEXT")return finish(static_cast<std::uint32_t>(window_interval(c.window)));
        if(n(0)<0){last_error_=13;return finish(0);}
        // 此後端支援 0/1；EXT 規格允許將更大的正值 clamp 到實作上限。
        const auto interval=std::min(n(0),1);
        if(!c.native->swap_interval(interval)){last_error_=31;return finish(0);}
        opengl_.swap_intervals[c.window]=interval;c.interval=interval;return finish(1);
    }
    if(symbol=="glGetError"){
        if(c.beginning){error(c,invalid_operation);return finish(0);}if(c.error){const auto e=c.error;c.error=0;return finish(e);}return finish(c.native->execute(GlOperation::GetError));
    }
    const bool current_attribute=symbol=="glVertexAttrib1f" || symbol=="glVertexAttrib2f" || symbol=="glVertexAttrib3f" || symbol=="glVertexAttrib4f";
    const bool immediate=current_attribute || (symbol.starts_with("glVertex")&&!symbol.starts_with("glVertexAttrib")&&!symbol.ends_with("Pointer")) || (symbol.starts_with("glNormal")&&!symbol.ends_with("Pointer")) || (symbol.starts_with("glColor")&&!symbol.ends_with("Pointer")) || (symbol.starts_with("glTexCoord")&&!symbol.ends_with("Pointer")) || symbol.starts_with("glMaterial") || symbol=="glArrayElement" || symbol=="glCallList" || symbol=="glEnd";
    if(c.beginning && !immediate){error(c,invalid_operation);return finish(0);}
    if(symbol=="glGetStringi"){error(c,d(0)==0x1f03?invalid_value:invalid_enum);return finish(0);}
    if(symbol=="glGetString"){
        if(d(0)!=0x1f00 && d(0)!=0x1f01 && d(0)!=0x1f02 && d(0)!=0x1f03 && d(0)!=0x8b8c){error(c,invalid_enum);return finish(0);}
        if(!c.strings.contains(d(0))){const auto value=d(0)==0x1f03?std::string{}:c.native->string(d(0));std::vector<std::uint8_t> bytes(value.begin(),value.end());bytes.push_back(0);c.strings[d(0)]=runtime_allocate(memory,bytes);}
        return finish(c.strings.at(d(0)));
    }
    const auto array_index=[](std::uint32_t name)->int{switch(name){case 0x8074:return 0;case 0x8075:return 1;case 0x8076:return 2;case 0x8078:return 3;default:return -1;}};
    if(symbol=="glGetPointerv"){
        const auto i=d(0)==0x808e?0:d(0)==0x808f?1:d(0)==0x8090?2:d(0)==0x8092?3:-1;
        if(i<0)error(c,invalid_enum);else memory.store(GuestAddress{arg(1)},memory.execution_mode()==ExecutionMode::X86?32:64,c.client.arrays[static_cast<std::size_t>(i)].pointer);return finish(0);
    }
    if(symbol=="glIsEnabled"){const auto i=array_index(d(0));if(i>=0)return finish(c.client.arrays[static_cast<std::size_t>(i)].enabled?1:0);}
    if(symbol=="glGetIntegerv" || symbol=="glGetFloatv" || symbol=="glGetDoublev" || symbol=="glGetBooleanv"){
        std::optional<std::uint32_t> answer;const auto pname=d(0);const auto enabled=array_index(pname);
        if(enabled>=0)answer=c.client.arrays[static_cast<std::size_t>(enabled)].enabled?1U:0U;
        constexpr std::array<std::uint32_t,4> buffer_queries{0x8896,0x8897,0x8898,0x889a};
        for(unsigned i=0;i<4;++i)if(pname==buffer_queries[i])answer=c.client.arrays[i].buffer?c.client.arrays[i].buffer->name:0;
        constexpr std::array<std::array<std::uint32_t,3>,4> fields{{{0x807a,0x807b,0x807c},{0,0x807e,0x807f},{0x8081,0x8082,0x8083},{0x8088,0x8089,0x808a}}};
        for(unsigned i=0;i<4;++i)for(unsigned j=0;j<3;++j)if(fields[i][j] && fields[i][j]==pname){const auto& a=c.client.arrays[i];answer=j==0?static_cast<std::uint32_t>(a.size):j==1?a.type:static_cast<std::uint32_t>(a.stride);}
        if(answer){const auto value=*answer;if(symbol=="glGetBooleanv")memory.store(GuestAddress{arg(1)},8,value?1:0);
            else if(symbol=="glGetFloatv")memory.store(GuestAddress{arg(1)},32,std::bit_cast<std::uint32_t>(static_cast<float>(value)));
            else if(symbol=="glGetDoublev")memory.store(GuestAddress{arg(1)},64,std::bit_cast<std::uint64_t>(static_cast<double>(value)));
            else memory.store(GuestAddress{arg(1)},32,value);return finish(0);}
    }
    if(symbol=="glEnableClientState" || symbol=="glDisableClientState"){
        const auto i=array_index(d(0));if(i<0)error(c,invalid_enum);else c.client.arrays[static_cast<std::size_t>(i)].enabled=symbol=="glEnableClientState";return finish(0);
    }
    if(symbol=="glVertexPointer" || symbol=="glNormalPointer" || symbol=="glColorPointer" || symbol=="glTexCoordPointer"){
        const unsigned i=symbol=="glVertexPointer"?0U:symbol=="glNormalPointer"?1U:symbol=="glColorPointer"?2U:3U;
        const auto size=i==1?3:n(0),stride=n(i==1?1:2);const auto type=d(i==1?0:1);
        const bool valid_type=i==0?(type==0x1402 || type==0x1404 || type==0x1406 || type==0x140a):i==1?(type==0x1400 || type==0x1402 || type==0x1404 || type==0x1406 || type==0x140a):i==2?type_size(type)!=0:(type==0x1402 || type==0x1404 || type==0x1406 || type==0x140a);
        if(!valid_type){error(c,invalid_enum);return finish(0);}if(stride<0 || size<(i==0?2:i==2?3:1) || size>4){error(c,invalid_value);return finish(0);}
        auto buffer=c.native->bound_buffer(0x8892);const auto pointer=arg(i==1?2:3);
        if(c.vertex_array && !buffer && pointer){error(c,invalid_operation);return finish(0);}
        auto& a=c.client.arrays[i];a.pointer=pointer;a.size=size;a.stride=stride;a.type=type;a.buffer=std::move(buffer);return finish(0);
    }
    const auto array_read=[&](const OpenGlArray& array,std::uint64_t offset,std::span<std::uint8_t> bytes){
        if(array.pointer>UINT64_MAX-offset){error(c,invalid_operation);return false;}offset+=array.pointer;
        if(array.buffer){if(offset>array.buffer->bytes.size() || bytes.size()>array.buffer->bytes.size()-offset){error(c,invalid_operation);return false;}
            if(!bytes.empty())std::memcpy(bytes.data(),array.buffer->bytes.data()+offset,bytes.size());}
        else if(!bytes.empty())memory.read(GuestAddress{offset},bytes);return true;
    };
    const auto array_element=[&](std::uint32_t index){
        // ArrayElement 在 display list 編譯時擷取當時的 guest 陣列內容。
        for(const unsigned i:{2U,1U,3U,0U}){
            const auto& a=c.client.arrays[i];if(!a.enabled)continue;const auto unit=type_size(a.type),extent=unit*static_cast<std::uint32_t>(a.size);const auto stride=a.stride?static_cast<std::uint32_t>(a.stride):extent;
            const auto offset=static_cast<std::uint64_t>(index)*stride;
            std::array<std::uint8_t,32> bytes{};if(!array_read(a,offset,std::span(bytes).first(extent)))return;std::array<double,4> values{0,0,0,1};
            for(unsigned j=0;j<static_cast<unsigned>(a.size);++j){std::uint64_t raw=0;for(unsigned k=0;k<unit;++k)raw|=std::uint64_t(bytes[j*unit+k])<<(8*k);
                switch(a.type){case 0x1400:values[j]=std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(raw));if(i==1||i==2)values[j]=std::max(-1.0,values[j]/127.0);break;
                case 0x1401:values[j]=static_cast<std::uint8_t>(raw);if(i==2)values[j]/=255.;break;
                case 0x1402:values[j]=std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(raw));if(i==1||i==2)values[j]=std::max(-1.0,values[j]/32767.);break;
                case 0x1403:values[j]=static_cast<std::uint16_t>(raw);if(i==2)values[j]/=65535.;break;
                case 0x1404:values[j]=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(raw));if(i==1||i==2)values[j]=std::max(-1.0,values[j]/2147483647.);break;
                case 0x1405:values[j]=static_cast<std::uint32_t>(raw);if(i==2)values[j]/=4294967295.;break;
                case 0x1406:values[j]=std::bit_cast<float>(static_cast<std::uint32_t>(raw));break;case 0x140a:values[j]=std::bit_cast<double>(raw);break;default:break;}}
            const auto op=i==0?GlOperation::Vertex4dv:i==1?GlOperation::Normal3dv:i==2?GlOperation::Color4dv:GlOperation::TexCoord4dv;
            c.native->execute(op,{},std::span(reinterpret_cast<const std::uint8_t*>(values.data()),sizeof(values)));
        }
    };
    if(symbol=="glArrayElement"){if(n(0)<0)error(c,invalid_value);else array_element(d(0));return finish(0);}
    const auto* found=find_command(symbol);
    if(!found)throw HostCallFault("未知 OpenGL 服務");
    std::array<std::uint64_t,10> values{};for(unsigned i=0;i<service.arguments;++i){values[i]=arg(i);if(memory.execution_mode()!=ExecutionMode::X86 && i<4 && ((found->float_mask|found->double_mask)&(1U<<i))){values[i]=0;for(unsigned k=0;k<((found->double_mask&(1U<<i))?8U:4U);++k)values[i]|=std::uint64_t(state.xmm[i][k])<<(k*8);}}
    auto& input=c.input_scratch;auto& output=c.output_scratch;input.clear();output.clear();std::uint64_t output_address=0;
    const auto set_input=[&](std::uint64_t pointer,std::size_t length){read_reusable(memory,pointer,length,input);};
    const auto set_output=[&](std::uint64_t pointer,std::size_t length){memory.check(GuestAddress{pointer},length,Access::Write);output.resize(length);output_address=pointer;};
    // 無號整數顏色先依宣告寬度正規化；向量在改寫引數前完整讀取。
    if(symbol.starts_with("glColor") && symbol.size()>=10 && symbol[8]=='u'){
        const unsigned count=static_cast<unsigned>(symbol[7]-'0');
        const unsigned bits=symbol[9]=='b'?8U:symbol[9]=='s'?16U:32U;
        const auto maximum=(std::uint64_t{1}<<bits)-1;
        const auto source=values[0];
        std::array<std::uint64_t,4> components{};
        for(unsigned i=0;i<count;++i)components[i]=symbol.ends_with("v")?memory.load(GuestAddress{source+i*(bits/8)},static_cast<std::uint8_t>(bits)):(values[i]&maximum);
        for(unsigned i=0;i<4;++i)values[i]=std::bit_cast<std::uint32_t>(i<count?static_cast<float>(static_cast<double>(components[i])/static_cast<double>(maximum)):1.0F);
    }
    // glTexCoord2d／2i 依 GL 規則轉成單精度目前紋理座標。
    if(symbol=="glTexCoord2d")for(unsigned i=0;i<2;++i)values[i]=std::bit_cast<std::uint32_t>(static_cast<float>(std::bit_cast<double>(values[i])));
    if(symbol=="glTexCoord2i")for(unsigned i=0;i<2;++i)values[i]=std::bit_cast<std::uint32_t>(static_cast<float>(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(values[i]))));
    if(symbol=="glGetTexLevelParameteriv" || symbol=="glGetTexLevelParameterfv"){set_output(arg(3),4);memory.read(GuestAddress{arg(3)},output);}
    else if(symbol=="glShaderSource"){
        if(n(1)<0 || d(1)>4096){error(c,invalid_value);return finish(0);}const auto width=memory.execution_mode()==ExecutionMode::X86?4U:8U;
        for(unsigned i=0;i<d(1);++i){const auto pointer=memory.load(GuestAddress{arg(2)+static_cast<std::uint64_t>(i)*width},static_cast<std::uint8_t>(width*8));const auto length=arg(3)?std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)+i*4},32))):-1;
            if(length<0){const auto source=read_ansi(memory,pointer,16U*1024U*1024U);input.insert(input.end(),source.begin(),source.end());}else{auto source=read_bytes(memory,pointer,static_cast<std::uint32_t>(length));input.insert(input.end(),source.begin(),source.end());}if(input.size()>16U*1024U*1024U){error(c,out_of_memory);return finish(0);}}
    }else if(symbol=="glGetUniformLocation" || symbol=="glBindAttribLocation"){
        const auto name=read_ansi(memory,arg(symbol=="glGetUniformLocation"?1:2),4096);input.assign(name.begin(),name.end());
    }else if(symbol=="glGetShaderiv" || symbol=="glGetProgramiv" || symbol=="glGetBufferParameteriv")set_output(arg(2),4);
    else if(symbol=="glGetVertexAttribiv" || symbol=="glGetVertexAttribfv")set_output(arg(2),d(1)==0x8626?16:4);
    else if(symbol=="glGetVertexAttribPointerv")set_output(arg(2),memory.execution_mode()==ExecutionMode::X86?4:8);
    else if(symbol=="glGetBufferSubData"){
        if(arg(2)>transfer_limit){error(c,invalid_value);return finish(0);}if(arg(2))set_output(arg(3),static_cast<std::size_t>(arg(2)));
    }
    else if(symbol=="glGetShaderInfoLog" || symbol=="glGetProgramInfoLog"){
        if(n(1)<0 || d(1)>transfer_limit){error(c,invalid_value);return finish(0);}if(d(1))set_output(arg(3),d(1));if(arg(2))memory.check(GuestAddress{arg(2)},4,Access::Write);
    }else if(symbol.starts_with("glUniform")){
        if(n(1)<0){error(c,invalid_value);return finish(0);}const bool matrix=symbol=="glUniformMatrix4fv";const unsigned components=matrix?16U:static_cast<unsigned>(symbol[9]-'0');if(n(0)!=-1)set_input(arg(matrix?3:2),static_cast<std::size_t>(d(1))*components*4);
    }else if(symbol=="glGenVertexArrays" || symbol=="glDeleteVertexArrays" || symbol=="glGenBuffers" || symbol=="glGenFramebuffers" || symbol=="glDeleteBuffers" || symbol=="glDeleteFramebuffers" || symbol=="glDrawBuffers"){
        if(n(0)<0 || d(0)>transfer_limit/4){error(c,invalid_value);return finish(0);}if(symbol.starts_with("glGen"))set_output(arg(1),d(0)*4);else set_input(arg(1),d(0)*4);
    }else if(symbol=="glBufferData" || symbol=="glBufferSubData"){
        const auto size=arg(symbol=="glBufferData"?1:2);if(size>transfer_limit){error(c,invalid_value);return finish(0);}if(symbol=="glBufferSubData" || arg(2))set_input(arg(symbol=="glBufferData"?2:3),static_cast<std::size_t>(size));
    }else if((c.program && (symbol=="glDrawArrays" || symbol=="glDrawElements")) || symbol=="glDrawArraysInstanced" || symbol=="glDrawElementsInstanced"){
        const bool indexed=symbol=="glDrawElements" || symbol=="glDrawElementsInstanced";const auto count=n(indexed?1:2);if(count<0 || (!indexed && n(1)<0) || (symbol.ends_with("Instanced") && n(indexed?4:3)<0)){error(c,invalid_value);return finish(0);}
        // EBO 屬於 VAO，也可能保留已刪除名稱的物件；以 renderer 的實際狀態決定是否讀取客體指標。
        if(indexed && !c.native->bound_buffer(0x8893)){if(d(2)!=0x1401 && d(2)!=0x1403 && d(2)!=0x1405){error(c,invalid_enum);return finish(0);}if(count)set_input(arg(3),static_cast<std::size_t>(count)*type_size(d(2)));}++c.draws;
    }else if(symbol=="glBegin"){
        if(d(0)>9){error(c,invalid_enum);return finish(0);}c.beginning=true;++c.draws;
    }else if(symbol=="glEnd"){
        if(!c.beginning){error(c,invalid_operation);return finish(0);}c.beginning=false;
    }else if(symbol=="glVertex3fv" || symbol=="glNormal3fv")set_input(arg(0),12);
    else if(symbol=="glVertex4fv" || symbol=="glColor4fv")set_input(arg(0),16);
    else if(symbol=="glTexCoord2fv")set_input(arg(0),8);
    else if(symbol=="glLoadMatrixf" || symbol=="glMultMatrixf")set_input(arg(0),64);
    else if(symbol=="glMaterialfv" || symbol=="glLightfv" || symbol=="glFogfv"){
        const auto pname=d(symbol=="glFogfv"?0:1);unsigned count=0;
        if(symbol=="glFogfv")count=pname==0xb66?4:pname==0xb61||pname==0xb62||pname==0xb63||pname==0xb64||pname==0xb65?1:0;
        else if(symbol=="glMaterialfv")count=pname==0x1601?1:pname==0x1603?3:pname==0x1200||pname==0x1201||pname==0x1202||pname==0x1600||pname==0x1602?4:0;
        else count=pname==0x1204?3:pname>=0x1205&&pname<=0x1209?1:pname>=0x1200&&pname<=0x1203?4:0;
        if(!count){error(c,invalid_enum);return finish(0);}set_input(arg(symbol=="glFogfv"?1:2),count*4);
    }else if(symbol=="glGenTextures" || symbol=="glDeleteTextures"){
        if(n(0)<0 || d(0)>transfer_limit/4){error(c,invalid_value);return finish(0);}if(symbol=="glGenTextures")set_output(arg(1),d(0)*4);else set_input(arg(1),d(0)*4);
    }else if(symbol=="glGetIntegerv" || symbol=="glGetFloatv" || symbol=="glGetDoublev" || symbol=="glGetBooleanv"){
        const auto size=query_size(d(0));if(!size){error(c,invalid_enum);return finish(0);}const auto unit=symbol=="glGetBooleanv"?1U:symbol=="glGetDoublev"?8U:4U;set_output(arg(1),size*unit);
    }else if(symbol=="glTexEnviv" || symbol=="glTexEnvfv" || symbol=="glGetTexEnviv" || symbol=="glGetTexEnvfv"){
        if(d(0)!=0x2300 || (d(1)!=0x2200 && d(1)!=0x2201)){error(c,invalid_enum);return finish(0);}
        const auto bytes=d(1)==0x2201?16U:4U;if(symbol.starts_with("glGet"))set_output(arg(2),bytes);else set_input(arg(2),bytes);
    }else if(symbol=="glTexImage2D" || symbol=="glTexSubImage2D"){
        const bool sub=symbol=="glTexSubImage2D",proxy=!sub && d(0)==0x8064;const unsigned wi=sub?4:3,hi=sub?5:4;
        if(n(wi)<0 || n(hi)<0 || (!proxy && (n(wi)>16384 || n(hi)>16384))){error(c,invalid_value);return finish(0);}if(!proxy && arg(8)){const auto bytes=pixel_bytes(c.client,n(wi),n(hi),d(6),d(7),false);if(!bytes){error(c,invalid_enum);return finish(0);}set_input(arg(8),*bytes);}
    }else if(symbol=="glBitmap"){
        if(n(0)<0 || n(1)<0){error(c,invalid_value);return finish(0);}if(arg(6)){const auto bytes=pixel_bytes(c.client,n(0),n(1),0x1900,0x1a00,false);if(!bytes){error(c,invalid_value);return finish(0);}set_input(arg(6),*bytes);}
    }else if(symbol=="glReadPixels"){
        if(n(2)<0 || n(3)<0){error(c,invalid_value);return finish(0);}const auto bytes=pixel_bytes(c.client,n(2),n(3),d(4),d(5),true);if(!bytes){error(c,invalid_enum);return finish(0);}set_output(arg(6),*bytes);
        // pack row/skip 留白依規格不能覆蓋，先保留客體現有值。
        if(!output.empty())memory.read(GuestAddress{output_address},output);
    }else if(symbol=="glPixelStorei"){
        const auto pname=d(0);if(!((pname>=0xcf0&&pname<=0xcf5)||(pname>=0xd00&&pname<=0xd05))){error(c,invalid_enum);return finish(0);}
        const auto index=pname&15;if(n(1)<0 || (index==5 && n(1)!=1&&n(1)!=2&&n(1)!=4&&n(1)!=8)){error(c,invalid_value);return finish(0);}c.client.pixel_store[pname]=(index<=1)?(n(1)?1:0):n(1);
    }else if(symbol=="glPushClientAttrib"){
        if((d(0)&~3U) && d(0)!=UINT32_MAX){error(c,invalid_value);return finish(0);}if(c.client_stack.size()>=16){error(c,0x503);return finish(0);}c.client_stack.emplace_back(d(0),c.client);
    }else if(symbol=="glPopClientAttrib"){
        if(c.client_stack.empty()){error(c,0x504);return finish(0);}const auto saved=c.client_stack.back();c.client_stack.pop_back();if(saved.first&1)c.client.pixel_store=saved.second.pixel_store;if(saved.first&2)c.client.arrays=saved.second.arrays;
    }else if(symbol=="glDrawArrays" || symbol=="glDrawElements"){
        const bool indexed=symbol=="glDrawElements";const auto count=n(indexed?1:2);if(count<0 || (!indexed && n(1)<0)){error(c,invalid_value);return finish(0);}if(d(0)>9){error(c,invalid_enum);return finish(0);}if(!count)return finish(0);
        std::uint32_t first=indexed?UINT32_MAX:d(1),last=indexed?0:d(1)+static_cast<std::uint32_t>(count)-1;
        if(!indexed && last<first){error(c,invalid_value);return finish(0);}
        auto& indices=c.index_scratch;
        if(indexed){const auto type=d(2);if(type!=0x1401&&type!=0x1403&&type!=0x1405){error(c,invalid_enum);return finish(0);}const auto unit=type_size(type);const auto length=static_cast<std::uint64_t>(count)*unit;if(length>transfer_limit){error(c,out_of_memory);return finish(0);}input.resize(static_cast<std::size_t>(length));OpenGlArray elements;elements.pointer=arg(3);elements.buffer=c.native->bound_buffer(0x8893);if(!array_read(elements,0,input))return finish(0);const auto& bytes=input;indices.resize(static_cast<std::size_t>(count));
            for(std::size_t i=0;i<indices.size();++i){std::uint32_t value=0;for(unsigned j=0;j<unit;++j)value|=std::uint32_t(bytes[i*unit+j])<<(j*8);indices[i]=value;first=std::min(first,value);last=std::max(last,value);}}
        const auto extent=static_cast<std::uint64_t>(last)-first+1;if(extent>transfer_limit/32){error(c,out_of_memory);return finish(0);}
        for(unsigned i=0;i<4;++i){const auto& a=c.client.arrays[i];if(!a.enabled){if(!upload(i,a.size,a.type,{},false))return finish(0);continue;}
            const auto element=type_size(a.type)*static_cast<std::uint32_t>(a.size),stride=a.stride?static_cast<std::uint32_t>(a.stride):element;
            const auto offset=static_cast<std::uint64_t>(first)*stride,length=(extent-1)*stride+element;if(length>transfer_limit){error(c,out_of_memory);return finish(0);}
            auto& bytes=c.array_scratch;bytes.resize(static_cast<std::size_t>(length));if(!array_read(a,offset,bytes))return finish(0);
            if(stride!=element){auto& packed=c.packed_scratch;packed.resize(static_cast<std::size_t>(extent*element));for(std::size_t j=0;j<extent;++j)std::memcpy(packed.data()+j*element,bytes.data()+j*stride,element);if(!upload(i,a.size,a.type,packed,true))return finish(0);}
            else if(!upload(i,a.size,a.type,bytes,true))return finish(0);
        }
        if(indexed){for(auto& index:indices)index-=first;input.resize(indices.size()*4);std::memcpy(input.data(),indices.data(),input.size());values[2]=0x1405;}
        else values[1]=0;
        ++c.draws;
    }
    if(std::getenv("X64ARM_GL_TRACE") && (found->operation==GlOperation::TexImage2D || found->operation==GlOperation::TexParameteri)){
        std::fprintf(stderr,"[gl-texture] %.*s",static_cast<int>(symbol.size()),symbol.data());
        for(auto value:values)std::fprintf(stderr," %llx",static_cast<unsigned long long>(value));std::fprintf(stderr,"\n");
    }
    std::array<bool,4> deleted_client_buffers{};
    if(symbol=="glDeleteBuffers")for(std::size_t i=0;i<input.size();i+=4){std::uint32_t name;std::memcpy(&name,input.data()+i,4);const auto buffer=c.native->buffer_object(name);
        if(buffer)for(unsigned j=0;j<4;++j)if(c.client.arrays[j].buffer==buffer)deleted_client_buffers[j]=true;}
    std::uint64_t result=0;try{result=c.native->execute(found->operation,values,input,output);}catch(const std::bad_alloc&){
        if(std::getenv("X64ARM_GL_TRACE"))std::fprintf(stderr,"[gl-allocation-failed] operation=%u\n",static_cast<unsigned>(found->operation));
        error(c,out_of_memory);return finish(0);}

    if(symbol=="glUseProgram" && result)c.program=d(0);
    if(symbol=="glDeleteBuffers" && result)for(unsigned i=0;i<4;++i)if(deleted_client_buffers[i])c.client.arrays[i].buffer.reset();
    if(symbol=="glBindVertexArray" && result){
        c.vertex_array_clients[c.vertex_array]=c.client.arrays;c.vertex_array=d(0);
        auto [it,added]=c.vertex_array_clients.try_emplace(c.vertex_array);if(added)it->second[1].size=3;c.client.arrays=it->second;
    }
    if(symbol=="glDeleteVertexArrays" && result){for(std::size_t i=0;i<input.size();i+=4){std::uint32_t name;std::memcpy(&name,input.data()+i,4);if(!name)continue;
        if(c.vertex_array==name){c.vertex_array=0;auto [it,added]=c.vertex_array_clients.try_emplace(0);if(added)it->second[1].size=3;c.client.arrays=it->second;}c.vertex_array_clients.erase(name);}}
    if(!result && (symbol=="glGetBufferParameteriv" || symbol=="glGetBufferSubData" || symbol=="glGetVertexAttribiv" || symbol=="glGetVertexAttribfv" || symbol=="glGetVertexAttribPointerv"))output.clear();
    if((symbol=="glGetShaderInfoLog" || symbol=="glGetProgramInfoLog") && arg(2))memory.store(GuestAddress{arg(2)},32,result);
    if(!output.empty())memory.write(GuestAddress{output_address},output);
    return finish(result);
}
std::string WindowsServices::opengl_diagnostics()const{
    std::ostringstream stream;
    for(const auto& entry:opengl_.completed)stream<<entry;
    for(const auto& [handle,c]:opengl_.contexts)stream<<context_diagnostics(handle,c);
    return stream.str();
}
}

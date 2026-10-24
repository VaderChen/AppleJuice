#pragma once
#include "x64arm/windows_abi.hpp"
#include "x64arm/windows_graphics.hpp"
#include <array>
#include <map>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>
#include <span>
#include <string>
#include <vector>
namespace x64arm {
enum class GlOperation {
    Begin,End,Vertex2f,Vertex2i,Vertex3f,Vertex3fv,Vertex4fv,Normal3f,Normal3fv,Color3f,Color4f,Color4fv,TexCoord2f,TexCoord2fv,TexCoord4fv,
    Clear,ClearColor,ClearDepth,Enable,Disable,CullFace,FrontFace,BlendFunc,AlphaFunc,StencilFunc,StencilMask,StencilOp,ClearStencil,DepthFunc,DepthMask,DepthRange,ShadeModel,Hint,Viewport,Scissor,
    MatrixMode,LoadIdentity,LoadMatrixf,MultMatrixf,PushMatrix,PopMatrix,Translatef,Rotatef,Scalef,Ortho,Frustum,Perspective,
    Lightfv,Materialf,Materialfv,Fogf,Fogi,Fogfv,PushAttrib,PopAttrib,PushClientAttrib,PopClientAttrib,
    TextureCombine,TexEnvi,TexEnvf,TexEnviv,TexEnvfv,GetTexEnviv,GetTexEnvfv,BindTexture,GenTextures,DeleteTextures,TexImage2D,TexSubImage2D,TexParameterf,TexParameteri,PixelStorei,
    NewList,EndList,GenLists,DeleteLists,CallList,DrawArrays,DrawElements,Bitmap,RasterPos2i,RasterPos3f,
    DrawBuffer,ReadBuffer,GetError,GetIntegerv,GetFloatv,GetDoublev,GetBooleanv,IsEnabled,ReadPixels,Flush,Finish,
    Color4dv,Normal3dv,TexCoord4dv,Vertex4dv,
    CreateShader,ShaderSource,CompileShader,GetShaderiv,GetShaderInfoLog,DeleteShader,CreateProgram,AttachShader,DetachShader,BindAttribLocation,LinkProgram,GetProgramiv,GetProgramInfoLog,DeleteProgram,UseProgram,GetUniformLocation,Uniform1fv,Uniform2fv,Uniform3fv,Uniform4fv,Uniform1iv,Uniform2iv,Uniform3iv,Uniform4iv,UniformMatrix4fv,GenBuffers,BindBuffer,BufferData,BufferSubData,DeleteBuffers,BindBufferBase,EnableVertexAttribArray,DisableVertexAttribArray,VertexAttribPointer,VertexAttrib1f,VertexAttrib3f,VertexAttrib4f,VertexAttribDivisor,DrawArraysInstanced,DrawElementsInstanced,GenFramebuffers,BindFramebuffer,DeleteFramebuffers,FramebufferTexture2D,CheckFramebufferStatus,DrawBuffers,ActiveTexture,GenerateMipmap,BlendEquationSeparate,BlendFuncSeparate,PolygonMode,
    GenVertexArrays,BindVertexArray,DeleteVertexArrays,IsVertexArray,IsBuffer,GetBufferParameteriv,GetBufferSubData,CopyBufferSubData,
    GetVertexAttribiv,GetVertexAttribfv,GetVertexAttribPointerv,VertexAttrib2f,GetTexLevelParameteriv,GetTexLevelParameterfv,IsTexture,
    ColorMask,LineWidth,PixelZoom,RenderMode
};
struct OpenGlRendererInfo { std::string vendor,renderer,version;bool accelerated{}; };
struct OpenGlBuffer {std::vector<std::uint8_t> bytes;std::uint32_t name{},usage{0x88e4};bool created{};};
// wglShareLists：Shared 表示加入 source 的物件群組；Busy 表示目標已有自己的共用物件；Unsupported 表示後端或裝置不同而無法共用。
enum class OpenGlShareResult {Shared,Busy,Unsupported};
class NativeOpenGlContext:public NativeGraphics {
public:
    virtual ~NativeOpenGlContext()=default;
    virtual OpenGlShareResult share_objects(NativeOpenGlContext& source){return &source==this?OpenGlShareResult::Shared:OpenGlShareResult::Unsupported;}
    virtual bool programmable()const noexcept{return false;}
    virtual std::shared_ptr<OpenGlBuffer> buffer_object(std::uint32_t){return {};}
    virtual std::shared_ptr<OpenGlBuffer> bound_buffer(std::uint32_t){return {};}
    virtual void bind()=0;
    virtual void drawable(void* view,std::uint32_t width,std::uint32_t height)=0;
    virtual bool swap()=0;
    virtual bool swap_interval(std::int32_t interval)=0;
    virtual std::uint64_t execute(GlOperation,std::span<const std::uint64_t> args={},std::span<const std::uint8_t> data={},std::span<std::uint8_t> output={})=0;
    virtual void array(unsigned index,std::int32_t size,std::uint32_t type,std::span<const std::uint8_t> bytes,bool enabled)=0;
    virtual std::string string(std::uint32_t name)=0;
    virtual OpenGlRendererInfo info() const=0;
    virtual std::string statistics() const{return {};}
    virtual std::string capture(const std::filesystem::path& path)=0;
    // 將固定管線的實際 backing 暴露為共用繪圖 view；兩種管線維持同一份色彩與深度。
    virtual GraphicsView graphics_drawable(bool depth){(void)depth;return {};}
};
std::shared_ptr<NativeOpenGlContext> make_metal_opengl(void* view,std::uint32_t width,std::uint32_t height);
// 包裝後端，讓繪製／運算／寫入／複製／清除在專用宿主執行緒依序執行（見 metal_command_thread.inc）。
std::shared_ptr<NativeOpenGlContext> make_metal_command_thread(std::shared_ptr<NativeOpenGlContext> inner);
// 不建立 Metal 裝置即可確定 make_metal_opengl 會回報的 renderer／vendor 時回傳兩者；否則回空，呼叫端改建裝置。
std::optional<std::pair<std::string,std::string>> metal_opengl_identity();
std::shared_ptr<NativeOpenGlContext> make_native_opengl(void* view,std::uint32_t width,std::uint32_t height);
struct OpenGlArray { std::uint64_t pointer{};std::int32_t size{4},stride{};std::uint32_t type{0x1406};bool enabled{};std::shared_ptr<OpenGlBuffer> buffer;bool operator==(const OpenGlArray&)const=default; };
struct OpenGlClientState {
    std::array<OpenGlArray,4> arrays{};
    std::map<std::uint32_t,std::int32_t> pixel_store{{0xcf5,4},{0xd05,4}};
    bool operator==(const OpenGlClientState&)const=default;
};
struct OpenGlContext {
    std::shared_ptr<NativeOpenGlContext> native;
    std::uint64_t dc{},thread{},window{},calls{},draws{},frames{},capture_next_milliseconds{};
    OpenGlClientState client;
    std::vector<std::pair<std::uint32_t,OpenGlClientState>> client_stack;
    std::uint32_t program{};
    std::uint32_t vertex_array{};
    std::map<std::uint32_t,std::array<OpenGlArray,4>> vertex_array_clients;
    std::uint32_t error{};std::int32_t interval{1};bool beginning{};
    std::map<std::uint32_t,std::uint64_t> strings;
    // 每個 context 保留 staging 容量；內容每次從客體重新讀入。
    std::vector<std::uint8_t> input_scratch,output_scratch,array_scratch,packed_scratch;
    std::vector<std::uint32_t> index_scratch;
};
struct OpenGlState {
    std::map<std::uint64_t,OpenGlContext> contexts;
    std::map<std::uint64_t,std::uint64_t> current;
    std::map<std::uint64_t,std::uint32_t> pixel_formats;
    // EXT_swap_control 的 interval 是視窗狀態；context.interval 僅記錄已套用的 host 值。
    std::map<std::uint64_t,std::int32_t> swap_intervals;
    std::uint64_t next_context{0x90000000};
    std::uint64_t extensions_string{};
    std::vector<std::string> completed;
};
std::span<const WindowsServiceDescriptor> opengl_service_descriptors();
}

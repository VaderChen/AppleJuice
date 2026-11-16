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
    ColorMask,LineWidth,PixelZoom,RenderMode,CopyTexSubImage2D,GetTexParameteriv,GetTexParameterfv,PolygonOffset,PointSize,IndexMask,
    // 立即模式型別變體共用的雙精度入口與 GL 1.1 其餘固定管線狀態；新增值放在最後，不改動既有編號。
    RasterPos4dv,Indexd,EdgeFlag,ClearIndex,IsList,ListBase,CallLists,LightModelfv,GetLightfv,GetMaterialfv,ColorMaterial,
    CopyTexImage2D,PrioritizeTextures,TexParameterfv,
    // 求值器：服務層把 f／d 變體一律換成雙精度；Map1d／Map2d 的控制點以 [u][v][分量] 緊密排列放在 data，GetMapdv 回傳寫入的 double 數。
    Map1d,Map2d,MapGrid1d,MapGrid2d,EvalCoord1d,EvalCoord2d,EvalMesh1,EvalMesh2,EvalPoint1,EvalPoint2,GetMapdv,
    // glTexGen／glGetTexGen：args 為 {coord,pname}，data／output 為 1（GL_TEXTURE_GEN_MODE）或 4（平面）個 double；GetTexGendv 回傳寫入的 double 數。
    TexGendv,GetTexGendv,
    // 選取／回饋模式：SelectBuffer {size}、FeedbackBuffer {size,type}、名稱堆疊操作與 PassThrough {float 位元}。
    // RenderMode 離開 SELECT／FEEDBACK 時在 output 寫入「字數＋內容」（命中紀錄為 uint32，回饋值為 float），回傳值為命中數／值數或溢位時的 -1。
    SelectBuffer,InitNames,LoadName,PushName,PopName,FeedbackBuffer,PassThrough,
    // 使用者剪裁面：args {plane}，ClipPlane 的 data 與 GetClipPlane 的 output 皆為 4 個 double。
    ClipPlane,GetClipPlane,
    // glGetTexImage：args {target,level,format,type}，output 依 GL_PACK_* 版面；回傳寫入的位元組數。
    GetTexImage,
    // 點畫：LineStipple {factor,pattern}；PolygonStipple 的 data 為依 GL_UNPACK_* 排列的 32×32 位元；GetPolygonStipple 的 output 依 GL_PACK_*。
    LineStipple,PolygonStipple,GetPolygonStipple,
    // 像素傳輸：PixelTransfer {pname,double 位元}；PixelMap {map,mapsize,type}，data 為 mapsize 個 type（FLOAT／UNSIGNED_INT／UNSIGNED_SHORT）值；
    // GetPixelMap {map,type}，output 為該表全部項目；DrawPixels {width,height,format,type}，data 依 GL_UNPACK_*；CopyPixels {x,y,width,height,type}。
    PixelTransfer,PixelMap,GetPixelMap,DrawPixels,CopyPixels,
    // 累積緩衝與邏輯運算：Accum {op,value 浮點位元}；ClearAccum {r,g,b,a 浮點位元}；LogicOp {opcode}。
    Accum,ClearAccum,LogicOp
};
// GL_MAP1_*（0x0D90～0x0D98）與 GL_MAP2_*（0x0DB0～0x0DB8）每個控制點的分量數；其他值為 0。服務層、各後端與 GLU NURBS 共用。
constexpr unsigned gl_map_components(std::uint32_t target){
    if(!((target>=0xd90 && target<=0xd98) || (target>=0xdb0 && target<=0xdb8)))return 0;
    constexpr unsigned counts[]{4,1,3,1,2,3,4,3,4}; // COLOR_4、INDEX、NORMAL、TEXTURE_COORD_1～4、VERTEX_3、VERTEX_4
    return counts[target&0xf];
}
// GL_MAX_EVAL_ORDER：Metal 後端回報的上限，服務層也以此限制讀取的控制點數。
constexpr unsigned gl_max_eval_order=30;
struct OpenGlRendererInfo { std::string vendor,renderer,version;bool accelerated{}; };
struct OpenGlBuffer {std::vector<std::uint8_t> bytes;std::uint32_t name{},usage{0x88e4};bool created{};};
// wglShareLists：Shared 表示加入 source 的物件群組；Busy 表示目標已有自己的共用物件；Unsupported 表示後端或裝置不同而無法共用。
enum class OpenGlShareResult {Shared,Busy,Unsupported};
class NativeOpenGlContext:public NativeGraphics {
public:
    virtual ~NativeOpenGlContext()=default;
    virtual OpenGlShareResult share_objects(NativeOpenGlContext& source){return &source==this?OpenGlShareResult::Shared:OpenGlShareResult::Unsupported;}
    virtual bool programmable()const noexcept{return false;}
    // wglCopyContext：把 source 中依 glPushAttrib 位元選定的狀態群組套到這個 context；不支援時回傳 false。
    virtual bool copy_state(NativeOpenGlContext&,std::uint32_t){return false;}
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
// on_error：設定時背景工作的例外交給它記錄（在命令執行緒上呼叫），不留到下一次同步時在呼叫端拋出；
// 多個執行緒共用後端時（D3D12），延後的例外否則會在無關的呼叫端出現。
std::shared_ptr<NativeOpenGlContext> make_metal_command_thread(std::shared_ptr<NativeOpenGlContext> inner,
    std::function<void(const std::string&)> on_error={});
// 不建立 Metal 裝置即可確定 make_metal_opengl 會回報的 renderer／vendor 時回傳兩者；否則回空，呼叫端改建裝置。
std::optional<std::pair<std::string,std::string>> metal_opengl_identity();
std::shared_ptr<NativeOpenGlContext> make_native_opengl(void* view,std::uint32_t width,std::uint32_t height);
struct OpenGlArray { std::uint64_t pointer{};std::int32_t size{4},stride{};std::uint32_t type{0x1406};bool enabled{};std::shared_ptr<OpenGlBuffer> buffer;bool operator==(const OpenGlArray&)const=default; };
inline OpenGlArray opengl_array(std::int32_t size,std::uint32_t type){OpenGlArray array;array.size=size;array.type=type;return array;}
// 用戶端陣列：0 頂點、1 法線（固定 3 分量）、2 顏色、3 貼圖座標，以上四個交給後端上傳；4 邊旗標（GLboolean）、5 色彩索引（預設 GL_FLOAT）只由服務層逐元素送出。
struct OpenGlClientState {
    std::array<OpenGlArray,6> arrays{OpenGlArray{},opengl_array(3,0x1406),OpenGlArray{},OpenGlArray{},opengl_array(1,0x1401),opengl_array(1,0x1406)};
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
    std::map<std::uint32_t,std::array<OpenGlArray,6>> vertex_array_clients;
    std::uint32_t error{};std::int32_t interval{1};bool beginning{};
    // glSelectBuffer／glFeedbackBuffer 的客體緩衝與大小（以 32 位元字計）；render_mode 為服務層記錄的 GL_RENDER_MODE，用來在離開時寫回客體。
    std::uint64_t select_buffer{},feedback_buffer{};std::uint32_t select_size{},feedback_size{},feedback_type{},render_mode{0x1c00};bool select_set{},feedback_set{};
    std::map<std::uint32_t,std::uint64_t> strings;
    // 每個 context 保留 staging 容量；內容每次從客體重新讀入。
    std::vector<std::uint8_t> input_scratch,output_scratch,array_scratch,packed_scratch;
    std::vector<std::uint32_t> index_scratch;
};
// gluNewQuadric 的狀態（預設 GLU_SMOOTH、GLU_FILL、GLU_OUTSIDE、不產生貼圖座標）。
struct GluQuadric {std::uint32_t normals{100000},style{100012},orientation{100020};bool texture{};std::uint64_t callback{};bool operator==(const GluQuadric&)const=default;};
// gluNewTess 的狀態：回呼依 GLU_TESS_BEGIN（100100）起算的 12 個槽位；state 0 休眠、1 多邊形中、2 輪廓中。
struct GluTessVertex {std::array<double,3> coords{};std::uint64_t data{};bool operator==(const GluTessVertex&)const=default;};
struct GluTessellator {
    std::array<std::uint64_t,12> callbacks{};std::uint32_t winding{100130};bool boundary_only{};double tolerance{};std::array<double,3> normal{};
    std::uint32_t state{};std::uint64_t polygon_data{};std::vector<std::vector<GluTessVertex>> contours;
    bool operator==(const GluTessellator&)const=default;
};
// gluNewNurbsRenderer 的狀態（windows_glu_nurbs.cpp）：控制點一律保存為 4 個 float；callbacks[0] 為 GLU_ERROR，其餘依 GLU_NURBS_BEGIN（100164）起算。
struct GluNurbsCurveData {std::vector<float> knots;std::vector<std::array<float,4>> points;std::uint32_t order{},type{};bool operator==(const GluNurbsCurveData&)const=default;};
struct GluNurbsSurfaceData {std::vector<float> sknots,tknots;std::vector<std::array<float,4>> points;std::uint32_t scount{},tcount{},sorder{},torder{},type{};bool operator==(const GluNurbsSurfaceData&)const=default;};
struct GluNurbsTrimPiece {bool pwl{};std::vector<std::array<float,3>> points;GluNurbsCurveData curve;bool operator==(const GluNurbsTrimPiece&)const=default;};
struct GluNurbs {
    std::uint32_t display{100012},sampling{100215},mode{100162};float tolerance{50.F},parametric{0.5F},ustep{100.F},vstep{100.F};bool culling{},auto_load{true},loaded{};
    std::array<float,16> model{},projection{};std::array<std::int32_t,4> viewport{};
    std::array<std::uint64_t,13> callbacks{};std::uint32_t state{};
    std::vector<GluNurbsCurveData> curves;std::vector<GluNurbsSurfaceData> surfaces;std::vector<std::vector<GluNurbsTrimPiece>> trims;
    bool operator==(const GluNurbs&)const=default;
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
    // GLU（windows_glu.cpp）：二次曲面以客體堆積位址為鍵；錯誤與版本字串在第一次查詢時配置於客體記憶體。
    std::map<std::uint64_t,GluQuadric> glu_quadrics;
    std::map<std::uint64_t,std::uint64_t> glu_strings;
    std::map<std::uint64_t,GluTessellator> glu_tessellators;
    std::map<std::uint64_t,GluNurbs> glu_nurbs;
};
std::span<const WindowsServiceDescriptor> opengl_service_descriptors();
}

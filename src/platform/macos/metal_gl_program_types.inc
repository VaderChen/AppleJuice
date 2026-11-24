struct GlShader {GlslSource source;std::string log;bool compiled{},deleted{};};
struct GlProgramStage {GlslStage reflection;id<MTLFunction> function,flipped;id<MTLArgumentEncoder> arguments,flipped_arguments;std::vector<std::uint8_t> uniforms;std::vector<std::vector<int>> units;};
struct GlUniformTarget {unsigned stage{},index{},element{};bool sampler{};};
struct GlUniformLocation {unsigned type{},components{},columns{},remaining{};std::vector<GlUniformTarget> targets;};
struct GlProgram {
    std::map<unsigned,std::shared_ptr<GlShader>> attached;
    std::map<std::string,unsigned> attribute_bindings;
    std::map<std::string,int> locations;
    std::vector<GlUniformLocation> uniforms;
    std::vector<GlProgramStage> stages;
    // 共用 context 快取使用全程序唯一的 link 世代，刪除／重連後不可命中舊管線。
    std::uint64_t pipeline_generation{};
    std::string log;bool linked{},deleted{};
};
struct GlBuffer:OpenGlBuffer {std::shared_ptr<GraphicsResource> resource;};
// client：VAO 0 未綁定 ARRAY_BUFFER 時的用戶端陣列，offset 保存客體指標；client_data 為服務層在繪製前送入、從指標起算的頂點位元組。
struct GlVertexAttribute {std::shared_ptr<GlBuffer> buffer;std::uint64_t offset{};unsigned components{4},type{0x1406},stride{},divisor{};bool enabled{},normalized{},client{};std::shared_ptr<GlBuffer> client_data;};
struct GlVertexArray {std::array<GlVertexAttribute,16> attributes;std::shared_ptr<GlBuffer> elements;bool created{};};
// renderbuffer：format 為 glRenderbufferStorage 的內部格式（初始 GL_RGBA）；samples 為實際配置的樣本數（0 表示單一樣本）。
// bound 記錄名稱是否已經由 glBindRenderbuffer 建立物件（glIsRenderbuffer 只對已建立的物件回 TRUE）。
struct GlRenderbuffer {Texture texture;unsigned format{0x1908},width{},height{},samples{};bool bound{};};
// 附件：貼圖附件每次使用時依名稱重新取得 textures_ 的最新內容；renderbuffer 附件保留物件本身，刪除名稱後仍可使用到解除附加。
struct GlAttachment {unsigned name{},level{};Texture texture;std::shared_ptr<GlRenderbuffer> renderbuffer;unsigned texture_target{};};
struct GlFramebuffer {std::map<unsigned,GlAttachment> attachments;std::vector<unsigned> draw_buffers{0x8ce0};unsigned read_buffer{0x8ce0};bool bound{};};
struct GlProgrammable {
    // shader、program 與 buffer 屬 wglShareLists 的共用物件；framebuffer 與 VAO 是容器物件，各 context 獨立。
    GlSharedNames<std::shared_ptr<GlShader>> shaders;
    GlSharedNames<std::shared_ptr<GlProgram>> programs;
    GlSharedNames<std::shared_ptr<GlBuffer>> buffers;
    // renderbuffer 與貼圖相同屬共用物件；名稱已配置但尚未綁定時值為空指標。
    GlSharedNames<std::shared_ptr<GlRenderbuffer>> renderbuffers;
    std::map<unsigned,std::shared_ptr<GlFramebuffer>> framebuffers;
    std::map<unsigned,unsigned> buffer_bindings,indexed_bindings;
    std::map<unsigned,GlVertexArray> vertex_arrays{{0,{}}};
    // CURRENT_VERTEX_ATTRIB 是 context 狀態，不隨 VAO 綁定切換。
    std::array<std::array<float,4>,16> attribute_values=[] {std::array<std::array<float,4>,16> values;values.fill({0,0,0,1});return values;}();
    unsigned next_vertex_array{1},vertex_array{};
    GlVertexArray& vao(){return vertex_arrays.at(vertex_array);}
    std::array<unsigned,32> texture_units{};
    std::shared_ptr<GlProgram> current;
    GlSharedCounter next_object{1},next_buffer{1},next_renderbuffer{1};
    unsigned next_framebuffer{1},program{},active_texture{},draw_framebuffer{},read_framebuffer{},renderbuffer{};
    // glBlendColor 的常數色（CONSTANT_COLOR／CONSTANT_ALPHA 系列混色因子），固定與可程式化管線共用。
    std::array<float,4> blend_color{};
    unsigned src_alpha{1},dst_alpha{},equation_rgb{0x8006},equation_alpha{0x8006},polygon_mode{0x1b02};
    std::uint64_t compiles{},links{},compile_ns{},link_ns{};
    std::uint64_t buffer_uploads{},buffer_upload_bytes{},direct_vertex_bindings{},direct_index_draws{},converted_vertices{};
};
// renderbuffer 內部格式對應的 Metal 格式、每像素位元組，以及 GL_RENDERBUFFER_{RED,GREEN,BLUE,ALPHA,DEPTH,STENCIL}_SIZE 回報的位元數。
// RGB 類格式以 RGBA 儲存但回報 0 個 alpha 位元；深度一律以 32 位元浮點保存。
struct GlRenderbufferFormat {MTLPixelFormat format;unsigned bytes;std::array<unsigned,6> bits;};
inline std::optional<GlRenderbufferFormat> gl_renderbuffer_format(unsigned internal){
    switch(internal){
    case 0x1908:case 0x8058:case 0x8056:case 0x8057:return GlRenderbufferFormat{MTLPixelFormatRGBA8Unorm,4,{8,8,8,8,0,0}};
    case 0x1907:case 0x8051:case 0x8d62:case 0x804f:case 0x8050:return GlRenderbufferFormat{MTLPixelFormatRGBA8Unorm,4,{8,8,8,0,0,0}};
    case 0x8059:case 0x805b:return GlRenderbufferFormat{MTLPixelFormatRGBA16Unorm,8,{16,16,16,16,0,0}};
    case 0x8229:return GlRenderbufferFormat{MTLPixelFormatR8Unorm,1,{8,0,0,0,0,0}};
    case 0x822b:return GlRenderbufferFormat{MTLPixelFormatRG8Unorm,2,{8,8,0,0,0,0}};
    case 0x822a:return GlRenderbufferFormat{MTLPixelFormatR16Unorm,2,{16,0,0,0,0,0}};
    case 0x822c:return GlRenderbufferFormat{MTLPixelFormatRG16Unorm,4,{16,16,0,0,0,0}};
    case 0x822d:return GlRenderbufferFormat{MTLPixelFormatR16Float,2,{16,0,0,0,0,0}};
    case 0x822f:return GlRenderbufferFormat{MTLPixelFormatRG16Float,4,{16,16,0,0,0,0}};
    case 0x881a:return GlRenderbufferFormat{MTLPixelFormatRGBA16Float,8,{16,16,16,16,0,0}};
    case 0x881b:return GlRenderbufferFormat{MTLPixelFormatRGBA16Float,8,{16,16,16,0,0,0}};
    case 0x822e:return GlRenderbufferFormat{MTLPixelFormatR32Float,4,{32,0,0,0,0,0}};
    case 0x8230:return GlRenderbufferFormat{MTLPixelFormatRG32Float,8,{32,32,0,0,0,0}};
    case 0x8814:return GlRenderbufferFormat{MTLPixelFormatRGBA32Float,16,{32,32,32,32,0,0}};
    case 0x8815:return GlRenderbufferFormat{MTLPixelFormatRGBA32Float,16,{32,32,32,0,0,0}};
    case 0x8c3a:return GlRenderbufferFormat{MTLPixelFormatRG11B10Float,4,{11,11,10,0,0,0}};
    case 0x1902:case 0x81a5:case 0x81a6:case 0x81a7:case 0x8cac:return GlRenderbufferFormat{MTLPixelFormatDepth32Float,4,{0,0,0,0,32,0}};
    case 0x84f9:case 0x88f0:case 0x8cad:return GlRenderbufferFormat{MTLPixelFormatDepth32Float_Stencil8,8,{0,0,0,0,32,8}};
    case 0x8d46:case 0x8d47:case 0x8d48:case 0x8d49:return GlRenderbufferFormat{MTLPixelFormatStencil8,1,{0,0,0,0,0,8}};
    default:return std::nullopt;}
}
// 繪製目標格式的各通道位元數（GL_FRAMEBUFFER_ATTACHMENT_*_SIZE 與 GetTexLevelParameter 共用）：紅、綠、藍、alpha、深度、模板。
inline std::array<unsigned,6> gl_pixel_format_bits(MTLPixelFormat format){
    switch(format){
    case MTLPixelFormatR8Unorm:return {8,0,0,0,0,0};case MTLPixelFormatRG8Unorm:return {8,8,0,0,0,0};case MTLPixelFormatRGBA8Unorm:case MTLPixelFormatBGRA8Unorm:return {8,8,8,8,0,0};
    case MTLPixelFormatR16Unorm:case MTLPixelFormatR16Float:return {16,0,0,0,0,0};case MTLPixelFormatRG16Unorm:case MTLPixelFormatRG16Float:return {16,16,0,0,0,0};
    case MTLPixelFormatRGBA16Unorm:case MTLPixelFormatRGBA16Float:return {16,16,16,16,0,0};case MTLPixelFormatR32Float:return {32,0,0,0,0,0};case MTLPixelFormatRG32Float:return {32,32,0,0,0,0};
    case MTLPixelFormatRGBA32Float:return {32,32,32,32,0,0};case MTLPixelFormatRG11B10Float:return {11,11,10,0,0,0};
    case MTLPixelFormatDepth32Float:return {0,0,0,0,32,0};case MTLPixelFormatDepth32Float_Stencil8:return {0,0,0,0,32,8};case MTLPixelFormatStencil8:return {0,0,0,0,0,8};
    default:return {};}
}

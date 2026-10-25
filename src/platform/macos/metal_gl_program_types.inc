struct GlShader {GlslSource source;std::string log;bool compiled{},deleted{};};
struct GlProgramStage {GlslStage reflection;id<MTLFunction> function,flipped;std::vector<std::uint8_t> uniforms;std::vector<std::vector<int>> units;};
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
struct GlVertexAttribute {std::shared_ptr<GlBuffer> buffer;std::uint64_t offset{};unsigned components{4},type{0x1406},stride{},divisor{};bool enabled{},normalized{};};
struct GlVertexArray {std::array<GlVertexAttribute,16> attributes;std::shared_ptr<GlBuffer> elements;bool created{};};
struct GlAttachment {unsigned name{},level{};Texture texture;};
struct GlFramebuffer {std::map<unsigned,GlAttachment> attachments;std::vector<unsigned> draw_buffers{0x8ce0};unsigned read_buffer{0x8ce0};};
struct GlProgrammable {
    // shader、program 與 buffer 屬 wglShareLists 的共用物件；framebuffer 與 VAO 是容器物件，各 context 獨立。
    GlSharedNames<std::shared_ptr<GlShader>> shaders;
    GlSharedNames<std::shared_ptr<GlProgram>> programs;
    GlSharedNames<std::shared_ptr<GlBuffer>> buffers;
    std::map<unsigned,std::shared_ptr<GlFramebuffer>> framebuffers;
    std::map<unsigned,unsigned> buffer_bindings,indexed_bindings;
    std::map<unsigned,GlVertexArray> vertex_arrays{{0,{}}};
    // CURRENT_VERTEX_ATTRIB 是 context 狀態，不隨 VAO 綁定切換。
    std::array<std::array<float,4>,16> attribute_values=[] {std::array<std::array<float,4>,16> values;values.fill({0,0,0,1});return values;}();
    unsigned next_vertex_array{1},vertex_array{};
    GlVertexArray& vao(){return vertex_arrays.at(vertex_array);}
    std::array<unsigned,32> texture_units{};
    std::shared_ptr<GlProgram> current;
    GlSharedCounter next_object{1},next_buffer{1};
    unsigned next_framebuffer{1},program{},active_texture{},draw_framebuffer{},read_framebuffer{};
    unsigned src_alpha{1},dst_alpha{},equation_rgb{0x8006},equation_alpha{0x8006},polygon_mode{0x1b02};
    std::uint64_t compiles{},links{},compile_ns{},link_ns{};
    std::uint64_t buffer_uploads{},buffer_upload_bytes{},direct_vertex_bindings{},direct_index_draws{},converted_vertices{};
};

// HLSLcc 的 SM5.1 資源索引正規化：保留 range ID／space 的契約，
// 再交給既有 SM4／5 的型別分析及 Metal 產碼。原始客體 bytecode 不變。
#include <map>
#include <stdexcept>
#include <utility>

struct X64armSm51Resources;
static thread_local X64armSm51Resources* x64armSm51Resources = NULL;

struct X64armSm51Resources {
    struct Range { uint32_t first, count, slot, space; };
    std::map<std::pair<unsigned, uint32_t>, Range> ranges;
    X64armSm51Resources* previous;
    bool enabled, declaration;
    uint32_t declarationSize, declarationSpace;

    explicit X64armSm51Resources(ShaderInfo& info, uint32_t version)
        : previous(x64armSm51Resources), enabled(((version >> 4) & 15U) == 5 && (version & 15U) == 1),
          declaration(false), declarationSize(0), declarationSpace(0) {
        if (!enabled) { x64armSm51Resources = NULL; return; }
        const unsigned limits[RGROUP_COUNT] = {15, 128, 16, 8};
        unsigned next[RGROUP_COUNT] = {};
        for (unsigned group = 0; group < RGROUP_COUNT; ++group) info.aui32ResourceMap[group].data.clear();
        std::vector<bool> matched(info.psConstantBuffers.size(), false);
        for (auto& binding : info.psResourceBindings) {
            const auto group = ShaderInfo::ResourceTypeToResourceGroup(binding.eType);
            if (group >= RGROUP_COUNT || !binding.ui32BindCount || binding.ui32BindCount == 0xffffffffU ||
                binding.ui32BindCount > limits[group] - next[group])
                throw std::invalid_argument("SM5.1 離散資源範圍超出 Metal 槽位；無界陣列須使用描述元表");
            if (group == RGROUP_CBUFFER && binding.ui32BindCount != 1)
                throw std::invalid_argument("SM5.1 幾何路徑尚不支援常數緩衝描述元陣列");
            const Range range = {binding.ui32BindPoint, binding.ui32BindCount, next[group], binding.ui32Space};
            if (!ranges.emplace(std::make_pair(group, binding.ui32RangeID), range).second)
                throw std::invalid_argument("SM5.1 同類資源的 range ID 重複");
            binding.ui32SourceBindPoint = binding.ui32BindPoint;
            binding.ui32BindPoint = next[group];
            next[group] += binding.ui32BindCount;
            // RDEF 的常數／結構化緩衝型別依名稱對應，不能沿用不同 space 的同號暫存器映射。
            for (unsigned i = 0; i < info.psConstantBuffers.size(); ++i) {
                if (!matched[i] && info.psConstantBuffers[i].name == binding.name) {
                    for (unsigned j = 0; j < binding.ui32BindCount; ++j)
                        info.aui32ResourceMap[group][binding.ui32BindPoint+j] = i;
                    matched[i] = true;
                    break;
                }
            }
        }
        x64armSm51Resources = this;
    }
    ~X64armSm51Resources() { x64armSm51Resources = previous; }

    static void immediate(Operand& operand, unsigned index, uint32_t value) {
        operand.aui32ArraySizes[index] = value;
        operand.eIndexRep[index] = OPERAND_INDEX_IMMEDIATE32;
        operand.m_SubOperands[index].reset();
    }
    void operand(Operand& value) const {
        unsigned group;
        switch (value.eType) {
            case OPERAND_TYPE_CONSTANT_BUFFER: group=RGROUP_CBUFFER; break;
            case OPERAND_TYPE_RESOURCE: group=RGROUP_TEXTURE; break;
            case OPERAND_TYPE_SAMPLER: group=RGROUP_SAMPLER; break;
            case OPERAND_TYPE_UNORDERED_ACCESS_VIEW: group=RGROUP_UAV; break;
            default: return;
        }
        const bool constant = group == RGROUP_CBUFFER;
        const unsigned dimensions = declaration || constant ? 3U : 2U;
        if (value.iIndexDims != static_cast<int>(dimensions) || value.eIndexRep[0] != OPERAND_INDEX_IMMEDIATE32)
            throw std::invalid_argument("SM5.1 資源 range ID 或索引維度不合法");
        const auto found = ranges.find(std::make_pair(group, value.aui32ArraySizes[0]));
        if (found == ranges.end()) throw std::invalid_argument("SM5.1 資源 range ID 沒有宣告");
        const auto& range = found->second;
        if (value.eIndexRep[1] != OPERAND_INDEX_IMMEDIATE32)
            throw std::invalid_argument("SM5.1 幾何路徑的動態描述元索引尚未支援");
        const auto source = value.aui32ArraySizes[1];
        if (source < range.first || source - range.first >= range.count)
            throw std::invalid_argument("SM5.1 資源索引超出宣告範圍");
        const auto slot = range.slot + source - range.first;
        if (declaration) {
            if (value.eIndexRep[2] != OPERAND_INDEX_IMMEDIATE32 || source != range.first ||
                uint64_t(value.aui32ArraySizes[2]) + 1 != uint64_t(range.first) + range.count ||
                declarationSpace != range.space)
                throw std::invalid_argument("SM5.1 資源宣告與反射的範圍／space 不符");
            immediate(value, 0, slot);
            if (constant) {
                if (!declarationSize || declarationSize > 4096)
                    throw std::invalid_argument("SM5.1 常數緩衝長度不合法");
                immediate(value, 1, declarationSize);
                value.iIndexDims = 2;
                value.ui32RegisterNumber = declarationSize;
            } else {
                value.iIndexDims = 1;
                value.ui32RegisterNumber = slot;
            }
            immediate(value, 2, 0);
        } else if (constant) {
            // cb[range ID][絕對 buffer 索引][vec4 位移] → cb[槽位][vec4 位移]。
            // vec4 的相對索引與完整 operand 樹一併保留。
            value.aui32ArraySizes[1] = value.aui32ArraySizes[2];
            value.eIndexRep[1] = value.eIndexRep[2];
            value.m_SubOperands[1] = value.m_SubOperands[2];
            immediate(value, 0, slot);
            immediate(value, 2, 0);
            value.iIndexDims = 2;
            value.ui32RegisterNumber = value.aui32ArraySizes[1];
        } else {
            immediate(value, 0, slot);
            immediate(value, 1, 0);
            value.iIndexDims = 1;
            value.ui32RegisterNumber = slot;
        }
    }
};

struct X64armSm51Declaration {
    X64armSm51Resources* context;
    bool previous;
    explicit X64armSm51Declaration(const uint32_t* words, uint32_t length)
        : context(x64armSm51Resources), previous(context && context->declaration) {
        if (!context) return;
        context->declaration = true;
        context->declarationSpace = length > 1 ? words[length-1] : 0;
        context->declarationSize = length > 2 ? words[length-2] : 0;
    }
    ~X64armSm51Declaration() { if (context) context->declaration = previous; }
};

static void x64armSm51ExpandDeclarations(Shader* shader) {
    if (!x64armSm51Resources) return;
    for (auto& phase : shader->asPhases) {
        std::vector<Declaration> declarations;
        for (const auto& declaration : phase.psDecl) {
            declarations.push_back(declaration);
            if (!declaration.ui32NumOperands || declaration.eOpcode == OPCODE_CUSTOMDATA) continue;
            const auto& operand = declaration.asOperands[0];
            ResourceGroup group;
            if (operand.eType == OPERAND_TYPE_RESOURCE) group=RGROUP_TEXTURE;
            else if (operand.eType == OPERAND_TYPE_SAMPLER) group=RGROUP_SAMPLER;
            else if (operand.eType == OPERAND_TYPE_UNORDERED_ACCESS_VIEW) group=RGROUP_UAV;
            else continue;
            const ResourceBinding* binding = NULL;
            if (!shader->sInfo.GetResourceFromBindingPoint(group, operand.ui32RegisterNumber, &binding))
                throw std::invalid_argument("SM5.1 展開資源宣告時找不到綁定");
            for (uint32_t i = 1; i < binding->ui32BindCount; ++i) {
                auto copy = declaration;
                copy.asOperands[0].ui32RegisterNumber += i;
                copy.asOperands[0].aui32ArraySizes[0] += i;
                declarations.push_back(copy);
            }
        }
        phase.psDecl.swap(declarations);
    }
}

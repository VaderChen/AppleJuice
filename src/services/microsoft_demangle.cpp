#include "microsoft_demangle.hpp"
#include "llvm/Demangle/MicrosoftDemangle.h"
#include "llvm/Demangle/MicrosoftDemangleNodes.h"
#include <algorithm>
#include <array>
#include <cctype>

namespace x64arm {
namespace {
using namespace llvm::ms_demangle;
constexpr std::size_t expansion_limit = 1024 * 1024;
// 先走訪展開後的 AST，包含 backreference 的每次使用，限制遞迴與輸出規模。
// 不能只限制輸入長度，因為短編碼也能重複展開先前的模板參數。
bool bounded_tree(const Node* node, std::size_t& budget, bool no_throw, unsigned depth = 0) {
    if (!node) return true;
    if (depth > 128 || budget < 256) return false;
    budget -= 256;
    const auto visit = [&](const Node* child) { return bounded_tree(child, budget, no_throw, depth + 1); };
    if (IdentifierNode::classof(node) && !visit(static_cast<const IdentifierNode*>(node)->TemplateParams)) return false;
    if (SymbolNode::classof(node) && !visit(static_cast<const SymbolNode*>(node)->Name)) return false;
    if (FunctionSignatureNode::classof(node)) {
        const auto* signature = static_cast<const FunctionSignatureNode*>(node);
        // AST 屬此呼叫，可連同巢狀函式指標一起套用 NO_THROW_SIGNATURES。
        if (no_throw) const_cast<FunctionSignatureNode*>(signature)->IsNoexcept = false;
        return visit(signature->ReturnType) && visit(signature->Params);
    }
    const auto text = [&](std::string_view value) {
        if (value.size() > budget) return false;
        budget -= value.size(); return true;
    };
    switch (node->kind()) {
    case NodeKind::NamedIdentifier: return text(static_cast<const NamedIdentifierNode*>(node)->Name);
    case NodeKind::LiteralOperatorIdentifier: return text(static_cast<const LiteralOperatorIdentifierNode*>(node)->Name);
    case NodeKind::EncodedStringLiteral: return text(static_cast<const EncodedStringLiteralNode*>(node)->DecodedString);
    case NodeKind::DynamicStructorIdentifier: {
        const auto* n = static_cast<const DynamicStructorIdentifierNode*>(node); return visit(n->Variable) && visit(n->Name);
    }
    case NodeKind::ConversionOperatorIdentifier: return visit(static_cast<const ConversionOperatorIdentifierNode*>(node)->TargetType);
    case NodeKind::StructorIdentifier: return visit(static_cast<const StructorIdentifierNode*>(node)->Class);
    case NodeKind::PointerType: {
        const auto* n = static_cast<const PointerTypeNode*>(node); return visit(n->ClassParent) && visit(n->Pointee) && visit(n->PointerAuthQualifier);
    }
    case NodeKind::TagType: return visit(static_cast<const TagTypeNode*>(node)->QualifiedName);
    case NodeKind::ArrayType: {
        const auto* n = static_cast<const ArrayTypeNode*>(node); return visit(n->Dimensions) && visit(n->ElementType);
    }
    case NodeKind::Custom: return visit(static_cast<const CustomTypeNode*>(node)->Identifier);
    case NodeKind::FunctionSymbol: return visit(static_cast<const FunctionSymbolNode*>(node)->Signature);
    case NodeKind::VariableSymbol: return visit(static_cast<const VariableSymbolNode*>(node)->Type);
    case NodeKind::SpecialTableSymbol: return visit(static_cast<const SpecialTableSymbolNode*>(node)->TargetName);
    case NodeKind::QualifiedName: return visit(static_cast<const QualifiedNameNode*>(node)->Components);
    case NodeKind::TemplateParameterReference: return visit(static_cast<const TemplateParameterReferenceNode*>(node)->Symbol);
    case NodeKind::NodeArray: {
        const auto* n = static_cast<const NodeArrayNode*>(node);
        for (std::size_t i = 0; i < n->Count; ++i) if (!visit(n->Nodes[i])) return false;
        return true;
    }
    default: return true;
    }
}
std::string keywords(std::string_view source, std::uint16_t flags) {
    static constexpr std::array words{"__cdecl", "__stdcall", "__thiscall", "__fastcall", "__clrcall",
        "__vectorcall", "__regcall", "__eabi", "__pascal", "__ptr64", "__unaligned", "__restrict", "__near", "__far", "__huge"};
    const auto identifier = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    std::string result;
    for (std::size_t i = 0; i < source.size();) {
        const auto start = i;
        if (!identifier(source[i])) { result += source[i++]; continue; }
        while (i < source.size() && identifier(source[i])) ++i;
        auto word = source.substr(start, i - start);
        const bool microsoft = std::find(words.begin(), words.end(), word) != words.end();
        if (microsoft && (flags & 2U)) { if (i < source.size() && source[i] == ' ') ++i; continue; }
        if (microsoft && (flags & 1U)) word.remove_prefix(2);
        result += word;
    }
    return result;
}
}
std::optional<std::string> demangle_microsoft_name(std::string_view source, std::uint16_t flags, bool type_only) {
    using namespace llvm::ms_demangle;
    if (source.size() > microsoft_name_limit) return std::nullopt;
    if ((flags & 0x4000U) && source.starts_with("??_") && !source.starts_with("??_U") && !source.starts_with("??_V")) return std::string(source);
    std::string decorated;
    if (type_only && !source.starts_with('.')) { decorated = '.'; decorated += source; source = decorated; }
    auto rest = source; Demangler demangler; auto* symbol = demangler.parse(rest);
    if (demangler.Error || !symbol || !rest.empty()) {
        if (type_only && !decorated.empty()) source.remove_prefix(1);
        return std::string(source);
    }
    std::size_t budget = expansion_limit;
    if (!bounded_tree(symbol, budget, (flags & 0x100U) != 0)) return std::nullopt;
    unsigned options = OF_Default;
    if (flags & 0x10U) options |= OF_NoCallingConvention;
    if (flags & 0x80U) options |= OF_NoAccessSpecifier;
    if (flags & 0x200U) options |= OF_NoMemberType;
    if (flags & 0x8000U) options |= OF_NoTagSpecifier;
    if (FunctionSymbolNode::classof(symbol)) {
        auto* signature = static_cast<FunctionSymbolNode*>(symbol)->Signature;
        if (flags & 4U) signature->ReturnType = nullptr;
        if (flags & 0x40U) {
            signature->Quals = static_cast<Qualifiers>(signature->Quals & ~(Q_Const | Q_Volatile));
            signature->RefQualifier = FunctionRefQualifier::None;
        }
        if (flags & 0x20U) signature->Quals = static_cast<Qualifiers>(signature->Quals & ~(Q_Restrict | Q_Unaligned | Q_Pointer64 | Q_Far | Q_Huge));
        if (flags & 0x100U) signature->IsNoexcept = false;
        if (!type_only && (flags & 0x2000U)) signature->FunctionClass = static_cast<FuncClass>(signature->FunctionClass | FC_NoParameterList);
    }
    std::string output;
    if (type_only && VariableSymbolNode::classof(symbol) && static_cast<VariableSymbolNode*>(symbol)->Type)
        output = static_cast<VariableSymbolNode*>(symbol)->Type->toString(static_cast<OutputFlags>(options));
    else if ((flags & 0x1000U) && symbol->Name)
        output = symbol->Name->toString(static_cast<OutputFlags>(options | OF_NoTagSpecifier));
    else output = symbol->toString(static_cast<OutputFlags>(options));
    if (output.size() > expansion_limit) return std::nullopt;
    return keywords(output, flags);
}
}

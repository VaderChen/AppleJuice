#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace x64arm::typelib {
using Guid = std::array<std::uint8_t,16>;
constexpr std::uint32_t invalid_data=0x80028018U, unsupported_format=0x80028019U;
struct Error : std::runtime_error {
    std::uint32_t code;
    Error(std::uint32_t status,const char* detail):std::runtime_error(detail),code(status){}
};
// 保留磁碟的 ANSI bytes；載入端依 Windows 碼頁轉成 BSTR，而非以宿主 wchar_t 解讀。
struct Constant {
    std::uint16_t vartype{};
    std::uint64_t bits{};
    std::optional<std::string> text;
    bool operator==(const Constant&)const=default;
};
struct CustomData { Guid guid{}; Constant value; bool operator==(const CustomData&)const=default; };
struct Bound { std::uint32_t count{}; std::int32_t lower{}; bool operator==(const Bound&)const=default; };
struct Type {
    std::uint16_t vartype{};
    std::shared_ptr<const Type> element;
    std::uint32_t reference{};
    std::vector<Bound> bounds;
    bool operator==(const Type& other)const{return vartype==other.vartype && reference==other.reference && bounds==other.bounds && (element && other.element?*element==*other.element:!element && !other.element);}
};
struct Documentation {
    std::string name,help;
    std::uint32_t context{},string_context{};
    bool operator==(const Documentation&)const=default;
};
struct Parameter {
    std::string name;
    Type type;
    std::uint16_t flags{};
    std::optional<Constant> default_value;
    std::vector<CustomData> custom;
    bool operator==(const Parameter&)const=default;
};
struct Function {
    std::int32_t id{};
    Documentation doc;
    Type result;
    std::uint16_t flags{},vtable_offset{},kind{},invocation{},calling_convention{};
    std::int16_t optional_parameters{};
    std::optional<std::uint16_t> entry_ordinal;
    std::optional<std::string> entry_name;
    std::vector<Parameter> parameters;
    std::vector<CustomData> custom;
    bool operator==(const Function&)const=default;
};
struct Variable {
    std::int32_t id{};
    Documentation doc;
    Type type;
    std::uint16_t flags{},kind{};
    std::uint32_t instance_offset{};
    std::optional<Constant> value;
    std::vector<CustomData> custom;
    bool operator==(const Variable&)const=default;
};
struct Implementation {
    std::uint32_t reference{},flags{};
    std::vector<CustomData> custom;
    bool operator==(const Implementation&)const=default;
};
struct TypeInfo {
    Guid guid{};
    Documentation doc;
    std::uint32_t reference{},size{};
    std::uint16_t kind{},alignment{},flags{},major{},minor{},vtable_size{};
    std::optional<Type> alias;
    std::string dll;
    std::vector<Function> functions;
    std::vector<Variable> variables;
    std::vector<Implementation> implementations;
    std::vector<CustomData> custom;
    bool operator==(const TypeInfo&)const=default;
};
struct Import {
    Guid guid{};
    std::uint32_t offset{},lcid{};
    std::uint16_t major{},minor{};
    std::string file;
    bool operator==(const Import&)const=default;
};
struct ExternalReference {
    std::uint32_t reference{},import_index{},type_index{};
    std::uint8_t kind{};
    std::optional<Guid> guid;
    bool operator==(const ExternalReference&)const=default;
};
struct Library {
    Guid guid{};
    Documentation doc;
    std::uint32_t lcid{},locale{},dispatch_reference{};
    std::uint16_t system{},major{},minor{},flags{};
    std::string help_file,help_dll;
    std::vector<TypeInfo> types;
    std::vector<Import> imports;
    std::vector<ExternalReference> external_references;
    std::vector<CustomData> custom;
    bool operator==(const Library&)const=default;
};
// 不映射客體記憶體、不執行 DLL。損毀輸入不可產生部分成功的 Library。
Library read(std::span<const std::uint8_t> bytes);
}

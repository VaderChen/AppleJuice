#pragma once
#include <cstdint>
#include <array>
#include <optional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "x64arm/guest_callback.hpp"
namespace x64arm {
namespace typelib {struct Library;}
struct TypeLibraryView {
    std::shared_ptr<const typelib::Library> data;
    std::string path;
    std::uint64_t library{};
    std::int32_t index{-1};
    std::uint32_t references{1};
    std::vector<std::uint64_t> modules{};
    bool operator==(const TypeLibraryView&)const;
};
struct TypeLibraryAllocation {
    std::uint64_t owner{};
    std::uint8_t kind{};
    std::vector<std::uint64_t> strings;
    bool operator==(const TypeLibraryAllocation&)const=default;
};
struct TypeLibraryInvocation {
    struct Parameter {
        std::uint16_t type{},flags{};
        bool indirect{};
        std::uint64_t source{};
        std::uint32_t argument{UINT32_MAX};
        std::optional<std::array<std::uint8_t,16>> interface_id;
        std::uint64_t writeback{};
        std::uint16_t writeback_type{};
        bool operator==(const Parameter&)const=default;
    };
    std::uint64_t owner{},result{},exception{},argument_error{};
    std::uint32_t phase{},index{},status{},cleanup{},lcid{};
    std::uint16_t return_type{},retval_type{};
    std::vector<Parameter> parameters;
    GuestCallback call;
    bool operator==(const TypeLibraryInvocation&)const=default;
};
struct TypeLibraryBridge {
    struct Compiler {
        std::uint64_t owner{};
        std::uint32_t references{1};
        bool operator==(const Compiler&)const=default;
    };
    std::uint64_t library_table{},info_table{},compiler_table{};
    std::map<std::uint64_t,Compiler> compilers;
    std::map<std::uint64_t,TypeLibraryInvocation> invocations;
    std::vector<std::uint64_t> pending_unloads;
    std::map<std::uint64_t,TypeLibraryView> views;
    std::map<std::string,std::uint64_t> loaded_paths;
    std::map<std::uint64_t,TypeLibraryAllocation> allocations;
    bool operator==(const TypeLibraryBridge&)const=default;
};
}

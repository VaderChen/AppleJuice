#pragma once

#include "x64arm/windows_runtime.hpp"
#include <algorithm>
#include <string_view>

namespace x64arm::detail {
enum class MsvcExceptionOperation {
    Default, DefaultClosure, BorrowMessage, IndirectMessage, Message, Copy,
    Assign, What, Destroy, ScalarDelete, VectorDelete, Vtable,
};

struct MsvcExceptionExport {
    std::string_view library, name;
    MsvcExceptionKind type;
    MsvcExceptionOperation operation;
    unsigned parameters, architectures;
    bool thiscall;

    bool supports(bool x86)const{return (architectures&(x86?1U:2U))!=0;}
};

inline constexpr MsvcExceptionExport msvc_exception_exports[]{
#define MSVC_EXCEPTION_EXPORT(library,name,type,op,params,arch,thiscall) \
    {library,name,MsvcExceptionKind::type,MsvcExceptionOperation::op,params,arch,thiscall},
#include "msvc_exception_exports.inc"
#undef MSVC_EXCEPTION_EXPORT
};

inline const MsvcExceptionExport* find_msvc_exception_export(std::string_view library,std::string_view symbol,bool x86){
    if(!library.starts_with("msvcr") || !symbol.starts_with('?'))return nullptr;
    const auto key=std::pair{library,symbol};
    const auto found=std::ranges::lower_bound(msvc_exception_exports,key,{},
        [](const auto& entry){return std::pair{entry.library,entry.name};});
    for(auto entry=found;entry!=std::end(msvc_exception_exports) &&
        entry->library==library && entry->name==symbol;++entry)
        if(entry->supports(x86))return entry;
    return nullptr;
}
}

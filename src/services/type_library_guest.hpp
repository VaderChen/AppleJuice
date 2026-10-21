#pragma once
#include "type_library.hpp"
namespace x64arm::typelib {
// 客體描述配置的指標先以區塊內相對位址表示；BSTR 由共用 SysAllocString 配置器建立。
struct Description {
    std::vector<std::uint8_t> bytes;
    std::vector<std::size_t> pointers;
    std::vector<std::pair<std::size_t,std::u16string>> strings;
};
Description library_description(const Library&);
Description type_description(const Library&,std::size_t index,unsigned unit);
Description function_description(const Function&,unsigned unit);
Description variable_description(const Variable&,unsigned unit);
}

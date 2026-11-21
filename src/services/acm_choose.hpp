#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace x64arm::acm {
struct ChooseAbi {
    unsigned pointer,character;
    unsigned title()const{return 12+2*pointer;}
    unsigned tag_text()const{return title()+pointer;}
    unsigned value_text()const{return tag_text()+48*character;}
    unsigned name()const{return value_text()+128*character;}
    unsigned name_capacity()const{return name()+pointer;}
    unsigned enum_flags()const{return name_capacity()+4;}
    unsigned enum_format()const{return enum_flags()+4;}
    unsigned instance()const{return enum_format()+pointer;}
    unsigned template_name()const{return instance()+pointer;}
    unsigned custom()const{return template_name()+pointer;}
    unsigned hook()const{return custom()+pointer;}
    unsigned size()const{return hook()+pointer;}
};

struct ChoiceDialog {
    struct Option {std::u16string description;std::vector<std::uint8_t> bytes;};
    struct Group {unsigned tag;std::u16string label;std::vector<Option> options;};
    std::vector<Group> groups;
    std::u16string name;
    std::uint64_t window{},owner{},hook{},custom{};
    unsigned group{},option{},style{},name_capacity{};
    bool filter{},confirmed{},initializing{};
};
inline constexpr unsigned name_control=100,tag_control=101,value_control=102,help_control=9;
}

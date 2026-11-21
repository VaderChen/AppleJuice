#pragma once

#include "d3d9_shader.hpp"
#include <bitset>

namespace x64arm::d3d9_validator {

struct Issue {
    std::uint32_t severity{}, id{};
    std::string text;
};

enum class Stage { Idle, AwaitingVersion, Body, Ended, Broken };

struct Shader {
    Stage stage{Stage::Idle};
    std::uint64_t callback{}, context{};
    std::uint32_t version{};
    std::bitset<10> pixel_inputs;
    std::vector<std::uint8_t> bytes;
    bool failed{};

    bool accepts_version(std::uint32_t token) const {
        const auto type=token>>16,major=(token>>8)&0xffU,minor=token&0xffU;
        if(type!=0xfffe && type!=0xffff)return false;
        if(major==1)return minor<=(type==0xfffe?1U:4U);
        return (major==2 || major==3) && minor==0;
    }

    static unsigned register_type(std::uint32_t operand) {
        return ((operand>>28)&7U)|((operand>>8)&24U);
    }

    std::optional<Issue> validate_pixel_inputs(std::span<const std::uint32_t> packet) {
        const auto opcode=packet.front()&0xffffU;
        if(version!=0xffff0300U || packet.size()<3)return std::nullopt;
        if(opcode==31 && register_type(packet[2])==1){
            const auto index=packet[2]&0x7ffU;
            if(index>=pixel_inputs.size())return Issue{2,0x12c,"Pixel shader input register is out of range."};
            pixel_inputs.set(index);
        }else if(opcode>=1 && opcode<=24){
            for(const auto operand:packet.subspan(2)){
                if(register_type(operand)!=1)continue;
                const auto index=operand&0x7ffU;
                if(index>=pixel_inputs.size() || !pixel_inputs.test(index))
                    return Issue{2,0x167,"Pixel shader reads an undeclared input register."};
            }
        }
        return std::nullopt;
    }
};
}

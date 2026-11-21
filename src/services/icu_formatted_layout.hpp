#pragma once

#include <stdint.h>
#include <unicode/uformattedvalue.h>

namespace x64arm::icu_formatted {
enum class Constraint : uint32_t {None, Category, Field};
struct Position {
    static constexpr uint32_t signature=0x55434600;
    uint32_t magic{signature};
    Constraint constraint{};
    int64_t context{};
    int32_t category{},field{},start{},limit{};

    void select(int32_t value){
        constraint=Constraint::Category;
        category=value;
    }
    void select(int32_t value,int32_t member){
        constraint=Constraint::Field;
        category=value;
        field=member;
    }
    bool matches(int32_t group,int32_t member)const{
        return constraint==Constraint::None ||
            (category==group && (constraint==Constraint::Category || field==member));
    }
    void set(int32_t group,int32_t member,int32_t begin,int32_t end){
        category=group;
        field=member;
        start=begin;
        limit=end;
    }
};
static_assert(sizeof(Position)==32);

struct Value {
    static constexpr uint32_t signature=0x55465600;
    uint64_t text{};
    int32_t length{};
    UErrorCode error{U_INVALID_STATE_ERROR};
    uint32_t magic{signature};
    uint32_t reserved{};
};
static_assert(sizeof(Value)==24);
}

#pragma once

#include <unicode/uiter.h>
#include "native_callback_job.hpp"

namespace x64arm::icu_collation_iterator {

enum class Callback : std::uint8_t { Index, Move, HasNext, HasPrevious, Current, Next, Previous };

constexpr std::size_t callback_offset(bool x86,Callback callback){
    const auto size=x86?4U:8U;
    const auto first=x86?24U:32U; // 內容指標、五個 int32，Win64 函式指標前有對齊填補。
    return first+size*static_cast<std::size_t>(callback);
}

static_assert(offsetof(UCharIterator,getIndex)==callback_offset(sizeof(void*)==4,Callback::Index));
static_assert(offsetof(UCharIterator,previous)==callback_offset(sizeof(void*)==4,Callback::Previous));

struct Request {
    std::uint64_t iterator;
    Callback callback;
    std::uint32_t delta,origin;
};

using CallbackJob = NativeCallbackJob<Request, U_SENTINEL>;

class Iterator {
public:
    Iterator(CallbackJob& job,std::uint64_t address):job_(job),address_(address){
        native_.context=this;
        native_.getIndex=[](UCharIterator* it,UCharIteratorOrigin origin){return self(it).call(Callback::Index,0,origin);};
        native_.move=[](UCharIterator* it,std::int32_t delta,UCharIteratorOrigin origin){return self(it).call(Callback::Move,delta,origin);};
        native_.hasNext=[](UCharIterator* it)->UBool{return self(it).call(Callback::HasNext)>0;};
        native_.hasPrevious=[](UCharIterator* it)->UBool{return self(it).call(Callback::HasPrevious)>0;};
        native_.current=[](UCharIterator* it){return self(it).call(Callback::Current);};
        native_.next=[](UCharIterator* it){return self(it).call(Callback::Next);};
        native_.previous=[](UCharIterator* it){return self(it).call(Callback::Previous);};
    }

    Iterator(const Iterator&)=delete;
    Iterator& operator=(const Iterator&)=delete;
    UCharIterator* get(){return &native_;}

private:
    static Iterator& self(UCharIterator* it){return *const_cast<Iterator*>(static_cast<const Iterator*>(it->context));}
    std::int32_t call(Callback callback,std::int32_t delta=0,UCharIteratorOrigin origin=UITER_ZERO){
        return job_.call({address_,callback,static_cast<std::uint32_t>(delta),static_cast<std::uint32_t>(origin)});
    }

    CallbackJob& job_;
    std::uint64_t address_;
    UCharIterator native_{};
};

inline bool valid_reply(Callback callback,std::int32_t value){
    if(callback==Callback::Index || callback==Callback::Move)return value>=UITER_UNKNOWN_INDEX;
    if(callback==Callback::HasNext || callback==Callback::HasPrevious)return true;
    return value>=U_SENTINEL && value<=0xffff;
}

}

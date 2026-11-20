#pragma once
#include "x64arm/windows_services.hpp"
#include "message_metadata_layout.h"

namespace x64arm {
inline GuestAddress gui_message_cell(const GuestThread& thread,bool x86,unsigned offset){
    return GuestAddress{thread.teb+(x86?X64ARM_MESSAGE_X86_BASE:X64ARM_MESSAGE_X64_BASE)+offset};
}
inline void store_gui_message_metadata(GuestMemory& memory,GuestThread& thread,bool x86,const GuiMessage& message){
    thread.message_time=message.queued_time;
    thread.message_position=static_cast<std::uint16_t>(message.queued_cursor[0])|
        (std::uint32_t{static_cast<std::uint16_t>(message.queued_cursor[1])}<<16);
    thread.message_extra=message.input_extra.value_or(0);
    if(!thread.teb)return;
    memory.store(gui_message_cell(thread,x86,X64ARM_MESSAGE_TIME),32,thread.message_time);
    memory.store(gui_message_cell(thread,x86,X64ARM_MESSAGE_POSITION),32,thread.message_position);
    memory.store(gui_message_cell(thread,x86,X64ARM_MESSAGE_EXTRA),x86?32:64,thread.message_extra);
}
}

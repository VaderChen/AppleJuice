// opengl32 選取與回饋模式（由 windows_opengl.cpp 的 invoke_opengl 引入）：glSelectBuffer、glFeedbackBuffer、glRenderMode、
// glInitNames／glLoadName／glPushName／glPopName、glPassThrough。客體緩衝的位址與大小留在服務層，後端在 RenderMode 離開 SELECT／FEEDBACK 時
// 以「字數＋內容」回傳命中紀錄或回饋值，服務層只寫入實際產生的字數，緩衝其餘內容不動。
if(symbol=="glSelectBuffer" || symbol=="glFeedbackBuffer" || symbol=="glRenderMode" || symbol=="glInitNames" || symbol=="glLoadName" || symbol=="glPushName" || symbol=="glPopName" || symbol=="glPassThrough"){
    const auto run=[&](GlOperation operation,std::span<const std::uint64_t> values,std::span<std::uint8_t> out)->std::uint64_t{
        try{return c.native->execute(operation,values,{},out);}catch(const std::bad_alloc&){error(c,out_of_memory);return 0;}};
    if(symbol=="glSelectBuffer"){
        // (size, buffer)：選取模式中不可更換；大小以 32 位元字計。
        if(n(0)<0){error(c,invalid_value);return finish(0);}if(c.render_mode==0x1c02){error(c,invalid_operation);return finish(0);}
        if(d(0)>transfer_limit/4){error(c,out_of_memory);return finish(0);}
        c.select_buffer=arg(1);c.select_size=d(0);c.select_set=true;const std::array<std::uint64_t,1> values{d(0)};run(GlOperation::SelectBuffer,values,{});return finish(0);
    }
    if(symbol=="glFeedbackBuffer"){
        // (size, type, buffer)：type 為 GL_2D 0x0600～GL_4D_COLOR_TEXTURE 0x0604；回饋模式中不可更換。
        if(d(1)<0x600 || d(1)>0x604){error(c,invalid_enum);return finish(0);}if(n(0)<0){error(c,invalid_value);return finish(0);}
        if(c.render_mode==0x1c01){error(c,invalid_operation);return finish(0);}if(d(0)>transfer_limit/4){error(c,out_of_memory);return finish(0);}
        c.feedback_buffer=arg(2);c.feedback_size=d(0);c.feedback_type=d(1);c.feedback_set=true;const std::array<std::uint64_t,2> values{d(0),d(1)};run(GlOperation::FeedbackBuffer,values,{});return finish(0);
    }
    if(symbol=="glRenderMode"){
        const auto mode=d(0);if(mode<0x1c00 || mode>0x1c02){error(c,invalid_enum);return finish(0);}
        if((mode==0x1c02 && !c.select_set) || (mode==0x1c01 && !c.feedback_set)){error(c,invalid_operation);return finish(0);}
        const auto previous=c.render_mode;const std::uint32_t capacity=previous==0x1c02?c.select_size:previous==0x1c01?c.feedback_size:0;
        auto& out=c.output_scratch;out.assign((static_cast<std::size_t>(capacity)+1)*4,0);
        const std::array<std::uint64_t,1> values{mode};const auto result=run(GlOperation::RenderMode,values,out);c.render_mode=mode;
        std::uint32_t words=0;std::memcpy(&words,out.data(),4);words=std::min(words,capacity);
        const auto target=previous==0x1c02?c.select_buffer:c.feedback_buffer;
        if(words && target)memory.write(GuestAddress{target},std::span<const std::uint8_t>(out).subspan(4,static_cast<std::size_t>(words)*4));
        return finish(static_cast<std::uint32_t>(result));
    }
    if(symbol=="glInitNames" || symbol=="glPopName"){run(symbol=="glInitNames"?GlOperation::InitNames:GlOperation::PopName,{},{});return finish(0);}
    if(symbol=="glLoadName" || symbol=="glPushName"){const std::array<std::uint64_t,1> values{d(0)};run(symbol=="glLoadName"?GlOperation::LoadName:GlOperation::PushName,values,{});return finish(0);}
    // glPassThrough(GLfloat)：x64 取自 XMM0，x86 為 4 位元組堆疊值。
    const std::array<std::uint64_t,1> values{static_cast<std::uint32_t>(floating_argument(0,false))};run(GlOperation::PassThrough,values,{});return finish(0);
}

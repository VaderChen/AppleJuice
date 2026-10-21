// 所有分析方法共用分塊讀取、客體回呼、失敗傳播及 COM lifetime。
const auto analysis_next=[&](std::uint64_t token,TextAnalysisStep step,std::uint64_t object,unsigned slot,std::vector<std::uint64_t> arguments={}){
    auto& operation=bridge.analyses.at(token);operation.step=step;
    start_callbacks(state,memory,{font_callback(memory,unit,object,slot,std::move(arguments))},Continuation::DWriteOperation,token);return std::optional<std::uint32_t>{};
};
const auto analysis_complete=[&](std::uint64_t token,std::uint32_t status){
    const auto operation=bridge.analyses.at(token);
    if(operation.number)bridge.deferred.push_back(font_callback(memory,unit,operation.number,2));
    if(operation.sink_retained)bridge.deferred.push_back(font_callback(memory,unit,operation.sink,2));
    if(operation.source_retained)bridge.deferred.push_back(font_callback(memory,unit,operation.source,2));
    bridge.deferred.push_back(font_callback(memory,unit,operation.object,2));
    (void)free_heap(memory,operation.scratch,state);bridge.analyses.erase(token);return finish(status);
};
const auto analysis_read=[&](std::uint64_t token){
    auto& operation=bridge.analyses.at(token);put(operation.scratch,0);memory.store(GuestAddress{operation.scratch+16},32,0);
    const bool number=operation.service.symbol=="TextAnalyzer.AnalyzeNumberSubstitution";
    return analysis_next(token,number?TextAnalysisStep::Number:TextAnalysisStep::Text,operation.source,number?7:3,
                         number?std::vector<std::uint64_t>{operation.start+operation.position,operation.scratch+16,operation.scratch}:std::vector<std::uint64_t>{operation.start+operation.position,operation.scratch,operation.scratch+16});
};
const auto analysis_report=[&](std::uint64_t token){
    auto& operation=bridge.analyses.at(token);const auto analysis_name=operation.service.symbol;const auto payload=operation.scratch+64;
    if(analysis_name=="TextAnalyzer.AnalyzeLineBreakpoints"){
        memory.write(GuestAddress{payload},operation.breaks);return analysis_next(token,TextAnalysisStep::Report,operation.sink,4,{operation.start,operation.length,payload});
    }
    if(operation.report_index>=operation.analysis.items.size())return analysis_complete(token,0);
    const auto& item=operation.analysis.items[operation.report_index];const auto end=operation.report_index+1<operation.analysis.items.size()?operation.analysis.items[operation.report_index+1].start:operation.length;
    if(analysis_name=="TextAnalyzer.AnalyzeScript"){
        memory.store(GuestAddress{payload},16,item.script);memory.store(GuestAddress{payload+2},16,0);memory.store(GuestAddress{payload+4},32,0);
        return analysis_next(token,TextAnalysisStep::Report,operation.sink,3,{operation.start+item.start,end-item.start,payload});
    }
    return analysis_next(token,TextAnalysisStep::Report,operation.sink,5,{operation.start+item.start,end-item.start,operation.analysis.paragraph,item.level});
};
if(resuming && bridge.analyses.contains(resuming)){
    try{
        auto& operation=bridge.analyses.at(resuming);const auto status=static_cast<std::uint32_t>(state.reg(Gpr::Rax));const auto analysis_name=operation.service.symbol;
        if(operation.step!=TextAnalysisStep::RetainSource && operation.step!=TextAnalysisStep::RetainSink && operation.step!=TextAnalysisStep::Direction && operation.step!=TextAnalysisStep::NumberRelease && (status&0x80000000U))return analysis_complete(resuming,status);
        if(operation.step==TextAnalysisStep::RetainSource){operation.source_retained=true;return analysis_next(resuming,TextAnalysisStep::RetainSink,operation.sink,1);}
        if(operation.step==TextAnalysisStep::RetainSink){operation.sink_retained=true;return analysis_read(resuming);}
        if(operation.step==TextAnalysisStep::Text){
            const auto pointer=memory.load(GuestAddress{operation.scratch},bits),available=memory.load(GuestAddress{operation.scratch+16},32);
            if(!pointer || !available)return analysis_complete(resuming,invalid);
            const auto count=static_cast<unsigned>(std::min<std::uint64_t>(available,operation.length-operation.position));
            for(unsigned i=0;i<count;++i)operation.text.push_back(static_cast<char16_t>(memory.load(GuestAddress{pointer+i*2},16)));
            operation.position+=count;if(operation.position<operation.length)return analysis_read(resuming);
            return analysis_next(resuming,TextAnalysisStep::Direction,operation.source,5);
        }
        if(operation.step==TextAnalysisStep::Direction){
            if(status>1)return analysis_complete(resuming,0x80004001U);operation.direction=status;
            if(analysis_name=="TextAnalyzer.AnalyzeBidi" && std::any_of(operation.text.begin(),operation.text.end(),[](char16_t c){return (c>=0x202a && c<=0x202e) || (c>=0x2066 && c<=0x2069);}))return analysis_complete(resuming,0x80004001U);
            operation.analysis=text_analysis::analyze(operation.text,operation.direction,true);
            if(analysis_name=="TextAnalyzer.AnalyzeLineBreakpoints")return analysis_next(resuming,TextAnalysisStep::Locale,operation.source,6,{operation.start,operation.scratch+16,operation.scratch});
            return analysis_report(resuming);
        }
        if(operation.step==TextAnalysisStep::Locale){
            const auto pointer=memory.load(GuestAddress{operation.scratch},bits);operation.locale=pointer?read_utf16(memory,GuestAddress{pointer},85):u"";
            const auto locale=utf16_to_utf8(operation.locale);operation.breaks=text_analysis::line_breaks(operation.text,locale.empty()?nullptr:locale.c_str());return analysis_report(resuming);
        }
        if(operation.step==TextAnalysisStep::Number){
            const auto count=memory.load(GuestAddress{operation.scratch+16},32);operation.number=memory.load(GuestAddress{operation.scratch},bits);
            if(!count)return analysis_complete(resuming,invalid);
            operation.number_length=static_cast<unsigned>(std::min<std::uint64_t>(count,operation.length-operation.position));
            return analysis_next(resuming,TextAnalysisStep::Report,operation.sink,6,{operation.start+operation.position,operation.number_length,operation.number});
        }
        if(operation.step==TextAnalysisStep::Report){
            if(analysis_name=="TextAnalyzer.AnalyzeNumberSubstitution"){
                operation.position+=operation.number_length;
                if(operation.number)return analysis_next(resuming,TextAnalysisStep::NumberRelease,operation.number,2);
                return operation.position==operation.length?analysis_complete(resuming,0):analysis_read(resuming);
            }
            if(analysis_name=="TextAnalyzer.AnalyzeLineBreakpoints")return analysis_complete(resuming,0);
            ++operation.report_index;return analysis_report(resuming);
        }
        if(operation.step==TextAnalysisStep::NumberRelease){operation.number=0;return operation.position==operation.length?analysis_complete(resuming,0):analysis_read(resuming);}
        throw HostCallFault("DirectWrite 文字分析回呼階段無效");
    }catch(const MemoryFault&){return analysis_complete(resuming,pointer_error);}
     catch(const std::bad_alloc&){return analysis_complete(resuming,0x8007000eU);}
     catch(const std::invalid_argument&){return analysis_complete(resuming,invalid);}
}

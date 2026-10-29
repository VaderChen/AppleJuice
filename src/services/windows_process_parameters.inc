if(service.family=="ntdll.dll" && (symbol=="RtlCreateProcessParameters" || symbol=="RtlCreateProcessParametersEx" || symbol=="RtlCreateProcessParametersWithTemplate" ||
   symbol=="RtlDestroyProcessParameters" || symbol=="RtlNormalizeProcessParams" || symbol=="RtlDeNormalizeProcessParams")) {
    const auto saved_error=last_error_;
    const auto finish=[&](std::uint64_t value)->std::optional<std::uint32_t>{last_error_=saved_error;arg.return_to(state,value);return std::nullopt;};
    const auto layout=detail::ProcessParameterLayout::for_x86(x86_);const auto unit=layout.unit;const auto bits=static_cast<std::uint8_t>(unit*8);
    if(symbol=="RtlDestroyProcessParameters") {
        if(arg(0))(void)free_heap(memory,arg(0),state);
        return finish(0);
    }
    if(symbol=="RtlNormalizeProcessParams" || symbol=="RtlDeNormalizeProcessParams") {
        const auto base=arg(0);if(!base)return finish(0);
        detail::normalize_process_parameter_pointers(memory,base,x86_,symbol=="RtlNormalizeProcessParams");
        return finish(base);
    }
    std::uint64_t allocation=0;
    try {
        const auto output=arg(0);memory.check(GuestAddress{output},unit,Access::Write);
        const bool with_template=symbol.ends_with("WithTemplate");
        const auto flags=with_template?static_cast<std::uint32_t>(arg(11)):symbol.ends_with("Ex")?static_cast<std::uint32_t>(arg(10)):0U;
        if(flags&~1U)return finish(0xc000000dU);
        if(!arg(1))return finish(0xc000000dU);
        struct ParameterString {std::vector<std::uint8_t> bytes;std::uint32_t capacity{};};
        const auto decode=[&](std::uint64_t address,bool binary=false){
            ParameterString result;
            memory.check(GuestAddress{address},unit*2,Access::Read);
            const auto length=memory.load(GuestAddress{address},16),maximum=memory.load(GuestAddress{address+2},16);
            const auto pointer=memory.load(GuestAddress{address+unit},bits);
            if(length>maximum || (!binary && length%2) || (length && !pointer))throw std::invalid_argument("程序參數字串長度或指標無效");
            result.bytes.resize(static_cast<std::size_t>(length));if(length)memory.read(GuestAddress{pointer},result.bytes);
            result.capacity=static_cast<std::uint32_t>(maximum);return result;
        };
        const auto from_text=[&](std::u16string_view text){ParameterString value;value.bytes=utf16_bytes(text);value.capacity=static_cast<std::uint32_t>(value.bytes.size());value.bytes.resize(value.bytes.size()-2);return value;};
        std::array<ParameterString,9> strings;
        strings[0]=arg(3)?decode(arg(3)):from_text(runtime_.current_directory);
        if(!strings[0].bytes.empty() && (strings[0].bytes[strings[0].bytes.size()-2]!='\\' || strings[0].bytes.back())){
            strings[0].bytes.push_back('\\');strings[0].bytes.push_back(0);
        }
        strings[0].capacity=static_cast<std::uint32_t>(std::max<std::size_t>(520,strings[0].bytes.size()+2));
        if(arg(2))strings[1]=decode(arg(2));
        strings[2]=decode(arg(1));strings[2].capacity=static_cast<std::uint32_t>(strings[2].bytes.size()+2);
        strings[3]=arg(4)?decode(arg(4)):strings[2];strings[3].capacity=static_cast<std::uint32_t>(strings[3].bytes.size()+2);
        for(unsigned i=4;i<8;++i)strings[i]=arg(i+2)?decode(arg(i+2),i==7):i==7?ParameterString{}:from_text(u"");
        if(with_template && arg(10))strings[8]=decode(arg(10));
        std::vector<std::uint8_t> environment;
        if(const auto address=arg(5)) {
            bool previous_zero=false;
            for(std::uint64_t offset=0;;offset+=2) {
                if(offset>=output_limit-2)throw std::length_error("程序環境區塊過長");
                const auto value=memory.load(GuestAddress{address+offset},16);environment.push_back(static_cast<std::uint8_t>(value));environment.push_back(static_cast<std::uint8_t>(value>>8));
                if(!value && (!offset || previous_zero))break;
                previous_zero=!value;
            }
        } else {
            for(const auto& [key,entry]:runtime_.environment){
                (void)key;const auto value=utf16_bytes(entry.first+u"="+entry.second);
                if(value.size()>output_limit-environment.size())throw std::length_error("程序環境區塊過長");
                environment.insert(environment.end(),value.begin(),value.end());
            }
            environment.push_back(0);environment.push_back(0);
        }
        const auto align=[&](std::size_t size){return (size+unit-1)&~std::size_t{unit-1};};
        std::vector<std::uint8_t> bytes(align(layout.header));std::array<std::size_t,9> positions{};
        for(unsigned i=0;i<strings.size();++i) {
            if(strings[i].capacity>65535)return finish(0xc0000106U);
            if(!strings[i].capacity)continue;
            positions[i]=bytes.size();bytes.resize(bytes.size()+align(strings[i].capacity));
            std::copy(strings[i].bytes.begin(),strings[i].bytes.end(),bytes.begin()+static_cast<std::ptrdiff_t>(positions[i]));
        }
        const auto parameter_size=bytes.size(),environment_size=align(environment.size());
        bytes.resize(parameter_size+environment_size);std::copy(environment.begin(),environment.end(),bytes.begin()+static_cast<std::ptrdiff_t>(parameter_size));
        allocation=allocate_heap(memory,bytes.size());if(!allocation)return finish(0xc0000017U);
        const auto put=[&](std::size_t offset,std::uint64_t value,unsigned size){for(unsigned i=0;i<size;++i)bytes[offset+i]=static_cast<std::uint8_t>(value>>(i*8));};
        put(0,parameter_size,4);put(4,parameter_size,4);put(8,flags,4);
        const auto current=memory.load(GuestAddress{peb_base_.value+(x86_?0x10U:0x20U)},bits);
        if(current)put(layout.console_flags,memory.load(GuestAddress{current+layout.console_flags},32),4);
        for(unsigned i=0;i<strings.size();++i) {
            put(layout.strings[i],strings[i].bytes.size(),2);put(layout.strings[i]+2,strings[i].capacity,2);
            if(positions[i])put(layout.strings[i]+unit,positions[i]+((flags&1U)?allocation:0),unit);
        }
        // Environment 始終是絕對指標，容量涵蓋配置器實際交付的尾端空間；字串才隨正規化變更。
        put(layout.environment,allocation+parameter_size,unit);put(layout.environment_size,allocations_.at(allocation)-parameter_size,unit);
        memory.write(GuestAddress{allocation},bytes);memory.store(GuestAddress{output},bits,allocation);return finish(0);
    } catch(const MemoryFault&){if(allocation)(void)free_heap(memory,allocation,state);return finish(0xc0000005U);}
      catch(const std::bad_alloc&){if(allocation)(void)free_heap(memory,allocation,state);return finish(0xc0000017U);}
      catch(const std::invalid_argument&){if(allocation)(void)free_heap(memory,allocation,state);return finish(0xc000000dU);}
      catch(const std::length_error&){if(allocation)(void)free_heap(memory,allocation,state);return finish(0xc0000106U);}
}

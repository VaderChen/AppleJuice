    if(name=="AVISaveOptionsFree"){
        const WindowsCall args(state,memory,service);
        const auto count=static_cast<std::int32_t>(args(0));
        if(count<0 || !args(1)){args.return_to(state,bad_parameter);return std::nullopt;}
        const unsigned unit=x86_?4:8;
        const auto bits=static_cast<std::uint8_t>(unit*8);
        for(int i=count;i>0;--i){
            const auto opt=memory.load(GuestAddress{args(1)+static_cast<unsigned>(i-1)*unit},bits);
            if(!opt)continue;
            auto flags=GuestAddress{opt+20};
            memory.store(flags,32,memory.load(flags,32)&~8ULL);
            for(unsigned offset:{x86_?32U:40U,24U}){
                const auto field=GuestAddress{opt+offset};
                if(const auto address=memory.load(field,bits)){
                    free_heap(memory,address,state);
                    memory.store(field,bits,0);memory.store(GuestAddress{field.value+unit},32,0);
                }
            }
        }
        args.return_to(state,0);return std::nullopt;
    }
    if(service.family=="internal-avifil32-options"){
        const WindowsCall args(state,memory,service);
        const auto hwnd=args(0);const auto message=static_cast<unsigned>(args(1));
        const auto finish=[&](unsigned value){args.return_to(state,value);return std::optional<std::uint32_t>{};};
        auto found=gui_.windows.find(hwnd);if(found==gui_.windows.end())return finish(0);
        auto& window=found->second;
        if(message==0x110){
            window.user_data=args(3);
            for(unsigned id:{106U,107U})if(const auto child=gui_child(hwnd,id)){
                gui_.windows.at(child).check=static_cast<unsigned>(memory.load(GuestAddress{args(3)+(id==106?20U:24U)},32));
                present_gui(child);
            }
            return finish(1);
        }
        if(message!=0x111 && message!=0x10)return finish(0);
        const auto command=message==0x10?2U:static_cast<unsigned>(args(2))&65535U;
        if(command!=1 && command!=2)return finish(0);
        if(command==1){
            const auto data=window.user_data;
            if(!data)return finish(0);
            auto text=[&](unsigned id){const auto child=gui_child(hwnd,id);return child?gui_.windows.at(child).title:std::u16string{};};
            auto number=[&](unsigned id,unsigned fallback,unsigned max)->std::optional<unsigned>{
                if(!gui_child(hwnd,id))return fallback;
                auto str=text(id);if(str.empty())return fallback;
                std::uint64_t value=0;for(const auto c:str){
                    if(c<u'0' || c>u'9')return {};
                    value=value*10+static_cast<unsigned>(c-u'0');if(value>max)return {};
                }
                return static_cast<unsigned>(value);
            };
            const auto codec=text(101);
            if(!codec.empty() && codec.size()!=4)return finish(1);
            unsigned handler=0;
            for(unsigned i=0;i<codec.size();++i){
                if(codec[i]<32 || codec[i]>126)return finish(1);
                handler|=static_cast<unsigned>(codec[i])<<(8*i);
            }
            const auto quality=number(102,UINT32_MAX,10000),key=number(103,0,INT32_MAX),rate=number(104,0,INT32_MAX),
                interval=number(105,0,INT32_MAX);
            if(!quality || !key || !rate || !interval)return finish(1);
            const std::array values{handler,*quality,*key,*rate,*interval};
            for(unsigned i=0;i<values.size();++i)memory.store(GuestAddress{data+i*4},32,values[i]);
            const auto checked=gui_child(hwnd,106);
            memory.store(GuestAddress{data+20},32,checked && gui_.windows.at(checked).check);
            const auto select=gui_child(hwnd,107);
            const bool enabled=select && gui_.windows.at(select).check;
            if(enabled && gui_child(hwnd,108)){
                auto hz=number(108,0,384000),channels=number(109,0,2),depth=number(110,0,32);
                if(!hz || *hz<1000 || !channels || !*channels || !depth ||
                    (*depth!=8 && *depth!=16 && *depth!=24 && *depth!=32))return finish(1);
                memory.store(GuestAddress{data+28},32,*hz);memory.store(GuestAddress{data+32},32,*channels);
                memory.store(GuestAddress{data+36},32,*depth);
            }
            memory.store(GuestAddress{data+24},32,enabled);
        }
        if(window.modal){window.ended=true;window.dialog_result=command==1;}
        return finish(1);
    }

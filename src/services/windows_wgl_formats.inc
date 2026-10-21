    if(symbol=="wglGetExtensionsStringARB"){
        if(!dc_window(arg(0))){last_error_=6;return finish(0);}
        if(!opengl_.extensions_string){
            // create_context 的跨 context 共享尚未提供，因此不宣告完整 extension。
            constexpr std::string_view value="WGL_ARB_extensions_string WGL_ARB_pixel_format WGL_EXT_swap_control";
            std::vector<std::uint8_t> bytes(value.begin(),value.end());bytes.push_back(0);
            opengl_.extensions_string=runtime_allocate(memory,bytes);
        }return finish(opengl_.extensions_string);
    }
    if(symbol=="wglGetPixelFormatAttribivARB" || symbol=="wglGetPixelFormatAttribfvARB"){
        if(!dc_window(arg(0))){last_error_=6;return finish(0);}
        if(d(3)>256 || (d(3) && (!arg(4) || !arg(5)))){last_error_=87;return finish(0);}
        const auto attributes=read_bytes(memory,arg(4),static_cast<std::size_t>(d(3))*4);
        std::vector<std::uint8_t> values(attributes.size());
        for(unsigned i=0;i<d(3);++i){
            unsigned name;std::memcpy(&name,attributes.data()+i*4,4);const auto attribute=wgl_format_attribute(name);
            if(!attribute){last_error_=87;return finish(0);}
            if(name!=0x2000 && (d(1)!=1 || (!attribute->main_plane_only && n(2)!=0))){last_error_=2000;return finish(0);}
            if(symbol=="wglGetPixelFormatAttribfvARB"){const auto value=static_cast<float>(attribute->value);std::memcpy(values.data()+i*4,&value,4);}
            else std::memcpy(values.data()+i*4,&attribute->value,4);
        }
        if(!values.empty())memory.write(GuestAddress{arg(5)},values);return finish(1);
    }
    if(symbol=="wglChoosePixelFormatARB"){
        if(!dc_window(arg(0))){last_error_=6;return finish(0);}
        if(!arg(5) || (d(3) && !arg(4))){last_error_=87;return finish(0);}
        bool matches=true;
        // 整數及浮點清單共用條件比對，重複條件依最後出現的值決定。
        std::map<unsigned,double> requested;
        for(unsigned list=1;list<=2;++list){
            if(!arg(list))continue;bool terminated=false;
            for(unsigned pair=0;pair<256;++pair){
                const auto at=arg(list)+static_cast<std::uint64_t>(pair)*8;
                const auto token=static_cast<std::uint32_t>(memory.load(GuestAddress{at},32));
                const auto numeric=list==1?static_cast<double>(std::bit_cast<std::int32_t>(token)):static_cast<double>(std::bit_cast<float>(token));
                if(numeric==0){terminated=true;break;}
                if(!std::isfinite(numeric) || numeric<0 || numeric>UINT32_MAX || std::floor(numeric)!=numeric){last_error_=87;return finish(0);}
                const auto name=static_cast<unsigned>(numeric);if(!wgl_format_attribute(name)){last_error_=87;return finish(0);}
                const auto bits=static_cast<std::uint32_t>(memory.load(GuestAddress{at+4},32));
                const auto value=list==1?static_cast<double>(std::bit_cast<std::int32_t>(bits)):static_cast<double>(std::bit_cast<float>(bits));
                if(!std::isfinite(value)){last_error_=87;return finish(0);}requested[name]=value;
            }
            if(!terminated){last_error_=87;return finish(0);}
        }
        for(const auto& [name,value]:requested){const auto attribute=*wgl_format_attribute(name);if(!attribute.ignored && (attribute.minimum?attribute.value<value:attribute.value!=value))matches=false;}
        memory.check(GuestAddress{arg(5)},4,Access::Write);
        const std::uint32_t count=matches && d(3)?1:0;
        if(count)memory.store(GuestAddress{arg(4)},32,1);
        memory.store(GuestAddress{arg(5)},32,count);return finish(1);
    }

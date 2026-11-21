        if(service.family=="internal-acm-choice"){
            if(!acm_){arg.return_to(state,0);return {};}
            const auto window=arg(0);
            const auto message=d(1);
            const auto found=message==0x110?acm_->dialogs.find(arg(3)):
                std::find_if(acm_->dialogs.begin(),acm_->dialogs.end(),[&](const auto& item){return item.second->window==window;});
            if(found==acm_->dialogs.end()){arg.return_to(state,0);return {};}
            const auto dialog=found->second;
            const auto address=found->first;
            return nested_continuation(state,memory,"ACM 音訊選擇對話框",
                [this,service,dialog,window,message,address](GuestState& s,GuestMemory& m)
                    ->service_callback::Task<std::optional<std::uint32_t>>{
                    const WindowsCall call(s,m,service);
                    const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
                    GuestScratch scratch(*this,m,s);
                    const auto api=[&](std::string_view symbol,std::vector<std::uint64_t> args)
                        ->service_callback::Task<std::uint64_t>{
                        co_return co_await service_callback::call_value(resolve("user32.dll",symbol).value,std::move(args));
                    };
                    const auto select=[&](std::uint64_t combo,unsigned index)->service_callback::Task<std::uint64_t>{
                        co_return co_await api("SendMessageW",{combo,0x14e,index,0});
                    };
                    const auto add=[&](std::uint64_t combo,std::u16string_view text)->service_callback::Task<std::uint64_t>{
                        const auto label=scratch.text(text);
                        co_return label?co_await api("SendMessageW",{combo,0x143,0,label}):UINT32_MAX;
                    };
                    const auto fill_options=[&](std::uint64_t combo)->service_callback::Task<bool>{
                        (void)co_await api("SendMessageW",{combo,0x14b,0,0});
                        for(const auto& option:dialog->groups.at(dialog->group).options){
                            const auto result=static_cast<std::uint32_t>(co_await add(combo,option.description));
                            if(result>=UINT32_MAX-1)co_return false;
                        }
                        (void)co_await select(combo,dialog->option);
                        co_return true;
                    };
                    const auto tag=gui_child(window,acm::tag_control);
                    const auto values=gui_child(window,acm::value_control);
                    if(message==0x110){
                        dialog->window=window;
                        if(!tag || !values){
                            (void)co_await api("EndDialog",{window,UINT32_MAX});
                            co_return finish(0);
                        }
                        dialog->initializing=true;
                        bool ready=true;
                        for(const auto& group:dialog->groups){
                            const auto result=static_cast<std::uint32_t>(co_await add(tag,group.label));
                            if(result>=UINT32_MAX-1){ready=false;break;}
                        }
                        if(ready){
                            (void)co_await select(tag,dialog->group);
                            ready=co_await fill_options(values);
                        }
                        if(const auto field=gui_child(window,acm::name_control);field && dialog->name_capacity){
                            if(const auto text=scratch.text(dialog->name))(void)co_await api("SetWindowTextW",{field,text});
                        }
                        dialog->initializing=false;
                        if(!ready){
                            (void)co_await api("EndDialog",{window,UINT32_MAX});
                            co_return finish(0);
                        }
                        if(dialog->hook)co_return finish(co_await service_callback::call_value(dialog->hook,
                                                             {window,message,call(2),dialog->custom}));
                        co_return finish(1);
                    }
                    if(dialog->initializing)co_return finish(0);
                    if(dialog->hook && co_await service_callback::call_value(dialog->hook,{window,message,call(2),call(3)}))
                        co_return finish(1);
                    const auto control=static_cast<unsigned>(call(2))&65535U;
                    const auto notice=static_cast<unsigned>(call(2))>>16;
                    if(message==0x53 || (message==0x111 && control==acm::help_control)){
                        if(dialog->owner && (dialog->style&(message==0x53?0x80U:4U))){
                            const auto label=scratch.text(message==0x53?u"acmchoose_contexthelp":u"acmchoose_help");
                            if(label){
                                const auto event=co_await api("RegisterWindowMessageW",{label});
                                if(event)(void)co_await api("SendMessageW",{dialog->owner,event,window,address});
                            }
                        }
                        co_return finish(1);
                    }
                    if(message==0x10 || (message==0x111 && control==2)){
                        (void)co_await api("EndDialog",{window,0});
                        co_return finish(1);
                    }
                    if(message!=0x111 || !tag || !values)co_return finish(0);
                    if((control==acm::tag_control && notice==1) || control==1){
                        const auto selected=static_cast<unsigned>(co_await api("SendMessageW",{tag,0x147,0,0}));
                        if(selected>=dialog->groups.size())co_return finish(1);
                        if(selected!=dialog->group){
                            dialog->group=selected;
                            dialog->option=0;
                            if(!(co_await fill_options(values)))co_return finish(1);
                        }
                    }
                    if(control!=1)co_return finish(0);
                    const auto index=static_cast<unsigned>(co_await api("SendMessageW",{values,0x147,0,0}));
                    if(index>=dialog->groups.at(dialog->group).options.size())co_return finish(1);
                    if(const auto field=gui_child(window,acm::name_control);field && dialog->name_capacity){
                        const auto capacity=std::min(dialog->name_capacity,1024U);
                        const auto text=scratch.allocate(capacity*2);
                        if(!text)co_return finish(1);
                        (void)co_await api("GetWindowTextW",{field,text,capacity});
                        dialog->name=read_utf16(m,GuestAddress{text},capacity);
                    }
                    dialog->option=index;
                    dialog->confirmed=true;
                    (void)co_await api("EndDialog",{window,1});
                    co_return finish(1);
                });
        }

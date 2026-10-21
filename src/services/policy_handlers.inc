    if(symbol=="EnterCriticalPolicySection" || symbol=="LeaveCriticalPolicySection"){
        const auto now=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        const auto finish=[&](std::uint64_t value,std::uint32_t error=0){if(error)last_error_=error;arg.return_to(state,value);return std::optional<std::uint32_t>{};};
        if(symbol=="LeaveCriticalPolicySection"){
            const auto found=policy_sections_.find(arg(0));if(found==policy_sections_.end() || !found->second.generation)return finish(0,6);
            if(found->second.thread!=threads_.current)return finish(0,288);
            try{const auto error=found->second.policy->release(found->second.thread,found->second.generation,now);
                policy_sections_.erase(found);return finish(error?0:1,error);
            }catch(const SecurityDescriptorError& error){return finish(0,error.code);}
        }
        std::uint64_t handle{};
        try{
            auto pending=std::find_if(policy_sections_.begin(),policy_sections_.end(),[&](const auto& entry){return entry.second.thread==threads_.current && !entry.second.generation;});
            if(pending==policy_sections_.end()){
                if(policy_sections_.size()>=1024)return finish(0,8);
                initialize_tokens();const auto name=arg(0)?u"machine":u"user-"+utf8_to_utf16(token_user_sid_string(effective_token_id()));
                auto policy=SharedPolicy::open(mapping_session(),name);handle=runtime_.allocate_wait_handle();
                pending=policy_sections_.emplace(handle,PolicySection{std::move(policy),threads_.current,0}).first;
            }else handle=pending->first;
            if(const auto wake=mapping_wake())pending->second.wake_epoch=wake->snapshot();
            const auto acquired=pending->second.policy->acquire(threads_.current,now);
            if(acquired.error==170){
                // 通知變更時由排程器提早喚醒；100 ms 上限仍探查異常退出而未通知的持有者。
                start_callbacks(state,memory,{{resolve("kernel32.dll","Sleep").value,{100}}},Continuation::PolicyRetry,handle);
                return std::nullopt;
            }
            if(acquired.error){policy_sections_.erase(handle);return finish(0,acquired.error);}
            pending->second.generation=acquired.generation;return finish(handle);
        }catch(const SecurityDescriptorError& error){if(handle)policy_sections_.erase(handle);return finish(0,error.code);}
         catch(const std::bad_alloc&){if(handle)policy_sections_.erase(handle);return finish(0,8);}
    }

            constexpr std::array<std::u16string_view,4> class_names{
                u"XAudio2",u"XAudio2Debug",u"AudioReverb",u"AudioVolumeMeter"
            };
            const auto& guids=xaudio::classic_guids.at(legacy_version);
            std::array<com_registry::ClassEntry,class_names.size()> classes{};
            for(unsigned i=0;i<classes.size();++i)classes[i]={guids[i],class_names[i]};
            const auto update=com_registry::update(runtime_.registry,registry_file_,registry_store_cache_,
                registry_view(0,x86_),classes,utf8_to_utf16(service.family),name=="DllUnregisterServer",runtime_.system_filetime());
            result=update.error;
            if(!result)for(const auto& path:update.changed)poll_registry_notifications(true,path,5);

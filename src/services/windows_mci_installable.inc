        if(installable){
            const auto driver=static_cast<std::uint32_t>(arg(0));
            const auto type=wave_installable?mci::wave_device:sequencer_installable?mci::sequencer_device:
                cd_installable?mci::cd_device:mci::avi_device;
            const auto registered=bridge.installable_drivers.find(driver);
            const bool owns_device=registered!=bridge.installable_drivers.end() && registered->second.module==service.family;
            switch(message){
            case 1:case 2:case 5:case 6: // DRV_LOAD、DRV_ENABLE、DRV_DISABLE、DRV_FREE
                return finish(1);
            case 3: { // DRV_OPEN
                const auto open=arg(4);
                if(!open)return finish(UINT32_MAX); // 驅動程式工作階段
                const auto id=static_cast<std::uint32_t>(memory.load(GuestAddress{open},32));
                if(!id || id==mci::all_devices || mci_control(bridge,id) || !bridge.can_open())return finish(0);
                // MCI_OPEN_DRIVER_PARMS 使用 pack(1)，型別與命令表都在設定字串指標之後。
                memory.check(GuestAddress{open+p+4},8,Access::Write);
                bridge.installable_drivers.emplace(id,MciState::InstallableDriver{{threads_.current},service.family});
                memory.store(GuestAddress{open+p+4},32,UINT32_MAX); // 使用 WinMM 共用命令字串表
                memory.store(GuestAddress{open+p+8},32,type);
                return finish(id);
            }
            case 4: // DRV_CLOSE
                if(driver==UINT32_MAX)return finish(1);
                if(!owns_device)return finish(0);
                if(bridge.devices.contains(driver) || bridge.videos.contains(driver) ||
                   bridge.sequences.contains(driver) || bridge.compact_discs.contains(driver)){
                    complete_mci_transport(driver,4);
                    bridge.devices.erase(driver);
                    bridge.videos.erase(driver);
                    bridge.sequences.erase(driver);
                    bridge.compact_discs.erase(driver);
                }
                bridge.installable_drivers.erase(registered);
                return finish(1);
            case 8: // DRV_QUERYCONFIGURE
                return finish(1);
            case 7: // DRV_CONFIGURE
                return nested_continuation(state,memory,"MCI 驅動程式說明",
                    [this,service,type](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                        const WindowsCall call(s,m,service);
                        GuestScratch scratch(*this,m,s);
                        const auto title=scratch.text(type==mci::wave_device?u"波形音訊驅動程式":
                            type==mci::sequencer_device?u"MIDI 序列器":type==mci::cd_device?u"光碟音訊":
                            service.family=="mciqtz32.dll"?u"Quartz 視訊驅動程式":u"AVI 視訊驅動程式");
                        const auto text=scratch.text(type==mci::wave_device?u"支援波形音訊播放、錄音及檔案儲存。":
                            type==mci::sequencer_device?u"支援 MIDI 檔案播放與傳輸控制。":
                            type==mci::cd_device?u"支援已映射光碟映像的音軌播放與傳輸控制。":u"支援視訊、音訊及播放控制。");
                        const auto result=title && text?co_await service_callback::call_value(
                            resolve("user32.dll","MessageBoxW").value,{call(3),text,title,0x40}):0;
                        call.return_to(s,result?1:0);
                        co_return std::optional<std::uint32_t>{};
                    });
            case 9:case 10: // DRV_INSTALL、DRV_REMOVE
                return finish(2); // DRVCNF_RESTART
            default:break;
            }
            if(message<0x800)return finish(0);
            if(driver==UINT32_MAX)return finish(sequencer_installable || cd_installable?mci::invalid_device:wave_installable?mci::unsupported:1U);
            if(!owns_device)return finish(mci::invalid_device);
            if(message==mci::open || message==mci::close)return finish(mci::unrecognized);
            if(message==mci::open_driver){
                if(bridge.videos.contains(driver) || bridge.devices.contains(driver) ||
                   bridge.sequences.contains(driver) || bridge.compact_discs.contains(driver))return finish(265);
                if(quartz_installable && (!(arg(3)&mci::open_element) || (arg(3)&mci::open_element_id)))
                    return finish(mci::missing_parameter);
            }else if(wave_installable && !bridge.devices.contains(driver)){
                return finish(message==mci::close_driver?0:mci::invalid_device);
            }else if(sequencer_installable && !bridge.sequences.contains(driver)){
                return finish(message==mci::close_driver?0:mci::invalid_device);
            }else if(quartz_installable && !bridge.videos.contains(driver)){
                return finish(message==mci::close_driver?0:mci::invalid_device);
            }else if(cd_installable && !bridge.compact_discs.contains(driver)){
                return finish(message==mci::close_driver?0:mci::invalid_device);
            }
            if(avi_installable){
#include "windows_mci_avi_driver.inc"
            }
        }

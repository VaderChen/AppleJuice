                // 裝置 ID 預約後，AVI 在載入影片之前仍可回報基本狀態。
                if(message==mci::open_driver){
                    if(!(arg(3)&mci::open_element) || (arg(3)&mci::open_element_id))
                        return finish(mci::missing_parameter);
                }else if(!bridge.videos.contains(driver)){
                    if(message==mci::status){
                        if(!(arg(3)&0x100U))return finish(mci::missing_parameter);
                        const auto block=arg(4);
                        if(!block)return finish(mci::null_parameters);
                        const auto item=memory.load(GuestAddress{block+2*p},32);
                        if(arg(3)&mci::test_flag)return finish(0);
                        if(item==4 || item==5 || item==7){
                            memory.store(GuestAddress{block+p},static_cast<std::uint8_t>(bits),
                                         item==4?mci::resource(524,524):mci::boolean_resource(false));
                            return finish(mci::resource_returned);
                        }
                    }
                    return finish(message==mci::close_driver?0:mci::not_ready);
                }

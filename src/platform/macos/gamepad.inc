Gamepad gamepad_profile(GCExtendedGamepad* profile){
    Gamepad value;
    const auto button=[&](GCControllerButtonInput* input,std::uint16_t bit){if(input){value.supported_buttons|=bit;if(input.isPressed)value.buttons|=bit;}};
    button(profile.dpad.up,1);button(profile.dpad.down,2);button(profile.dpad.left,4);button(profile.dpad.right,8);
    button(profile.buttonMenu,0x10);button(profile.buttonOptions,0x20);
    button(profile.leftThumbstickButton,0x40);button(profile.rightThumbstickButton,0x80);
    button(profile.leftShoulder,0x100);button(profile.rightShoulder,0x200);
    button(profile.buttonA,0x1000);button(profile.buttonB,0x2000);button(profile.buttonX,0x4000);button(profile.buttonY,0x8000);
    const std::array<GCControllerButtonInput*,2> triggers{profile.leftTrigger,profile.rightTrigger};
    for(unsigned i=0;i<2;++i){value.supported_triggers[i]=triggers[i]!=nil;value.triggers[i]=triggers[i].value;}
    const std::array<GCControllerAxisInput*,4> axes{profile.leftThumbstick.xAxis,profile.leftThumbstick.yAxis,profile.rightThumbstick.xAxis,profile.rightThumbstick.yAxis};
    for(unsigned i=0;i<4;++i){value.supported_sticks[i]=axes[i]!=nil;value.sticks[i]=axes[i].value;}
    return value;
}
// GameController 負責辨識各硬體的標準按鍵／獨立扳機；Core Haptics 控制左右 handle。
// 這是既有 input::Backend 的 profile，沒有另建 Windows API 專用 HID 或產品型號特例。
class NativeGamepads {
    struct Motor {
        CHHapticEngine* engine{};
        id<CHHapticPatternPlayer> player{};
        std::shared_ptr<std::atomic<bool>> interrupted=std::make_shared<std::atomic<bool>>(false);
        std::uint16_t speed{};
        ~Motor(){if(player)[player stopAtTime:CHHapticTimeImmediate error:nil];if(engine)[engine stopWithCompletionHandler:nil];}
        std::uint32_t set(GCController* controller,unsigned side,std::uint16_t value){
            const auto error_code=[](NSError* error){return error.code==CHHapticErrorCodeNotSupported?50U:31U;};
            NSError* error=nil;
            if(player){if(![player stopAtTime:CHHapticTimeImmediate error:&error]){player=nil;speed=0;return error_code(error);}player=nil;}
            speed=0;interrupted->store(false);
            if(!value)return 0;
            if(!engine){
                engine=[controller.haptics createEngineWithLocality:side?GCHapticsLocalityRightHandle:GCHapticsLocalityLeftHandle];
                if(!engine)return 50;
                engine.playsHapticsOnly=YES;engine.autoShutdownEnabled=NO;
                const auto flag=interrupted;
                engine.stoppedHandler=^(CHHapticEngineStoppedReason){flag->store(true);};
                engine.resetHandler=^{flag->store(true);};
            }
            if(![engine startAndReturnError:&error])return error_code(error);
            CHHapticEventParameter* intensity=[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:value/65535.0F];
            CHHapticEventParameter* sharpness=[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:side?1.0F:0.0F];
            CHHapticEvent* event=[[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous parameters:@[intensity,sharpness] relativeTime:0 duration:GCHapticDurationInfinite];
            CHHapticPattern* pattern=[[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:&error];
            if(!pattern)return error_code(error);
            player=[engine createPlayerWithPattern:pattern error:&error];
            if(!player || ![player startAtTime:CHHapticTimeImmediate error:&error]){player=nil;return error_code(error);}
            speed=value;return 0;
        }
    };
    struct Controller {
        GCController* native{};Guid id{};std::array<Motor,2> motors;
    };
    std::map<std::uintptr_t,std::unique_ptr<Controller>> controllers_;
    std::uint64_t next_id_{};
    static std::array<bool,2> motors(GCController* controller){
        const auto localities=controller.haptics.supportedLocalities;
        return {static_cast<bool>([localities containsObject:GCHapticsLocalityLeftHandle]),static_cast<bool>([localities containsObject:GCHapticsLocalityRightHandle])};
    }
    void refresh(){
        std::set<std::uintptr_t> attached;
        for(GCController* controller in [GCController controllers]){
            if(!controller.extendedGamepad)continue;
            const auto key=reinterpret_cast<std::uintptr_t>((__bridge void*)controller);attached.insert(key);
            if(!controllers_.contains(key)){
                auto record=std::make_unique<Controller>();record->native=controller;
                record->id={0x67,0x63,0x2d,0x70,0x61,0x64,0x40,0,0x80};
                const auto serial=++next_id_;for(unsigned i=0;i<7;++i)record->id[9+i]=static_cast<std::uint8_t>(serial>>(i*8));
                controllers_.emplace(key,std::move(record));
            }
        }
        // 斷線釋放 player／engine，停止上一個裝置的無限長震動。
        std::erase_if(controllers_,[&](const auto& entry){return !attached.contains(entry.first);});
    }
public:
    std::vector<Gamepad> sample(){@autoreleasepool{
        refresh();std::vector<Gamepad> out;
        for(auto& [key,record]:controllers_){
            (void)key;const auto profile=[record->native capture].extendedGamepad;
            if(!profile)continue;
            auto value=gamepad_profile(profile);value.device=record->id;value.rumble=motors(record->native);
            for(unsigned i=0;i<2;++i)if(record->motors[i].interrupted->exchange(false)){
                const auto speed=record->motors[i].speed;record->motors[i].player=nil;
                if(speed)(void)record->motors[i].set(record->native,i,speed);
            }
            out.push_back(value);
        }
        return out;
    }}
    std::uint32_t rumble(const Guid& id,std::uint16_t left,std::uint16_t right){@autoreleasepool{
        refresh();
        for(auto& [key,record]:controllers_){
            (void)key;if(record->id!=id)continue;
            const auto supported=motors(record->native);
            if((left && !supported[0]) || (right && !supported[1]))return 50;
            const std::array<std::uint16_t,2> speeds{left,right};
            for(unsigned i=0;i<2;++i){const auto result=record->motors[i].set(record->native,i,speeds[i]);if(result){for(unsigned j=0;j<2;++j)(void)record->motors[j].set(record->native,j,0);return result;}}
            return 0;
        }
        return 1167;
    }}
};

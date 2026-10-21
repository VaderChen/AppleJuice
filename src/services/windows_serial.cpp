#include "x64arm/windows_services.hpp"
#include "x64arm/windows_security_attributes.hpp"
#include "x64arm/serial.hpp"
#include "x64arm/unicode.hpp"
#include "serial_definition.hpp"
#include <algorithm>

namespace x64arm {
std::span<const WindowsServiceDescriptor> serial_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"kernel32.dll","BuildCommDCBA",2},{"kernel32.dll","BuildCommDCBW",2},
        {"kernel32.dll","BuildCommDCBAndTimeoutsA",3},{"kernel32.dll","BuildCommDCBAndTimeoutsW",3},
        {"kernel32.dll","GetCommState",2},{"kernel32.dll","SetCommState",2},
        {"kernel32.dll","GetCommTimeouts",2},{"kernel32.dll","SetCommTimeouts",2},
        {"kernel32.dll","GetCommMask",2},{"kernel32.dll","SetCommMask",2},
        {"kernel32.dll","WaitCommEvent",3},{"kernel32.dll","ClearCommError",3},
        {"kernel32.dll","GetCommModemStatus",2},{"kernel32.dll","EscapeCommFunction",2},
        {"kernel32.dll","SetCommBreak",1},{"kernel32.dll","ClearCommBreak",1},
        {"kernel32.dll","PurgeComm",2},{"kernel32.dll","SetupComm",3},
        {"kernel32.dll","CancelSynchronousIo",1}
    };return table;
}
std::optional<unsigned> WindowsServices::serial_port(std::u16string_view path){
    if(path.starts_with(u"\\\\.\\") || path.starts_with(u"\\\\?\\"))path.remove_prefix(4);
    if(path.ends_with(u':'))path.remove_suffix(1);
    if(path.size()<4 || path.size()>7)return {};
    if((path[0]!=u'C' && path[0]!=u'c') || (path[1]!=u'O' && path[1]!=u'o') || (path[2]!=u'M' && path[2]!=u'm'))return {};
    unsigned number=0;for(auto c:path.substr(3)){if(c<u'0' || c>u'9')return {};number=number*10+static_cast<unsigned>(c-u'0');}
    return number && number<=4096?std::optional<unsigned>{number}:std::nullopt;
}
void WindowsServices::map_serial_port(unsigned number,const std::filesystem::path& path){
    if(!number || number>4096 || !path.is_absolute())throw std::invalid_argument("序列埠映射需要 COM1–COM4096 與絕對宿主路徑");
    if(std::any_of(files_.begin(),files_.end(),[&](const auto& item){return item.second.serial && item.second.key=="COM"+std::to_string(number);}))throw std::invalid_argument("不能修改使用中的序列埠映射");
    serial_ports_[number]=path.string();
}
void WindowsServices::scan_serial_ports(){
    if(serial_ports_scanned_)return;
    const auto ports=NativeSerial::devices();
    native_serial_paths_.insert(ports.begin(),ports.end());
    for(std::size_t i=0;i<ports.size() && i<4096;++i)serial_ports_.try_emplace(static_cast<unsigned>(i+1),ports[i]);
    serial_ports_scanned_=true;
}
std::optional<std::uint32_t> WindowsServices::invoke_serial(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service,const FileOpenArguments* arguments){
    const WindowsCall frame(state,memory,service);const auto arg=[&](unsigned i){return arguments?arguments->at(i):frame(i);};const auto name=service.symbol;
    std::uint64_t result=0;const auto fail=[&](std::uint32_t e){last_error_=e;result=0;};
    const auto d=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    if(name=="WaitCommEvent")return invoke_file_io(state,memory,service);
    try{
        if(name.starts_with("BuildCommDCB")){
            if(!arg(0) || !arg(1))fail(87);
            else {
                const auto definition=name.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)},4096)):read_ansi(memory,arg(0),4096);
                std::array<std::uint8_t,28> settings{};memory.read(GuestAddress{arg(1)},settings);std::optional<bool> timeout;
                if(!serial_definition(definition,settings,timeout))fail(87);
                else if(name.starts_with("BuildCommDCBAndTimeouts") && timeout && !arg(2))fail(87);
                else {
                    memory.check(GuestAddress{arg(1)},settings.size(),Access::Write);
                    if(name.starts_with("BuildCommDCBAndTimeouts") && timeout){memory.check(GuestAddress{arg(2)},20,Access::Write);for(unsigned i=0;i<5;++i)memory.store(GuestAddress{arg(2)+4*i},32,i==4 && *timeout?60000:0);}
                    memory.write(GuestAddress{arg(1)},settings);result=1;
                }
            }
        }else if(name=="CancelSynchronousIo"){
            const auto id=thread_id(arg(0));if(!id)fail(6);else if(!(thread_access(arg(0))&1U))fail(5);
            else {bool found=false;for(auto& io:pending_file_io_)if(io.thread==id && !io.done && (io.blocking || !io.overlapped)){io.result={0,995};found=true;}
                if(!found)fail(1168);else {poll_file_io(memory);result=1;}
            }
        }else if(name=="CreateFileA" || name=="CreateFileW"){
            result=x86_?UINT32_MAX:UINT64_MAX;
            const auto path=name.ends_with('W')?read_utf16(memory,GuestAddress{arg(0)},256):utf8_to_utf16(read_ansi(memory,arg(0),256));
            const auto number=serial_port(path);const auto security=synchronization_security(memory,arg(3),x86_);
            const auto error=[&](std::uint32_t e){last_error_=e;};
            if(!number || d(2) || d(4)!=3 || arg(6) || !(d(1)&0xc0000000U) || (d(1)&~0xc0000000U) || (d(5)&~0x40000080U))error(87);
            else if(security.error)error(security.error);
            else if(!runtime_.network.external_io)error(50);
            else if(files_.size()>=1024)error(4);
            else {
                scan_serial_ports();
                const auto key="COM"+std::to_string(*number);const auto found=serial_ports_.find(*number);
                if(found==serial_ports_.end())error(2);
                else if(std::any_of(files_.begin(),files_.end(),[&](const auto& entry){return entry.second.serial && entry.second.path==found->second;}))error(5);
                else {auto opened=NativeSerial::open(found->second,d(1));if(opened.error)error(opened.error);
                    else {FileHandle file;file.serial=std::move(opened.port);file.key=key;file.path=found->second;file.access=d(1);file.mode=(d(5)&0x40000000U)?0:0x20;file.bytes=std::make_shared<const std::vector<std::uint8_t>>();
                        const auto handle=runtime_.allocate_wait_handle();file.object=handle;files_.emplace(handle,std::move(file));runtime_.handle_flags[handle]=security.inherit?1:0;result=handle;
                    }
                }
            }
        }else {
            const auto file=files_.find(arg(0));if(file==files_.end() || !file->second.serial)fail(6);
            else {
                auto& port=*file->second.serial;std::uint32_t e=0;
                if(name=="GetCommState" || name=="SetCommState"){
                    if(!arg(1) || memory.load(GuestAddress{arg(1)},32)!=28)e=87;
                    else {std::array<std::uint8_t,28> settings{};
                        if(name=="GetCommState"){memory.check(GuestAddress{arg(1)},28,Access::Write);e=port.get_state(settings);if(!e)memory.write(GuestAddress{arg(1)},settings);}
                        else {memory.read(GuestAddress{arg(1)},settings);e=port.set_state(settings);}
                    }
                }else if(name=="GetCommTimeouts" || name=="SetCommTimeouts"){
                    if(!arg(1))e=87;
                    else if(name=="GetCommTimeouts"){memory.check(GuestAddress{arg(1)},20,Access::Write);const auto values=port.timeouts();for(unsigned i=0;i<5;++i)memory.store(GuestAddress{arg(1)+4*i},32,values[i]);}
                    else {std::array<std::uint32_t,5> values{};for(unsigned i=0;i<5;++i)values[i]=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)+4*i},32));e=port.timeouts(values);}
                }else if(name=="GetCommMask"){
                    if(!arg(1))e=87;else memory.store(GuestAddress{arg(1)},32,port.mask());
                }else if(name=="SetCommMask"){e=port.mask(d(1));if(!e)poll_file_io(memory);}
                else if(name=="SetupComm")e=port.setup(d(1),d(2));
                else if(name=="PurgeComm"){
                    e=port.purge(d(1));if(!e && (d(1)&3)){
                        for(auto& io:pending_file_io_)if(!io.done && io.serial && files_.contains(io.handle) && files_.at(io.handle).object==file->second.object &&
                            (((d(1)&1) && io.kind==PendingFileIo::Kind::Write) || ((d(1)&2) && io.kind==PendingFileIo::Kind::Read)))io.result={0,995};
                        poll_file_io(memory);
                    }
                }else if(name=="EscapeCommFunction")e=port.escape(d(1));
                else if(name=="SetCommBreak" || name=="ClearCommBreak")e=port.escape(name=="SetCommBreak"?8:9);
                else if(name=="GetCommModemStatus"){
                    if(!arg(1))e=87;else {memory.check(GuestAddress{arg(1)},4,Access::Write);std::uint32_t status=0;e=port.modem(status);if(!e)memory.store(GuestAddress{arg(1)},32,status);}
                }else if(name=="ClearCommError"){
                    if(arg(1))memory.check(GuestAddress{arg(1)},4,Access::Write);if(arg(2))memory.check(GuestAddress{arg(2)},12,Access::Write);
                    std::uint32_t errors=0;std::array<std::uint32_t,3> stat{};e=port.clear(errors,stat);
                    if(!e){if(arg(1))memory.store(GuestAddress{arg(1)},32,errors);if(arg(2))for(unsigned i=0;i<3;++i)memory.store(GuestAddress{arg(2)+4*i},32,stat[i]);}
                }else throw HostCallFault("未知的序列埠服務");
                if(e)fail(e);else {
                    const auto bytes=(name=="GetCommState" || name=="SetCommState")?28U:(name=="GetCommTimeouts" || name=="SetCommTimeouts")?20U:name=="ClearCommError"?((arg(1)?4U:0U)+(arg(2)?12U:0U)):(name=="GetCommMask" || name=="SetCommMask" || name=="GetCommModemStatus")?4U:0U;
                    process_io::record(process_io::Kind::Other,bytes);result=1;
                }
            }
        }
    }catch(const MemoryFault&){fail(87);if(name.starts_with("CreateFile"))result=x86_?UINT32_MAX:UINT64_MAX;}
    catch(const std::bad_alloc&){fail(8);if(name.starts_with("CreateFile"))result=x86_?UINT32_MAX:UINT64_MAX;}
    frame.return_to(state,result);return std::nullopt;
}
}

#include "x64arm/windows_services.hpp"
#include "x64arm/windows_security_attributes.hpp"
#include "x64arm/security_access.hpp"
#include "x64arm/serial.hpp"
#include "x64arm/windows_raw_input.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/shared_wake.hpp"
#include "x64arm/startup_profile.hpp"
#include <algorithm>

namespace x64arm {
namespace {
std::uint32_t pipe_status(std::uint32_t error) {
    switch (error) {
    case 0:
        return 0;
    case 5:
        return 0xc0000022;
    case 8:
        return 0xc0000017;
    case 122:
        return 0xc0000023;
    case 998:
        return 0xc0000005;
    case 33:
        return 0xc0000054;
    case 38:
        return 0xc0000011;
    case 234:
        return 0x80000005;
    case 109:
        return 0xc000014b;
    case 233:
        return 0xc00000b0;
    case 232:
        return 0xc00000b1;
    case 536:
        return 0xc00000b3;
    case 995:
        return 0xc0000120;
    case 1022:
        return 0x10c;
    case 121:
        return 0xc00000b5;
    case 1117:
        return 0xc0000185;
    case 1167:
        return 0xc000009d;
    default:
        return 0xc000000d;
    }
}
std::uint32_t pipe_error(std::uint32_t status) {
    switch (status) {
    case 0:
        return 0;
    case 0xc0000022:
        return 5;
    case 0xc0000017:
        return 8;
    case 0xc0000023:
        return 122;
    case 0xc0000005:
        return 998;
    case 0xc0000054:
        return 33;
    case 0xc0000011:
        return 38;
    case 0x80000005:
        return 234;
    case 0xc000014b:
        return 109;
    case 0xc00000b0:
        return 233;
    case 0xc00000b1:
        return 232;
    case 0xc00000b3:
        return 536;
    case 0xc0000120:
        return 995;
    case 0x10c:
        return 1022;
    case 0xc00000b5:
        return 121;
    case 0xc0000185:
        return 1117;
    case 0xc000009d:
        return 1167;
    case 0x103:
        return 996;
    default:
        return 87;
    }
}
} // namespace
std::optional<std::uint32_t> WindowsServices::invoke_overlapped_result(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state,memory,service);
    const bool extended=service.symbol=="GetOverlappedResultEx",winsock=service.symbol=="WSAGetOverlappedResult";
    // Win64 的 BOOL 仍為 32 bits；堆疊參數的上半部沒有值的語義。
    const bool alertable=extended && static_cast<std::uint32_t>(arg(4))!=0;
    const auto unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    auto& thread=threads_.items.at(threads_.current);
    const auto done=[&](std::uint64_t value,std::uint32_t error=0){thread.wait={};if(error)last_error_=error;arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    try {
        const bool socket=runtime_.network.sockets.contains(arg(0));
        const auto notification=address_notifications_.find(arg(0));
        if(winsock && !runtime_.network.startups)return done(0,10093);
        if(winsock?!socket:(!socket && notification==address_notifications_.end() && !files_.contains(arg(0))))return done(0,winsock?10038:6);
        if(!arg(1) || !arg(2) || (winsock && !arg(4)))return done(0,winsock?10014:87);
        memory.check(GuestAddress{arg(1)},x86_?20:32,Access::Read);memory.check(GuestAddress{arg(2)},4,Access::Write);
        if(winsock)memory.check(GuestAddress{arg(4)},4,Access::Write);
        if(notification!=address_notifications_.end() && notification->second.overlapped!=arg(1))return done(0,87);
        const bool resumed=thread.wait.kind==ThreadWaitKind::FileIo && thread.wait.handles.size()==3 && thread.wait.handles[0]==arg(0) && thread.wait.handles[1]==arg(1);
        if(resumed && thread.wait.handles[2] && !runtime_.wait_objects.contains(thread.wait.handles[2]) && !file_io_signaled(thread.wait.handles[2]).has_value())return done(0,6);
        if(resumed && thread.wait.timed_out)return done(0,258);
        if(resumed && thread.wait.alertable && !thread.apcs.empty()){
            thread.wait={};if(dispatch_user_apc(state,memory,0))return std::nullopt;
        }
        if(socket)poll_network_io(memory);
        else if(notification!=address_notifications_.end())poll_address_notifications(memory);
        else {if(files_.at(arg(0)).metadata.attributes&0x10U)poll_directory_notifications();poll_file_io(memory);}
        const auto status=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},bits));
        if(status==0x103) {
            const auto timeout=extended?static_cast<std::uint32_t>(arg(3)):static_cast<std::uint32_t>(arg(3))?UINT32_MAX:0U;
            if(!timeout || (resumed && thread.wait.signaled))return done(0,996);
            const auto event=memory.load(GuestAddress{arg(1)+(x86_?16:24)},bits)&~UINT64_C(1);
            if(event){const auto found=runtime_.wait_objects.find(event);if(found==runtime_.wait_objects.end() || found->second.kind!=WaitObject::Kind::Event)return done(0,6);if(!(found->second.access&0x100000U))return done(0,5);}
            if(alertable && !thread.apcs.empty()){
                thread.wait={};if(dispatch_user_apc(state,memory,0))return std::nullopt;
            }
            if(!resumed) {
                thread.wait={};thread.wait.kind=ThreadWaitKind::FileIo;
                thread.wait.handles={arg(0),arg(1),event?event:notification!=address_notifications_.end()?0:arg(0)};
                thread.wait.alertable=alertable;
                const auto duration=std::uint64_t{timeout}*10000;
                thread.wait.deadline=timeout==UINT32_MAX || duration>UINT64_MAX-runtime_.clock_ticks?UINT64_MAX:runtime_.clock_ticks+duration;
            }
            thread.status=ThreadStatus::Waiting;schedule(state,memory);return std::nullopt;
        }
        if(!winsock)memory.store(GuestAddress{arg(2)},32,memory.load(GuestAddress{arg(1)+unit},bits));
        if(status) {
            auto error=pipe_error(status);
            if(socket)error=winsock?socket_io_error(status):status==0xc000020d?64U:status==0xc0000241?1236U:status==0xc000009a?1450U:status==0xc00000b5?121U:status==0xc000000d?87U:socket_io_error(status);
            if(winsock){const auto completed=runtime_.network.completion_errors.find(arg(1));if(completed!=runtime_.network.completion_errors.end() && completed->second.first==arg(0) && socket_io_status(completed->second.second)==status)error=completed->second.second;}
            return done(0,error);
        }
        if(winsock){memory.store(GuestAddress{arg(2)},32,memory.load(GuestAddress{arg(1)+unit},bits));const auto flags=runtime_.network.completion_flags.find(arg(1));memory.store(GuestAddress{arg(4)},32,flags!=runtime_.network.completion_flags.end() && flags->second.first==arg(0)?flags->second.second:0);}
        return done(1);
    }catch(const MemoryFault&){return done(0,winsock?10014:998);}catch(const std::bad_alloc&){return done(0,8);}
}
std::optional<bool> WindowsServices::file_io_signaled(std::uint64_t handle) {
    if (handle == 0x100 && console_handle(handle))
        return console_input_ready(handle);
    if (const auto file = files_.find(handle); file != files_.end())
        return file->second.io_signaled;
    if (const auto socket = runtime_.network.sockets.find(handle); socket != runtime_.network.sockets.end())
        return socket->second->io_signaled;
    return {};
}
void WindowsServices::set_file_io_signaled(std::uint64_t handle, bool signaled) {
    if (const auto file = files_.find(handle); file != files_.end()) {
        if (signaled && (file->second.completion_modes & 2))
            return;
        for (auto &[id, item] : files_) {
            (void)id;
            if (item.object == file->second.object)
                item.io_signaled = signaled;
        }
    } else if (const auto socket = runtime_.network.sockets.find(handle); socket != runtime_.network.sockets.end()) {
        if (!signaled || !(socket->second->completion_modes & 2))
            socket->second->io_signaled = signaled;
    }
}
std::optional<std::uint64_t> WindowsServices::file_io_poll_deadline(const SharedWake* wake) const {
    auto deadline=UINT64_MAX;
    for(const auto& io:pending_file_io_){
        if(io.done)continue;
        const auto file=files_.find(io.handle);
        if(!wake || !io.attempted || !io.result.pending || io.result.error || !io.pipe_poll_deadline ||
           io.flush || io.serial || io.hid || io.kind==PendingFileIo::Kind::Lock || io.kind==PendingFileIo::Kind::Directory ||
           file==files_.end() || !file->second.pipe || !file->second.pipe->uses_wake(wake))return {};
        deadline=std::min(deadline,io.pipe_poll_deadline);
    }
    for(const auto& [id,thread]:threads_.items){
        if(thread.status!=ThreadStatus::Waiting || thread.wait.kind!=ThreadWaitKind::FileIo)continue;
        // WaitNamedPipe、OVERLAPPED／event 查詢與 socket 等待沿用原輪詢；僅接受已登錄的普通管道 I/O。
        if(thread.wait.handles.size()!=1 || !thread.wait.handles.front() ||
           !std::any_of(pending_file_io_.begin(),pending_file_io_.end(),[&](const auto& io){
               return !io.done && (!io.overlapped || io.blocking) && io.thread==id && io.handle==thread.wait.handles.front();
           }))return {};
    }
    return deadline;
}
void WindowsServices::cancel_file_io(GuestMemory &memory, std::uint64_t handle, std::uint64_t thread,
                                     std::uint64_t overlapped) {
    for (auto &io : pending_file_io_)
        if (io.handle == handle && !io.done && (!thread || io.thread == thread) &&
            (!overlapped || io.overlapped == overlapped))
            io.result = {0, 995};
    poll_file_io(memory);
}
void WindowsServices::poll_file_io(GuestMemory &memory, bool scheduler_poll) {
    if (pending_file_io_.empty())
        return;
    startup_profile::Scope measurement("io", "file_poll");
    const auto wake = scheduler_poll && runtime_.host_clock ? mapping_wake() : nullptr;
    // 在檢查完成條件之前擷取 epoch；檢查途中發生的通知留待下一輪處理。
    const auto epoch = wake ? wake->snapshot() : 0;
    const unsigned unit = x86_ ? 4 : 8;
    const std::uint8_t bits = x86_ ? 32 : 64;
    for (auto &io : pending_file_io_) {
        if (io.done)
            continue;
        const auto file = files_.find(io.handle);
        if (wake && !io.result.error && file != files_.end() && file->second.pipe) {
            if (io.attempted && io.result.pending && io.pipe_wake_epoch == epoch &&
                runtime_.clock_ticks < io.pipe_poll_deadline)
                continue;
            io.pipe_wake_epoch = epoch;
            // 正常連接、讀寫、關閉都會通知；未正常關閉的對端仍在 50 ms 內重新查驗。
            io.pipe_poll_deadline =
                runtime_.clock_ticks > UINT64_MAX - 500000 ? UINT64_MAX : runtime_.clock_ticks + 500000;
        }
        struct Progress {
            PendingFileIo &io;
            ~Progress() {
                const auto kind = io.kind == PendingFileIo::Kind::Read    ? process_io::Kind::Read
                                  : io.kind == PendingFileIo::Kind::Write ? process_io::Kind::Write
                                                                          : process_io::Kind::Other;
                const auto bytes =
                    io.kind == PendingFileIo::Kind::Transact
                        ? io.input_transferred + std::max<std::uint64_t>(io.transferred, io.result.bytes)
                    : io.kind == PendingFileIo::Kind::Write
                        ? std::max({io.input_transferred, io.transferred, static_cast<std::uint64_t>(io.result.bytes)})
                        : std::max<std::uint64_t>(io.transferred, io.result.bytes);
                process_io::progress(kind, bytes, io.attempted && (io.done || bytes > 0), io.io_recorded,
                                     io.io_accounted);
            }
        } progress{io};
        if (!io.result.error && (file == files_.end() || (io.kind == PendingFileIo::Kind::Directory ? false
                                                          : io.flush                              ? !file->second.native
                                                          : io.kind == PendingFileIo::Kind::Lock ? !file->second.native
                                                          : io.hid                               ? !file->second.hid
                                                          : io.serial                            ? !file->second.serial
                                                                                                 : !file->second.pipe)))
            io.result = {0, 995};
        if (!io.result.error && io.flush) {
            // 宿主工作執行緒完成 fsync 後以 release 發布結果；未完成前維持 pending，不阻塞排程器。
            io.attempted = true;
            io.result = io.flush->done.load(std::memory_order_acquire) ? PipeResult{0, io.flush->error}
                                                                          : PipeResult{0, 0, true};
        } else if (!io.result.error && io.kind == PendingFileIo::Kind::Lock) {
            io.attempted = true;
            std::uint32_t error=0;
            if(runtime_.network.external_io){(void)mapping_session();error=filesystem_.publish_shared_file(*file->second.native);}
            if(!error)error = file->second.native->lock_range(io.offset, io.length, io.exclusive, false);
            io.result = {0, error == 33 && !io.immediate ? 0U : error, error == 33 && !io.immediate};
        } else if (!io.result.error && io.kind == PendingFileIo::Kind::Directory) {
            io.attempted = true;
            io.result = read_directory_notification(io, memory);
        } else if (!io.result.error && io.hid) {
            io.attempted = true;
            io.result = poll_hid_io(io, memory);
        } else if (!io.result.error && io.serial) {
            io.attempted = true;
            io.result = file->second.serial->poll(*io.serial);
            io.transferred = io.serial->position;
            if (!io.result.pending)
                try {
                    if (io.kind == PendingFileIo::Kind::Read && io.result.bytes)
                        memory.write(GuestAddress{io.output}, std::span(io.serial->data).first(io.result.bytes));
                } catch (const MemoryFault &) {
                    io.result = {0, 998};
                }
        } else if (!io.result.error) {
            io.attempted = true;
            auto &endpoint = *file->second.pipe;
            if (io.kind == PendingFileIo::Kind::Connect)
                io.result = endpoint.connect();
            else if (io.kind == PendingFileIo::Kind::Flush)
                io.result = endpoint.flush();
            else if (io.kind == PendingFileIo::Kind::Transact) {
                std::vector<std::uint8_t> input(io.transaction ? 0 : io.input_size), output(io.output_size);
                if (!io.transaction)
                    memory.read(GuestAddress{io.input}, input);
                io.result = endpoint.transact(input, output, io.transaction);
                // Transaction 在整則要求排入共用佇列後才公開；累計已提交 bytes，回覆只追加進度。
                if (io.transaction)
                    io.input_transferred = io.input_size;
                if (!io.result.pending && (!io.result.error || io.result.error == 234))
                    memory.write(GuestAddress{io.output}, std::span(output).first(io.result.bytes));
            } else {
                if ((io.kind == PendingFileIo::Kind::Write || io.kind == PendingFileIo::Kind::Transact) &&
                    !io.written) {
                    std::vector<std::uint8_t> data(io.pipe_write.end ? 0 : io.input_size);
                    if (!io.pipe_write.end)
                        memory.read(GuestAddress{io.input}, data);
                    io.result = endpoint.write(data, io.immediate, &io.pipe_write);
                    io.input_transferred = std::max<std::uint64_t>(io.input_transferred, io.result.bytes);
                    io.written = !io.result.pending && !io.result.error;
                }
                if (io.kind == PendingFileIo::Kind::Read || (io.kind == PendingFileIo::Kind::Transact && io.written)) {
                    // 待命讀取先走無鎖提示；每個時間片都會輪詢，命中時不配置清零緩衝區、不取 session 鎖。
                    if (endpoint.read_would_block())
                        io.result = {0, 0, true};
                    else {
                        std::vector<std::uint8_t> data(io.output_size);
                        io.result = endpoint.read(data, (file->second.pipe_mode & 2) != 0);
                        if (!io.result.pending && (!io.result.error || io.result.error == 234)) {
                            io.transferred = io.result.bytes;
                            try {
                                memory.write(GuestAddress{io.output}, std::span(data).first(io.result.bytes));
                            } catch (const MemoryFault &) {
                                io.result = {0, 998};
                            }
                        }
                    }
                }
            }
        }
        if (io.result.pending && io.immediate && io.kind != PendingFileIo::Kind::Lock)
            io.result = {0, io.kind == PendingFileIo::Kind::Connect ? 536U : 232U};
        if (io.result.pending)
            continue;
        if (io.serial && io.kind == PendingFileIo::Kind::CommEvent)
            try {
                memory.store(GuestAddress{io.output}, 32, io.result.error ? 0U : io.serial->events);
            } catch (const MemoryFault &) {
                io.result = {0, 998};
            }
        io.done = true;
        set_file_io_signaled(io.handle, true);
        if (io.routine && (io.notify || !io.result.error || io.result.error == 234)) {
            const auto thread = threads_.items.find(io.thread);
            if (thread != threads_.items.end() && thread->second.status != ThreadStatus::Exited &&
                !thread->second.terminating) {
                thread->second.apcs.push_back(
                    {io.routine, io.native_iosb
                                     ? std::vector<std::uint64_t>{io.native_context, io.native_iosb, 0}
                                     : std::vector<std::uint64_t>{io.result.error, io.result.bytes, io.overlapped}});
                threads_.yield = true;
            }
        }
        if (io.count_pointer)
            memory.store(GuestAddress{io.count_pointer}, 32, io.result.bytes);
        if (io.overlapped) {
            memory.store(GuestAddress{io.overlapped}, bits, pipe_status(io.result.error));
            memory.store(GuestAddress{io.overlapped + unit}, bits, io.result.bytes);
            if (io.native_iosb) {
                memory.store(GuestAddress{io.native_iosb}, bits, pipe_status(io.result.error));
                memory.store(GuestAddress{io.native_iosb + unit}, bits, io.result.bytes);
            }
            if (io.event) {
                const auto event = runtime_.wait_objects.find(io.event & ~UINT64_C(1));
                if (event != runtime_.wait_objects.end())
                    event->second.set_signal(true);
            }
            // 同步失敗的要求不會再排入 IOCP；已回 ERROR_IO_PENDING 的要求則須交付錯誤／取消封包。
            if (!io.routine && file != files_.end() && !(io.event & 1) &&
                (io.notify || !io.result.error || io.result.error == 234) &&
                (io.notify || io.result.error || !(file->second.completion_modes & 1))) {
                const auto port = runtime_.completion_ports.find(file->second.completion_port);
                if (port != runtime_.completion_ports.end())
                    port->second.packets.push_back({file->second.completion_key,
                                                    io.native_iosb ? io.native_context : io.overlapped,
                                                    pipe_status(io.result.error), io.result.bytes});
            }
        }
        if (io.native_scratch) {
            (void)free_heap(memory, io.native_scratch, threads_.items.at(io.thread).context);
            io.native_scratch = 0;
        }
    }
    std::erase_if(pending_file_io_,
                  [](const auto &io) { return io.overlapped && !io.blocking && io.notify && io.done; });
}
std::optional<std::uint32_t> WindowsServices::invoke_file_io(GuestState &state, GuestMemory &memory,
                                                             const WindowsServiceDescriptor &service,const FileOpenArguments* arguments) {
    if(service.symbol=="GetOverlappedResult" || service.symbol=="GetOverlappedResultEx")return invoke_overlapped_result(state,memory,service);
    const WindowsCall frame(state, memory, service);
    const auto arg=[&](unsigned i){return arguments?arguments->at(i):frame(i);};
    const auto symbol = service.symbol;
    const auto u32 = [&](unsigned i) { return static_cast<std::uint32_t>(arg(i)); };
    const unsigned unit = x86_ ? 4 : 8;
    const std::uint8_t bits = x86_ ? 32 : 64;
    std::uint64_t result = 0;
    const auto fail = [&](std::uint32_t error) {
        last_error_ = error;
        result = 0;
    };
    const auto text = [&]() {
        return symbol.ends_with('W') ? read_utf16(memory, GuestAddress{arg(0)}, 256)
                                     : utf8_to_utf16(read_ansi(memory, arg(0), 256));
    };
    if ((symbol == "CancelIo" || symbol == "CancelIoEx") && runtime_.network.sockets.contains(arg(0)))
        return invoke_network_extension(state, memory, service);
    if (symbol == "CreatePipe") {
        if (!arg(0) || !arg(1))
            fail(87);
        else if (!runtime_.network.external_io)
            fail(50);
        else if (files_.size() > 1022)
            fail(4);
        else {
            const auto first = runtime_.allocate_wait_handle(), second = runtime_.allocate_wait_handle();
            try {
                memory.check(GuestAddress{arg(0)}, unit, Access::Write);
                memory.check(GuestAddress{arg(1)}, unit, Access::Write);
                const auto attributes = read_security_attributes(memory, arg(2), x86_, true);
                const auto descriptor = object_security_descriptor(attributes.descriptor, true, true);
                auto pair = NamedPipe::anonymous(mapping_session(), u32(3), descriptor);
                if (pair.error)
                    fail(pair.error);
                else {
                    const auto insert = [&](std::uint64_t id, std::shared_ptr<NamedPipe> endpoint,
                                            std::uint32_t access) {
                        FileHandle file;
                        file.pipe = std::move(endpoint);
                        file.bytes = std::make_shared<const std::vector<std::uint8_t>>();
                        file.access = access;
                        file.mode = 0x20;
                        file.object = id;
                        files_.emplace(id, std::move(file));
                        runtime_.win_file_positions.emplace(id, 0);
                        runtime_.handle_flags[id] = attributes.inherit ? 1U : 0U;
                    };
                    insert(first, std::move(pair.read), 0x120089U);
                    insert(second, std::move(pair.write), 0x120116U);
                    memory.store(GuestAddress{arg(0)}, bits, first);
                    memory.store(GuestAddress{arg(1)}, bits, second);
                    result = 1;
                }
            } catch (const SecurityDescriptorError &error) {
                fail(error.code);
            } catch (const MemoryFault &) {
                fail(998);
            } catch (const std::bad_alloc &) {
                fail(8);
            }
            if (!result)
                for (const auto id : {first, second}) {
                    files_.erase(id);
                    runtime_.win_file_positions.erase(id);
                    runtime_.handle_flags.erase(id);
                }
        }
    } else if (symbol.starts_with("CreateNamedPipe") || symbol.starts_with("CreateFile")) {
        const bool server = symbol.starts_with("CreateNamedPipe");
        NamedPipe::Opened opened;
        bool inherit{};
        std::uint32_t rights{};
        try {
            auto attributes = read_security_attributes(memory, arg(server ? 7 : 3), x86_, server);
            inherit = attributes.inherit;
            constexpr security::AccessMapping mapping{0x120089U, 0x120116U, 0x1200a0U, 0x1f01ffU};
            rights = server ? ((u32(1) & 1U ? mapping[0] : 0U) | (u32(1) & 2U ? mapping[1] : 0U) | 0x100000U |
                               (u32(1) & 0x10c0000U))
                            : u32(1);
            if (!arg(0) || (server && (!(u32(1) & 3) || !u32(3) || u32(3) > 255)))
                opened.error = 87;
            else if (!runtime_.network.external_io)
                opened.error = 50;
            else if (files_.size() >= 1024)
                opened.error = 4;
            else if (server && (u32(1) & ~0xc10c0003U))
                opened.error = 87;
            else if (rights & 0x1000000U)
                opened.error = 1314;
            else if (!server && (u32(4) != 3 || arg(6) || (u32(1) & ~0xf31f01ffU) || (u32(5) & ~0x401f0080U)))
                opened.error = 50;
            else {
                if (server) {
                    auto &sd = attributes.descriptor;
                    if (!(sd.control & 4U)) {
                        initialize_tokens();
                        sd.control |= 4;
                        sd.data[3] = security::encode_acl(
                            {security::encode_ace(
                                 {0, 0, mapping[3], security::account_sid(*security::find_account("SYSTEM"))}),
                             security::encode_ace(
                                 {0, 0, mapping[3], security::account_sid(*security::find_account("Administrators"))}),
                             security::encode_ace(
                                 {0, 0, mapping[3], runtime_.token_objects.at(effective_token_id()).owner}),
                             security::encode_ace(
                                 {0, 0, mapping[0], security::account_sid(*security::find_account("Everyone"))}),
                             security::encode_ace(
                                 {0, 0, mapping[0],
                                  security::account_sid(*security::find_account("Anonymous Logon"))})});
                    }
                    const auto bytes = object_security_descriptor(std::move(sd), true, true);
                    opened = NamedPipe::create(
                        mapping_session(), text(), u32(1) & 3, u32(2), u32(3), u32(4), u32(5), u32(6),
                        (u32(1) & 0x80000) != 0, 0x2000, 1, UINT32_MAX, bytes,
                        [&](const auto &stored) { return object_security_access(stored, rights | 4U, mapping); });
                } else {
                    auto [context, error] = pipe_client_token(u32(5));
                    if (error)
                        opened.error = error;
                    else
                        opened = NamedPipe::open(
                            mapping_session(), text(), 0, context,
                            runtime_.token_objects.at(effective_token_id()).integrity,
                            [&](const auto &stored) { return object_security_access(stored, rights, mapping); });
                }
            }
        } catch (const SecurityDescriptorError &error) {
            opened.error = error.code;
        } catch (const MemoryFault &) {
            opened.error = 998;
        } catch (const std::bad_alloc &) {
            opened.error = 8;
        }
        if (opened.error) {
            last_error_ = opened.error;
            result = x86_ ? UINT32_MAX : UINT64_MAX;
        } else {
            FileHandle file;
            file.pipe = std::move(opened.pipe);
            file.bytes = std::make_shared<const std::vector<std::uint8_t>>();
            file.access = server ? rights : opened.access;
            file.mode = (u32(server ? 1 : 5) & 0x40000000) ? 0 : 0x20;
            file.pipe_mode = server ? u32(2) & 3 : 0;
            result = runtime_.allocate_wait_handle();
            file.object = result;
            files_.emplace(result, std::move(file));
            runtime_.handle_flags[result] = inherit ? 1 : 0;
        }
    } else if (symbol.starts_with("WaitNamedPipe")) {
        auto &thread = threads_.items.at(threads_.current);
        std::uint32_t timeout = 0;
        const auto error = NamedPipe::available(mapping_session(), text(), timeout);
        if (!error) {
            result = 1;
            thread.wait = {};
        } else if (error != 231) {
            fail(error);
            thread.wait = {};
        } else if (thread.wait.kind == ThreadWaitKind::FileIo && thread.wait.timed_out) {
            fail(121);
            thread.wait = {};
        } else {
            thread.wait = {};
            thread.wait.kind = ThreadWaitKind::FileIo;
            thread.wait.handles = {0};
            thread.wait.deadline = u32(1) == UINT32_MAX
                                       ? UINT64_MAX
                                       : runtime_.clock_ticks + std::uint64_t{u32(1) ? u32(1) : timeout} * 10000;
            thread.status = ThreadStatus::Waiting;
            schedule(state, memory);
            return std::nullopt;
        }
    } else {
        const auto file = files_.find(arg(0));
        const bool generic = symbol == "LockFile" || symbol == "UnlockFile" || symbol == "LockFileEx" ||
                             symbol == "UnlockFileEx" || symbol == "GetOverlappedResult" || symbol == "CancelIo" ||
                             symbol == "CancelIoEx";
        const bool serial_operation = symbol == "ReadFile" || symbol == "WriteFile" || symbol == "ReadFileEx" ||
                                      symbol == "WriteFileEx" || symbol == "FlushFileBuffers" ||
                                      symbol == "WaitCommEvent";
        if (file == files_.end() || (symbol == "WaitCommEvent" && !file->second.serial) ||
            (!file->second.pipe && !((file->second.serial || file->second.hid) && serial_operation) && !generic))
            fail(6);
        else if (symbol == "LockFile" || symbol == "UnlockFile") {
            if (!file->second.native)
                fail(6);
            else if (!(file->second.access & 0xc0000003U))
                fail(5);
            else {
                const auto offset = std::uint64_t{u32(1)} | (std::uint64_t{u32(2)} << 32),
                           length = std::uint64_t{u32(3)} | (std::uint64_t{u32(4)} << 32);
                std::uint32_t error=0;
                if(runtime_.network.external_io){(void)mapping_session();error=filesystem_.publish_shared_file(*file->second.native);}
                if(!error)error = file->second.native->lock_range(offset, length, true, symbol == "UnlockFile");
                if (error)
                    fail(error);
                else
                    result = 1;
            }
        } else if (symbol == "LockFileEx" || symbol == "UnlockFileEx") {
            const bool unlock = symbol == "UnlockFileEx";
            const auto overlap = arg(unlock ? 4 : 5);
            const auto previous = std::find_if(pending_file_io_.begin(), pending_file_io_.end(), [&](const auto &io) {
                return io.blocking && io.handle == arg(0) && io.thread == threads_.current && io.overlapped == overlap;
            });
            if (previous != pending_file_io_.end()) {
                if (!previous->done)
                    throw HostCallFault("檔案鎖尚未完成卻恢復執行緒");
                const auto completed = previous->result;
                pending_file_io_.erase(previous);
                threads_.items.at(threads_.current).wait = {};
                if (completed.error)
                    fail(completed.error);
                else
                    result = 1;
            } else if (!file->second.native || !overlap || u32(unlock ? 1 : 2) || (!unlock && (u32(1) & ~3U)))
                fail(87);
            else if (!(file->second.access & 0xc0000003U))
                fail(5);
            else {
                memory.check(GuestAddress{overlap}, x86_ ? 20 : 32, Access::Write);
                const auto offset = memory.load(GuestAddress{overlap + 2 * unit}, 64),
                           length = std::uint64_t{u32(unlock ? 2 : 3)} | (std::uint64_t{u32(unlock ? 3 : 4)} << 32);
                if (unlock) {
                    std::uint32_t error=0;
                    if(runtime_.network.external_io){(void)mapping_session();error=filesystem_.publish_shared_file(*file->second.native);}
                    if(!error)error = file->second.native->lock_range(offset, length, false, true);
                    if (error)
                        fail(error);
                    else
                        result = 1;
                } else {
                    PendingFileIo io;
                    io.kind = PendingFileIo::Kind::Lock;
                    io.handle = arg(0);
                    io.thread = threads_.current;
                    io.overlapped = overlap;
                    io.offset = offset;
                    io.length = length;
                    io.exclusive = (u32(1) & 2) != 0;
                    io.immediate = (u32(1) & 1) != 0;
                    io.blocking = (file->second.mode & 0x30) != 0;
                    io.event = memory.load(GuestAddress{overlap + (x86_ ? 16 : 24)}, bits);
                    const auto e = runtime_.wait_objects.find(io.event & ~UINT64_C(1));
                    if (std::any_of(pending_file_io_.begin(), pending_file_io_.end(),
                                    [&](const auto &item) { return item.overlapped == overlap; }))
                        fail(87);
                    else if ((io.event & ~UINT64_C(1)) &&
                             (e == runtime_.wait_objects.end() || e->second.kind != WaitObject::Kind::Event))
                        fail(6);
                    else {
                        memory.store(GuestAddress{overlap}, bits, 0x103);
                        memory.store(GuestAddress{overlap + unit}, bits, 0);
                        if (io.event & ~UINT64_C(1))
                            e->second.set_signal(false);
                        set_file_io_signaled(io.handle, false);
                        pending_file_io_.push_back(io);
                        poll_file_io(memory);
                        const auto current = std::find_if(pending_file_io_.begin(), pending_file_io_.end(),
                                                          [&](const auto &item) { return item.overlapped == overlap; });
                        if (current->done) {
                            const auto completed = current->result;
                            pending_file_io_.erase(current);
                            if (completed.error)
                                fail(completed.error);
                            else
                                result = 1;
                        } else if (!io.blocking) {
                            current->notify = true;
                            fail(997);
                        } else {
                            auto &thread = threads_.items.at(threads_.current);
                            thread.status = ThreadStatus::Waiting;
                            thread.wait = {};
                            thread.wait.kind = ThreadWaitKind::FileIo;
                            thread.wait.handles = {io.handle};
                            schedule(state, memory);
                            return std::nullopt;
                        }
                    }
                }
            }
        } else if (symbol == "GetNamedPipeClientProcessId" || symbol == "GetNamedPipeServerProcessId") {
            if (!arg(1))
                fail(87);
            else {
                std::uint32_t pid{};
                const auto error = file->second.pipe->process_id(symbol == "GetNamedPipeClientProcessId", pid);
                if (error)
                    fail(error);
                else {
                    memory.store(GuestAddress{arg(1)}, 32, pid);
                    result = 1;
                }
            }
        } else if (symbol == "SetNamedPipeHandleState") {
            const auto mode =
                arg(1) ? static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)}, 32)) : file->second.pipe_mode;
            if ((mode & ~3U) || ((mode & 2) && !(file->second.pipe->information().mode & 4)))
                fail(87);
            else if (arg(2) || arg(3))
                fail(50);
            else {
                file->second.pipe_mode = mode;
                result = 1;
            }
        } else if (symbol == "GetNamedPipeInfo" || symbol.starts_with("GetNamedPipeHandleState")) {
            const auto info = file->second.pipe->information();
            const bool flags = symbol == "GetNamedPipeInfo";
            const std::array values{
                flags ? info.mode | (file->second.pipe->server() ? 1U : 0U) : file->second.pipe_mode,
                flags ? info.output_size : info.instances, flags ? info.input_size : 0U, flags ? info.maximum : 0U};
            if (!flags && arg(5))
                fail(50);
            else {
                for (unsigned i = 1; i < 5; ++i)
                    if (arg(i))
                        memory.check(GuestAddress{arg(i)}, 4, Access::Write);
                for (unsigned i = 1; i < 5; ++i)
                    if (arg(i))
                        memory.store(GuestAddress{arg(i)}, 32, values[i - 1]);
                result = 1;
            }
        } else if (symbol == "PeekNamedPipe") {
            if (u32(2) > output_limit)
                fail(87);
            else {
                for (unsigned i = 3; i < 6; ++i)
                    if (arg(i))
                        memory.check(GuestAddress{arg(i)}, 4, Access::Write);
                const auto count = arg(1) ? u32(2) : 0;
                memory.check(GuestAddress{arg(1)}, count, Access::Write);
                std::vector<std::uint8_t> data(count);
                std::uint32_t available{}, remaining{};
                const auto io = file->second.pipe->read(data, (file->second.pipe->information().mode & 4) != 0, true,
                                                        &available, &remaining);
                process_io::record(process_io::Kind::Other, io.bytes);
                if (io.error)
                    fail(io.error);
                else {
                    memory.write(GuestAddress{arg(1)}, std::span(data).first(io.bytes));
                    if (arg(3))
                        memory.store(GuestAddress{arg(3)}, 32, io.bytes);
                    if (arg(4))
                        memory.store(GuestAddress{arg(4)}, 32, available);
                    if (arg(5))
                        memory.store(GuestAddress{arg(5)}, 32, remaining);
                    result = 1;
                }
            }
        } else if (symbol == "DisconnectNamedPipe") {
            const auto error = file->second.pipe->disconnect();
            if (error)
                fail(error);
            else {
                cancel_file_io(memory, arg(0));
                result = 1;
            }
        } else if (symbol == "CancelIo" || symbol == "CancelIoEx") {
            const bool extended = symbol == "CancelIoEx";
            const auto present = std::any_of(pending_file_io_.begin(), pending_file_io_.end(), [&](const auto &io) {
                return io.handle == arg(0) && !io.done &&
                       (extended ? (!arg(1) || arg(1) == io.overlapped) : io.thread == threads_.current);
            });
            if (!present && extended)
                fail(1168);
            else {
                cancel_file_io(memory, arg(0), extended ? 0 : threads_.current, extended ? arg(1) : 0);
                result = 1;
            }
        } else if (symbol == "ConnectNamedPipe" || symbol == "ReadFile" || symbol == "WriteFile" ||
                   symbol == "ReadFileEx" || symbol == "WriteFileEx" || symbol == "TransactNamedPipe" ||
                   symbol == "FlushFileBuffers" || symbol == "WaitCommEvent") {
            const auto previous = std::find_if(pending_file_io_.begin(), pending_file_io_.end(), [&](const auto &io) {
                return (!io.overlapped || ((file->second.serial || file->second.hid) && io.blocking)) &&
                       io.thread == threads_.current && io.handle == arg(0);
            });
            if (previous != pending_file_io_.end()) {
                if (!previous->done)
                    throw HostCallFault("管道尚未完成卻恢復執行緒");
                const auto completed = previous->result;
                pending_file_io_.erase(previous);
                threads_.items.at(threads_.current).wait = {};
                if (completed.error)
                    fail(completed.error);
                else
                    result = 1;
            } else {
                PendingFileIo io;
                io.handle = arg(0);
                io.thread = threads_.current;
                io.hid = file->second.hid != nullptr;
                if (symbol == "ConnectNamedPipe") {
                    io.kind = PendingFileIo::Kind::Connect;
                    io.overlapped = arg(1);
                } else if (symbol == "FlushFileBuffers")
                    io.kind = PendingFileIo::Kind::Flush;
                else if (symbol == "WaitCommEvent") {
                    io.kind = PendingFileIo::Kind::CommEvent;
                    io.output = arg(1);
                    io.output_size = 4;
                    io.overlapped = arg(2);
                } else if (symbol == "TransactNamedPipe") {
                    io.kind = PendingFileIo::Kind::Transact;
                    io.input = arg(1);
                    io.input_size = u32(2);
                    io.output = arg(3);
                    io.output_size = u32(4);
                    io.count_pointer = arg(5);
                    io.overlapped = arg(6);
                } else {
                    const bool extended = symbol.ends_with("Ex"), writing = symbol.starts_with("Write");
                    io.kind = writing ? PendingFileIo::Kind::Write : PendingFileIo::Kind::Read;
                    io.count_pointer = extended ? 0 : arg(3);
                    io.overlapped = arg(extended ? 3 : 4);
                    io.routine = extended ? arg(4) : 0;
                    if (writing) {
                        io.input = arg(1);
                        io.input_size = u32(2);
                    } else {
                        io.output = arg(1);
                        io.output_size = u32(2);
                    }
                }
                io.immediate = (file->second.pipe_mode & 1) && io.kind != PendingFileIo::Kind::Flush;
                const bool reading = io.kind == PendingFileIo::Kind::Read || io.kind == PendingFileIo::Kind::Transact,
                           writing = io.kind == PendingFileIo::Kind::Write || io.kind == PendingFileIo::Kind::Transact;
                std::uint32_t error = 0;
                if (file->second.serial && (io.kind == PendingFileIo::Kind::CommEvent && !io.output))
                    error = 87;
                else if (file->second.serial && io.kind == PendingFileIo::Kind::CommEvent &&
                         std::any_of(pending_file_io_.begin(), pending_file_io_.end(), [&](const auto &other) {
                             return !other.done && other.kind == PendingFileIo::Kind::CommEvent &&
                                    files_.contains(other.handle) &&
                                    files_.at(other.handle).object == file->second.object;
                         }))
                    error = 87;
                else if ((file->second.serial || file->second.hid) && !(file->second.mode & 0x30) && !io.overlapped &&
                         io.kind != PendingFileIo::Kind::Flush)
                    error = 87;
                else if (symbol.ends_with("Ex") &&
                         (!io.routine || !io.overlapped || (file->second.mode & 0x30) || file->second.completion_port))
                    error = 87;
                else if (io.routine &&
                         (threads_.items.at(threads_.current).apcs.size() +
                              static_cast<std::size_t>(std::count_if(
                                  pending_file_io_.begin(), pending_file_io_.end(),
                                  [&](const auto &p) { return p.thread == threads_.current && p.routine; })) >=
                          1024))
                    error = 8;
                else if ((reading && !(file->second.access & 0x80000001)) ||
                         (writing && !(file->second.access & 0x40000002)))
                    error = 5;
                else if (io.kind == PendingFileIo::Kind::Transact && !(file->second.pipe_mode & 2))
                    error = 87;
                else if ((reading || writing) && !io.overlapped && !io.count_pointer)
                    error = 87;
                else if (io.input_size > output_limit || io.output_size > output_limit)
                    error = 87;
                else if (io.overlapped && std::any_of(pending_file_io_.begin(), pending_file_io_.end(),
                                                      [&](const auto &p) { return p.overlapped == io.overlapped; }))
                    error = 87;
                if (!error && io.overlapped) {
                    memory.check(GuestAddress{io.overlapped}, x86_ ? 20 : 32, Access::Write);
                    io.event = io.routine ? 0 : memory.load(GuestAddress{io.overlapped + (x86_ ? 16 : 24)}, bits);
                    const auto e = runtime_.wait_objects.find(io.event & ~UINT64_C(1));
                    if ((io.event & ~UINT64_C(1)) &&
                        (e == runtime_.wait_objects.end() || e->second.kind != WaitObject::Kind::Event))
                        error = 6;
                    else if (file->second.serial && io.kind == PendingFileIo::Kind::CommEvent &&
                             !(file->second.mode & 0x30) &&
                             (!(io.event & ~UINT64_C(1)) || e == runtime_.wait_objects.end() ||
                              !e->second.manual_reset))
                        error = 87;
                }
                if (!error && io.kind == PendingFileIo::Kind::Connect) {
                    const auto connected = file->second.pipe->connect();
                    if (!connected.pending)
                        error = connected.error ? connected.error : 535;
                }
                if (error)
                    fail(error);
                else {
                    if (io.count_pointer) {
                        memory.check(GuestAddress{io.count_pointer}, 4, Access::Write);
                        memory.store(GuestAddress{io.count_pointer}, 32, 0);
                    }
                    memory.check(GuestAddress{io.input}, io.input_size, Access::Read);
                    memory.check(GuestAddress{io.output}, io.output_size, Access::Write);
                    if (io.hid)
                        io.blocking =
                            !io.overlapped || (file->second.mode & 0x30) || io.kind == PendingFileIo::Kind::Flush;
                    if (file->second.serial) {
                        std::vector<std::uint8_t> data(io.kind == PendingFileIo::Kind::Read ? io.output_size
                                                                                            : io.input_size);
                        if (io.input_size)
                            memory.read(GuestAddress{io.input}, data);
                        io.serial = file->second.serial->transfer(
                            io.kind == PendingFileIo::Kind::Read        ? SerialTransfer::Kind::Read
                            : io.kind == PendingFileIo::Kind::Write     ? SerialTransfer::Kind::Write
                            : io.kind == PendingFileIo::Kind::CommEvent ? SerialTransfer::Kind::Event
                                                                        : SerialTransfer::Kind::Flush,
                            std::move(data));
                        io.blocking =
                            !io.overlapped || (file->second.mode & 0x30) || io.kind == PendingFileIo::Kind::Flush;
                    }
                    if (io.overlapped) {
                        memory.store(GuestAddress{io.overlapped}, bits, 0x103);
                        memory.store(GuestAddress{io.overlapped + unit}, bits, 0);
                        if (io.event & ~UINT64_C(1))
                            runtime_.wait_objects.at(io.event & ~UINT64_C(1)).set_signal(false);
                    }
                    set_file_io_signaled(io.handle, false);
                    pending_file_io_.push_back(io);
                    poll_file_io(memory);
                    const auto current =
                        std::find_if(pending_file_io_.begin(), pending_file_io_.end(), [&](const auto &p) {
                            return p.handle == io.handle && p.thread == io.thread && p.overlapped == io.overlapped;
                        });
                    if (current->done) {
                        const auto completed = current->result;
                        pending_file_io_.erase(current);
                        if (completed.error && !(io.routine && completed.error == 234))
                            fail(completed.error);
                        else {
                            result = 1;
                            if (completed.error)
                                last_error_ = completed.error;
                        }
                    } else if (io.overlapped && !io.blocking) {
                        current->notify = true;
                        if (io.routine)
                            result = 1;
                        else
                            fail(997);
                    } else {
                        auto &thread = threads_.items.at(threads_.current);
                        thread.status = ThreadStatus::Waiting;
                        thread.wait = {};
                        thread.wait.kind = ThreadWaitKind::FileIo;
                        thread.wait.handles = {io.handle};
                        schedule(state, memory);
                        return std::nullopt;
                    }
                }
            }
        } else
            fail(50);
    }
    frame.return_to(state, result);
    return std::nullopt;
}
} // namespace x64arm

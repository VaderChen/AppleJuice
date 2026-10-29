// Application Restart 登錄繫結程序出生身分，與程序 handle 共用同一個鎖及生命週期。
if (symbol == "RegisterApplicationRestart" || symbol == "UnregisterApplicationRestart" ||
    symbol == "GetApplicationRestartSettings") {
    const auto saved_error = last_error_;
    const auto finish = [&](std::uint32_t status) -> std::optional<std::uint32_t> {
        last_error_ = saved_error;
        call.return_to(state, status);
        return std::nullopt;
    };
    try {
#if !defined(_WIN32)
        if (symbol != "GetApplicationRestartSettings") {
            ProcessRecord::Header::RestartSettings settings;
            if (symbol == "RegisterApplicationRestart") {
                settings.version = 1;
                settings.flags = static_cast<std::uint32_t>(arg(1));
                if (settings.flags & ~15U)
                    return finish(0x80070057U);
                if (arg(0)) {
                    for (;;) {
                        const auto offset = std::uint64_t{settings.length} * 2;
                        if (arg(0) > UINT64_MAX - offset)
                            return finish(0x80070057U);
                        const auto character = memory.load(GuestAddress{arg(0) + offset}, 16);
                        if (!character)
                            break;
                        if (settings.length == 1024)
                            return finish(0x80070057U);
                        settings.command_line[settings.length++] = static_cast<char16_t>(character);
                    }
                }
            }
            if (!processes_)
                processes_ = std::make_shared<ProcessState>();
            configure_process_identity();
            SharedEventLock lock(processes_->record->descriptor);
            processes_->record->data->restart = settings;
            return finish(0);
        }
        if (!arg(2))
            return finish(0x80070057U);
        memory.check(GuestAddress{arg(2)}, 4, Access::Write);
        const auto capacity = static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)}, 32));
        if (!arg(1) && capacity)
            return finish(0x80070057U);
        if (arg(3))
            memory.check(GuestAddress{arg(3)}, 4, Access::Write);
        if (process_object_type(arg(0)) != u"Process")
            return finish(0x80070006U);
        const auto pid = process_handle_pid(arg(0), 0x10U);
        if (!pid)
            return finish(0x80070005U);
        std::shared_ptr<ProcessRecord> record;
        if (current_process_handle(arg(0))) {
            if (processes_)
                record = processes_->record;
        } else {
            // 沿用 handle 已保留的出生身分；不得以同 PID 的新程序取代舊紀錄。
            auto &object = processes_->handles.at(arg(0));
            record = object.record;
            if (!record && object.identity) {
                record = ProcessRecord::open(mapping_session(), pid, false);
                if (record && record->data->birth != object.identity)
                    record.reset();
            }
        }
        if (!record || !record->alive())
            return finish(0x80070490U);
        ProcessRecord::Header::RestartSettings settings;
        {
            SharedEventLock lock(record->descriptor);
            settings = record->data->restart;
        }
        if (!settings.version)
            return finish(0x80070490U);
        if (settings.version != 1 || settings.length > 1024 || (settings.flags & ~15U) ||
            settings.command_line[settings.length])
            return finish(0x80004005U);
        const auto required = settings.length + 1;
        if (arg(1) && capacity >= required)
            memory.check(GuestAddress{arg(1)}, std::uint64_t{required} * 2, Access::Write);
        memory.store(GuestAddress{arg(2)}, 32, required);
        if (arg(3))
            memory.store(GuestAddress{arg(3)}, 32, settings.flags);
        if (!arg(1))
            return finish(0);
        if (capacity < required)
            return finish(0x8007007aU);
        for (std::uint32_t i = 0; i < required; ++i)
            memory.store(GuestAddress{arg(1) + std::uint64_t{i} * 2}, 16, settings.command_line[i]);
        return finish(0);
#else
        return finish(0x80004001U);
#endif
    } catch (const MemoryFault &) {
        return finish(0x80070057U);
    } catch (const std::bad_alloc &) {
        return finish(0x8007000eU);
    } catch (const SecurityDescriptorError &) {
        return finish(0x80004005U);
    } catch (const HostCallFault &) {
        return finish(0x80004005U);
    }
}

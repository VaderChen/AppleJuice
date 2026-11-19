// Application Recovery（由 windows_process.cpp 引入）：登記存在程序紀錄的 recovery 欄位（與 Application Restart 同一個鎖），其他程序可經 handle 查詢。
// 沙箱的 Windows 錯誤報告已依原則停用（wer.dll 的 WerReportSubmit 回 WerDisabled），程序當掉時不會啟動復原回呼；
// 因此 ApplicationRecoveryInProgress 在沒有進行中的復原時回 E_FAIL，ApplicationRecoveryFinished 沒有作用。
if (symbol == "RegisterApplicationRecoveryCallback" || symbol == "UnregisterApplicationRecoveryCallback" ||
    symbol == "GetApplicationRecoveryCallback" || symbol == "ApplicationRecoveryInProgress" || symbol == "ApplicationRecoveryFinished") {
    const auto saved_error = last_error_;
    const auto finish = [&](std::uint64_t status) -> std::optional<std::uint32_t> {
        last_error_ = saved_error;
        call.return_to(state, status);
        return std::nullopt;
    };
    try {
#if !defined(_WIN32)
        if (symbol == "ApplicationRecoveryFinished")
            return finish(0);
        if (symbol == "ApplicationRecoveryInProgress") {
            if (!arg(0))
                return finish(0x80070057U);
            memory.check(GuestAddress{arg(0)}, 4, Access::Write);
            return finish(0x80004005U);
        }
        if (symbol != "GetApplicationRecoveryCallback") {
            // (pRecoveyCallback, pvParameter, dwPingInterval, dwFlags)：間隔 0 為預設 5 秒，上限 RECOVERY_MAX_PING_INTERVAL（5 分鐘），旗標必須為 0。
            ProcessRecord::Header::RecoverySettings settings;
            if (symbol == "RegisterApplicationRecoveryCallback") {
                const auto ping = static_cast<std::uint32_t>(arg(2)), flags = static_cast<std::uint32_t>(arg(3));
                if (!arg(0) || ping > 5 * 60 * 1000 || flags)
                    return finish(0x80070057U);
                settings.version = 1;
                settings.ping = ping ? ping : 5000;
                settings.flags = flags;
                settings.callback = arg(0);
                settings.parameter = arg(1);
            }
            if (!processes_)
                processes_ = std::make_shared<ProcessState>();
            configure_process_identity();
            SharedEventLock lock(processes_->record->descriptor);
            processes_->record->data->recovery = settings;
            return finish(0);
        }
        // (hProcess, pRecoveryCallback, ppvParameter, pdwPingInterval, pdwFlags)：參數都要可寫入。
        for (unsigned i = 1; i < 5; ++i)
            if (!arg(i))
                return finish(0x80070057U);
        memory.check(GuestAddress{arg(1)}, x86_ ? 4 : 8, Access::Write);
        memory.check(GuestAddress{arg(2)}, x86_ ? 4 : 8, Access::Write);
        memory.check(GuestAddress{arg(3)}, 4, Access::Write);
        memory.check(GuestAddress{arg(4)}, 4, Access::Write);
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
        ProcessRecord::Header::RecoverySettings settings;
        {
            SharedEventLock lock(record->descriptor);
            settings = record->data->recovery;
        }
        if (!settings.version)
            return finish(0x80070490U);
        if (settings.version != 1 || settings.flags || !settings.ping || settings.ping > 5 * 60 * 1000)
            return finish(0x80004005U);
        memory.store(GuestAddress{arg(1)}, x86_ ? 32 : 64, settings.callback);
        memory.store(GuestAddress{arg(2)}, x86_ ? 32 : 64, settings.parameter);
        memory.store(GuestAddress{arg(3)}, 32, settings.ping);
        memory.store(GuestAddress{arg(4)}, 32, settings.flags);
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

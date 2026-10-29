// Native 與 Win32 共用物件、權限、出生身分及完整 DWORD 結束狀態。
if (symbol == "TerminateProcess" || symbol == "NtTerminateProcess" || symbol == "ZwTerminateProcess") {
    const bool native = symbol != "TerminateProcess";
    const auto saved_error = last_error_;
    const auto finish = [&](std::uint32_t status, std::uint32_t error) -> std::optional<std::uint32_t> {
        last_error_ = native || !status ? saved_error : error;
        call.return_to(state, native ? status : status ? 0 : 1);
        return std::nullopt;
    };
    const auto code = static_cast<std::uint32_t>(arg(1));
    const auto target = native && !arg(0) ? (x86_ ? UINT32_MAX : UINT64_MAX) : arg(0);
    const auto type = object_type(target);
    if (type != u"Process")
        return finish(type.empty() ? 0xc0000008U : 0xc0000024U, 6);
    if (!process_handle_pid(target, 1))
        return finish(0xc0000022U, 5);
    if (current_process_handle(target)) {
        if (native && !arg(0)) {
            bool changed = false;
            for (auto& [id, thread] : threads_.items)
                if (id != threads_.current)
                    changed |= detail::prepare_forced_thread_stop(thread, code, GuestAddress{callback_return_.value + 0xa0});
            if (changed) {
                notify_concurrency_changed();
                threads_.yield = true;
            }
            return finish(0, 0);
        }
        // 強制退出不呼叫 FLS、TLS 或 DLL_PROCESS_DETACH。
        if (code)
            report_crt_fast_fail(state, memory, native ? "NtTerminateProcess" : "TerminateProcess");
        record_job_exit(code);
        return code;
    }
#if !defined(_WIN32)
    const auto found = processes_->handles.find(target);
    const auto object = found->second;
    if (object.child)
        object.child->poll();
    const auto record = object.record;
    if (!record)
        return finish(0xc0000008U, 6);
    SharedEventLock lock(record->descriptor);
    if ((object.child && object.child->exited) || !record->alive())
        return finish(0xc000010aU, 5);
    if ((object.identity && object.identity != record->data->birth) ||
        record->data->pid != process_handle_pid(target, 0))
        return finish(0xc0000008U, 6);
    if (trace_process_protocol())
        (void)std::fprintf(stderr, "process_terminate parent=%d target=%u remote=%u code=%u\n", ::getpid(),
                           record->data->pid, object.remote_pid ? 1U : 0U, code);
    if (::kill(static_cast<pid_t>(record->data->pid), SIGKILL))
        return finish(errno == ESRCH ? 0xc000010aU : 0xc0000022U, 5);
    if (object.child) {
        object.child->terminated = object.child->have_result = object.child->success = true;
        object.child->exit_code = code;
    }
    record->finish(code);
    return finish(0, 0);
#else
    return finish(0xc00000bbU, 50);
#endif
}

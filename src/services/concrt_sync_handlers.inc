// Concrt 物件保留 v14 的儲存區大小；私有欄位採共用 SRW／CV 字組，沒有第二套等待佇列。
if (service.family == "concrt140.dll") {
    const auto self = x86_ ? state.reg(Gpr::Rcx) & UINT32_MAX : call(0);
    const auto argument = [&](unsigned index) { return call(index + (x86_ ? 0U : 1U)); };
    const bool condition = name.find("@_Condition_variable@") != name.npos;
    const bool scoped = name.find("scoped_lock@") != name.npos;
    const bool constructor = name.starts_with("??0"), destructor = name.starts_with("??1");
    const auto size = condition ? (x86_ ? 36U : 64U) : scoped ? (x86_ ? 28U : 48U) : (x86_ ? 32U : 56U);
    memory.check(GuestAddress{self}, size, Access::Write);
    op.arguments[0] = self; // 將 thiscall 與 x64 呼叫正規化，供跨排程的 NativeOperation 續接使用。
    const auto has_waiters = [&](std::uint64_t address, bool cv) {
        return std::any_of(threads_.items.begin(), threads_.items.end(), [&](const auto& item) {
            const auto& thread = item.second; const auto& wait = thread.wait;
            if (thread.status != ThreadStatus::Waiting) return false;
            if (cv) return wait.kind == ThreadWaitKind::Condition && wait.handles[0] == address;
            return (wait.kind == ThreadWaitKind::Slim && wait.handles[0] == address + unit) ||
                (wait.kind == ThreadWaitKind::Condition && !wait.critical && wait.handles[1] == address + unit);
        });
    };
    if (!scoped && (constructor || destructor)) {
        if (has_waiters(self, condition)) throw HostCallFault("不可重設或銷毀等待中的 Concrt 同步物件");
        if (destructor && !condition && (memory.load(GuestAddress{self}, bits) ||
            (memory.load(GuestAddress{self + unit}, bits) & ~UINT64_C(2))))
            throw HostCallFault("不可銷毀持有中的 Concrt critical_section");
        memory.write(GuestAddress{self}, std::vector<std::uint8_t>(size));
        return finish(constructor ? self : 0);
    }
    if (condition) {
        if (name.starts_with("?notify_"))
            return start("kernel32.dll", name.starts_with("?notify_one@") ? "WakeConditionVariable" : "WakeAllConditionVariable", {self});
        const auto section = argument(0);
        memory.check(GuestAddress{section}, x86_ ? 32U : 56U, Access::Write);
        if (memory.load(GuestAddress{section}, bits) != threads_.current)
            throw HostCallFault("Concrt 條件變數等待需要目前 context 持有鎖");
        op.arguments[1] = section;
        const auto timeout = name.starts_with("?wait_for@") ? static_cast<std::uint32_t>(argument(1)) : UINT32_MAX;
        memory.store(GuestAddress{section}, bits, 0);
        // Win32 共用服務原子地釋放 SRW 並登記等待；通知／逾時後都先重新取得鎖才完成續接。
        return start("kernel32.dll", "SleepConditionVariableSRW", {self, section + unit, timeout, 0});
    }
    const auto section = scoped ? (constructor ? argument(0) : memory.load(GuestAddress{self}, bits)) : self;
    memory.check(GuestAddress{section}, x86_ ? 32U : 56U, Access::Write);
    const auto owner = memory.load(GuestAddress{section}, bits);
    if (name.starts_with("?native_handle@")) return finish(section);
    if ((scoped && destructor) || name.starts_with("?unlock@")) {
        if (owner != threads_.current) throw HostCallFault("目前 context 未持有 Concrt critical_section");
        memory.store(GuestAddress{section}, bits, 0);
        return start("kernel32.dll", "ReleaseSRWLockExclusive", {section + unit});
    }
    if (owner == threads_.current) {
        if (name.starts_with("?try_lock@")) return finish(0);
        throw_msvc_exception(state, memory, service, MsvcExceptionKind::ImproperLock);
        return std::nullopt;
    }
    if (scoped) {
        memory.write(GuestAddress{self}, std::vector<std::uint8_t>(size));
        memory.store(GuestAddress{self}, bits, section); op.arguments[1] = section;
    }
    if (name.starts_with("?try_lock_for@"))
        return start("internal-sync", "TryAcquireSRWLockExclusiveFor", {section + unit, static_cast<std::uint32_t>(argument(0))});
    return start("kernel32.dll", name.starts_with("?try_lock@") ? "TryAcquireSRWLockExclusive" : "AcquireSRWLockExclusive", {section + unit});
}

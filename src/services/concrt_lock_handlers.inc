// Concurrency 例外類別的建構與 get_error_code：與 std::exception 共用訊息與虛擬表（invoke_rtti）。
if (service.family == "concrt140.dll" && (name.starts_with("??0") || name.starts_with("?get_error_code@")) &&
    (name.find("improper_") != name.npos || name.find("invalid_") != name.npos || name.find("missing_wait@") != name.npos || name.find("scheduler_resource_allocation_error@") != name.npos))
    return invoke_rtti(state, memory, service);
// SchedulerPolicy：[0] 指向 CRT heap 上 10 個 unsigned 的政策值袋（PolicyElementKey 0～9）。驗證規則與違規時的 Concurrency 例外同 ConcRT 文件。
if (service.family == "concrt140.dll" && (name.find("SchedulerPolicy@Concurrency@@") != name.npos || name.find("DefaultSchedulerPolicy@Scheduler@") != name.npos)) {
    constexpr unsigned keys = 10, max_resources = UINT32_MAX;
    constexpr std::array<std::uint32_t, keys> defaults{0, max_resources, 1, 1, 8, 0, 0, 0, 1, 1};
    // x86 的變參建構子是 cdecl 成員：this 在堆疊第一格，由呼叫端清理。
    const bool cdecl_member = name.starts_with("??0SchedulerPolicy@Concurrency@@QAA@"), member = name.find("@@SA") == name.npos && !cdecl_member;
    const auto self = cdecl_member ? call(0) : member ? (x86_ ? state.reg(Gpr::Rcx) & UINT32_MAX : call(0)) : 0;
    const auto argument = [&](unsigned index) { return call(index + ((member && !x86_) || cdecl_member ? 1U : 0U)); };
    const auto fail = [&](MsvcExceptionKind kind) { throw_msvc_exception(state, memory, service, kind); return std::optional<std::uint32_t>{}; };
    const auto valid = [&](std::uint32_t key, std::uint32_t value) {
        switch (key) {
        case 0: return value <= 1;                                  // ThreadScheduler；UmsThreadDefault 由 ThreadScheduler 承接
        case 1: return value >= 1;                                  // MaxConcurrency（含 MaxExecutionResources）
        case 3: return value >= 1;                                  // TargetOversubscriptionFactor
        case 6: return (value >= static_cast<std::uint32_t>(-15) || value <= 15U) || value == 0xF000U; // THREAD_PRIORITY_* 或 INHERIT_THREAD_PRIORITY
        case 7: case 8: case 9: return value <= 1;
        default: return true;
        }
    };
    const auto bag_of = [&](std::uint64_t policy) { const auto bag = memory.load(GuestAddress{policy}, bits); if (!bag) throw HostCallFault("SchedulerPolicy 尚未建構"); return bag; };
    const auto read_bag = [&](std::uint64_t bag) { std::array<std::uint32_t, keys> values{}; for (unsigned i = 0; i < keys; ++i) values[i] = static_cast<std::uint32_t>(memory.load(GuestAddress{bag + i * 4}, 32)); return values; };
    const auto write_bag = [&](std::uint64_t bag, const std::array<std::uint32_t, keys>& values) { for (unsigned i = 0; i < keys; ++i) memory.store(GuestAddress{bag + i * 4}, 32, values[i]); };
    const auto concurrency_ok = [&](std::uint32_t minimum, std::uint32_t maximum) { return maximum == max_resources || minimum <= maximum; };
    if (name.starts_with("?ResetDefaultSchedulerPolicy@")) { concrt_default_policy_.reset(); return finish(0); }
    if (name.starts_with("?SetDefaultSchedulerPolicy@")) { concrt_default_policy_ = read_bag(bag_of(call(0))); return finish(0); }
    if (name.starts_with("??0") || name.starts_with("??4")) {
        std::array<std::uint32_t, keys> values = defaults;
        const bool copy = name.find("AEBV01@@Z") != name.npos || name.find("ABV01@@Z") != name.npos, variadic = name.ends_with("ZZ");
        if (copy) values = read_bag(bag_of(argument(0)));
        if (variadic) {
            // (count, key0, value0, key1, value1, …)：每一對都要是合法鍵與值，最後再檢查並行數上下限。
            const auto count = argument(0); if (count > keys * 4) return fail(MsvcExceptionKind::InvalidSchedulerPolicyKey);
            for (std::uint64_t i = 0; i < count; ++i) {
                const auto key = static_cast<std::uint32_t>(argument(1 + static_cast<unsigned>(i) * 2)), value = static_cast<std::uint32_t>(argument(2 + static_cast<unsigned>(i) * 2));
                if (key >= keys) return fail(MsvcExceptionKind::InvalidSchedulerPolicyKey);
                if (!valid(key, value)) return fail(MsvcExceptionKind::InvalidSchedulerPolicyValue);
                values[key] = value;
            }
            if (!concurrency_ok(values[2], values[1])) return fail(MsvcExceptionKind::InvalidSchedulerPolicyThreadSpecification);
        }
        memory.check(GuestAddress{self}, unit, Access::Write);
        auto bag = name.starts_with("??4") ? bag_of(self) : 0;
        if (!bag) { bag = allocate_heap(memory, keys * 4); if (!bag) return fail(MsvcExceptionKind::BadAlloc); memory.store(GuestAddress{self}, bits, bag); }
        write_bag(bag, values); return finish(self);
    }
    if (name.starts_with("??1")) { if (const auto bag = memory.load(GuestAddress{self}, bits)) (void)free_heap(memory, bag, state); memory.store(GuestAddress{self}, bits, 0); return finish(0); }
    const auto bag = bag_of(self); auto values = read_bag(bag);
    if (name.starts_with("?GetPolicyValue@")) { const auto key = static_cast<std::uint32_t>(argument(0)); if (key >= keys) return fail(MsvcExceptionKind::InvalidSchedulerPolicyKey); return finish(values[key]); }
    if (name.starts_with("?SetPolicyValue@")) {
        // MinConcurrency／MaxConcurrency 只能經 SetConcurrencyLimits 設定。
        const auto key = static_cast<std::uint32_t>(argument(0)), value = static_cast<std::uint32_t>(argument(1));
        if (key >= keys || key == 1 || key == 2) return fail(MsvcExceptionKind::InvalidSchedulerPolicyKey);
        if (!valid(key, value)) return fail(MsvcExceptionKind::InvalidSchedulerPolicyValue);
        const auto previous = values[key]; values[key] = value; write_bag(bag, values); return finish(previous);
    }
    // SetConcurrencyLimits(min, max)
    const auto minimum = static_cast<std::uint32_t>(argument(0)), maximum = static_cast<std::uint32_t>(argument(1));
    if (!maximum) return fail(MsvcExceptionKind::InvalidSchedulerPolicyValue);
    if (!concurrency_ok(minimum, maximum)) return fail(MsvcExceptionKind::InvalidSchedulerPolicyThreadSpecification);
    values[2] = minimum; values[1] = maximum; write_bag(bag, values); return finish(0);
}
// Concrt 鎖與輔助（concrt_lock_descriptors.inc）：與 critical_section 相同沿用 kernel32 的 SRW、臨界區段、事件與等待服務，
// 經 NativeOperation 續接（resume_nt_native）記錄擁有者後才完成呼叫；不另建等待佇列。
if (service.family == "concrt140.dll" && (name.find("reader_writer_lock@Concurrency@@") != name.npos || (name.find("@event@Concurrency@@") != name.npos || name.starts_with("??0event@Concurrency@@") || name.starts_with("??1event@Concurrency@@")) ||
    name.find("BlockingLock@details@Concurrency@@") != name.npos || name.find("PPLLock@details@Concurrency@@") != name.npos ||
    name.find("_SpinWait@") != name.npos || name.find("_ReaderWriterLock@details@Concurrency@@") != name.npos || name.find("@_SpinCount@") != name.npos ||
    name.starts_with("?Yield@Context@") || name.starts_with("?_SpinYield@Context@") || name.starts_with("?_Yield@_Context@") ||
    name.starts_with("?Alloc@Concurrency@@") || name.starts_with("?Free@Concurrency@@") || name == "?wait@Concurrency@@YAXI@Z" || name.starts_with("?_GetConcurrency@"))) {
    const bool member = name.find("@@SA") == name.npos && name.find("@@YA") == name.npos;
    const auto self = member ? (x86_ ? state.reg(Gpr::Rcx) & UINT32_MAX : call(0)) : 0;
    const auto argument = [&](unsigned index) { return call(index + (member && !x86_ ? 1U : 0U)); };
    if (member) op.arguments[0] = self; // 將 thiscall 與 x64 呼叫正規化，供續接使用；其餘需要的值另存於 arguments[1]。
    const auto load = [&](std::uint64_t at) { return memory.load(GuestAddress{at}, bits); };
    const auto store = [&](std::uint64_t at, std::uint64_t value) { memory.store(GuestAddress{at}, bits, value); };
    const auto clear = [&](std::uint64_t at, unsigned size) { memory.check(GuestAddress{at}, size, Access::Write); memory.write(GuestAddress{at}, std::vector<std::uint8_t>(size)); };
    const auto guest = [&](std::uint64_t target) {
        const auto id = next_native_operation_++; native_operations_.emplace(id, op);
        start_callbacks(state, memory, {{target, {}}}, Continuation::NativeOperation, id); return std::optional<std::uint32_t>{};
    };
    const bool constructor = name.starts_with("??0"), destructor = name.starts_with("??1");
    // ---- reader_writer_lock：[0] 寫入者 context、[unit] SRW 字組、[2*unit] 讀取者數 ----
    if (name.find("reader_writer_lock@Concurrency@@") != name.npos) {
        const bool scoped = name.find("scoped_lock") != name.npos, shared = name.find("scoped_lock_read@") != name.npos;
        const unsigned size = x86_ ? 40U : 72U;
        if (!scoped && constructor) { clear(self, size); return finish(self); }
        const auto lock = scoped ? (constructor ? argument(0) : load(self)) : self;
        memory.check(GuestAddress{lock}, size, Access::Write);
        const auto owner = load(lock); const auto readers = static_cast<std::uint32_t>(memory.load(GuestAddress{lock + 2 * unit}, 32));
        if (!scoped && destructor) {
            if (owner || readers || load(lock + unit)) throw HostCallFault("不可銷毀持有中的 Concrt reader_writer_lock");
            clear(lock, size); return finish(0);
        }
        if (scoped && constructor) { memory.check(GuestAddress{self}, unit, Access::Write); store(self, lock); }
        op.arguments[1] = lock;
        if (name.starts_with("?unlock@") || (scoped && destructor)) {
            // unlock 釋放目前 context 持有的寫入鎖，否則釋放一個讀取者。
            if (owner == threads_.current && !shared) { store(lock, 0); return start("kernel32.dll", "ReleaseSRWLockExclusive", {lock + unit}); }
            if (!readers) throw HostCallFault("目前 context 未持有 Concrt reader_writer_lock");
            memory.store(GuestAddress{lock + 2 * unit}, 32, readers - 1);
            return start("kernel32.dll", "ReleaseSRWLockShared", {lock + unit});
        }
        const bool read = shared || name.starts_with("?lock_read@") || name.starts_with("?try_lock_read@"), trying = name.starts_with("?try_lock");
        if (owner == threads_.current) {
            if (trying) return finish(0);
            throw_msvc_exception(state, memory, service, MsvcExceptionKind::ImproperLock); return std::nullopt;
        }
        return start("kernel32.dll", read ? (trying ? "TryAcquireSRWLockShared" : "AcquireSRWLockShared") : (trying ? "TryAcquireSRWLockExclusive" : "AcquireSRWLockExclusive"), {lock + unit});
    }
    // ---- event：手動重設事件，[0] 為 kernel32 事件控制代碼；等待逾時回 COOPERATIVE_WAIT_TIMEOUT ----
    if ((name.find("@event@Concurrency@@") != name.npos || name.starts_with("??0event@Concurrency@@") || name.starts_with("??1event@Concurrency@@"))) {
        if (name.starts_with("?wait_for_multiple@")) {
            const auto events = call(0), count = call(1); const bool all = (call(2) & 0xffU) != 0; const auto timeout = static_cast<std::uint32_t>(call(3));
            if (!count) return finish(0);
            if (count > 64) throw HostCallFault("Concrt event::wait_for_multiple 一次最多等待 64 個 event");
            memory.check(GuestAddress{events}, count * unit, Access::Read);
            const auto handles = scratch(count * unit);
            for (std::uint64_t i = 0; i < count; ++i) {
                const auto item = load(events + i * unit); memory.check(GuestAddress{item}, unit, Access::Read);
                const auto handle = load(item); if (!handle) throw HostCallFault("Concrt event 尚未建構");
                store(handles + i * unit, handle);
            }
            op.arguments[1] = count;
            return start("kernel32.dll", "WaitForMultipleObjects", {count, handles, all ? 1U : 0U, timeout});
        }
        const unsigned size = x86_ ? 40U : 72U; memory.check(GuestAddress{self}, size, Access::Write);
        if (constructor) { clear(self, size); return start("kernel32.dll", "CreateEventW", {0, 1, 0, 0}); }
        const auto handle = load(self);
        if (destructor) { clear(self, size); if (!handle) return finish(0); return start("kernel32.dll", "CloseHandle", {handle}); }
        if (!handle) throw HostCallFault("Concrt event 尚未建構");
        if (name.starts_with("?set@")) return start("kernel32.dll", "SetEvent", {handle});
        if (name.starts_with("?reset@")) return start("kernel32.dll", "ResetEvent", {handle});
        return start("kernel32.dll", "WaitForSingleObject", {handle, static_cast<std::uint32_t>(argument(0))});
    }
    // ---- _NonReentrantBlockingLock／_ReentrantBlockingLock：物件儲存區即一個 CRITICAL_SECTION ----
    if (name.find("BlockingLock@details@Concurrency@@") != name.npos) {
        memory.check(GuestAddress{self}, x86_ ? 24U : 40U, Access::Write);
        if (constructor) return start("kernel32.dll", "InitializeCriticalSection", {self});
        if (destructor) return start("kernel32.dll", "DeleteCriticalSection", {self});
        if (name.starts_with("?_Release@")) return start("kernel32.dll", "LeaveCriticalSection", {self});
        return start("kernel32.dll", name.starts_with("?_TryAcquire@") ? "TryEnterCriticalSection" : "EnterCriticalSection", {self});
    }
    // ---- _NonReentrantPPLLock／_ReentrantPPLLock：內嵌 critical_section（與 critical_section 相同的擁有者＋SRW 字組），
    //      之後是遞迴次數與擁有者 ID（-1 表示沒有擁有者）；_Scoped_lock 的 [0] 指向鎖 ----
    if (name.find("PPLLock@details@Concurrency@@") != name.npos) {
        const bool scoped = name.find("@_Scoped_lock@") != name.npos, reentrant = name.find("_ReentrantPPLLock@") != name.npos;
        const unsigned section = x86_ ? 32U : 56U, size = section + 8;
        if (!scoped && constructor) { clear(self, size); memory.store(GuestAddress{self + section + 4}, 32, UINT32_MAX); return finish(self); }
        const auto lock = scoped ? (constructor ? argument(0) : load(self)) : self;
        memory.check(GuestAddress{lock}, size, Access::Write);
        if (scoped && constructor) { memory.check(GuestAddress{self}, unit, Access::Write); store(self, lock); }
        op.arguments[1] = lock;
        const auto count_at = GuestAddress{lock + section}, owner_at = GuestAddress{lock + section + 4};
        const auto context = static_cast<std::uint32_t>(threads_.current);
        if (name.starts_with("?_Release@") || (scoped && destructor)) {
            if (reentrant) {
                if (memory.load(owner_at, 32) != context) throw HostCallFault("目前 context 未持有 Concrt PPL 鎖");
                const auto depth = memory.load(count_at, 32);
                if (depth > 1) { memory.store(count_at, 32, depth - 1); return finish(0); }
                memory.store(count_at, 32, 0); memory.store(owner_at, 32, UINT32_MAX);
            }
            if (load(lock) != threads_.current) throw HostCallFault("目前 context 未持有 Concrt PPL 鎖");
            store(lock, 0); return start("kernel32.dll", "ReleaseSRWLockExclusive", {lock + unit});
        }
        if (reentrant && memory.load(owner_at, 32) == context) { memory.store(count_at, 32, memory.load(count_at, 32) + 1); return finish(scoped ? self : 0); }
        if (load(lock) == threads_.current) throw HostCallFault("Concrt 非重入 PPL 鎖不可由持有者再次取得");
        return start("kernel32.dll", "AcquireSRWLockExclusive", {lock + unit});
    }
    if (name.find("_ReaderWriterLock@details@Concurrency@@") != name.npos) { clear(self, 8); return finish(self); }
    // ---- _SpinWait<1>／<0>：{目前自旋數, 目前讓出數, 狀態, 讓出函式}；狀態 0 初始、1 自旋、2 讓出、3 結束 ----
    // _SpinCount::_Value 在多處理器時為 4000、單處理器為 0（不自旋直接讓出）；自旋本身不需在宿主空轉。
    const auto spin_count = runtime_.processor_count > 1 ? 4000U : 0U;
    if (name.find("@_SpinCount@") != name.npos) return finish(spin_count);
    if (name.find("_SpinWait@") != name.npos) {
        const unsigned function_at = x86_ ? 12U : 16U, size = x86_ ? 16U : 24U; const std::uint32_t yields = name.find("@$00@") != name.npos ? 1U : 0U;
        memory.check(GuestAddress{self}, size, Access::Write);
        const auto get = [&](unsigned at) { return static_cast<std::uint32_t>(memory.load(GuestAddress{self + at}, 32)); };
        const auto put = [&](unsigned at, std::uint32_t value) { memory.store(GuestAddress{self + at}, 32, value); };
        const auto set_count = [&](std::uint32_t count) {
            put(4, yields);
            if (count) { put(0, count); put(8, 1); } else put(8, yields ? 2U : 3U);
        };
        if (constructor || name.starts_with("??_F")) {
            const bool closure = name.starts_with("??_F"); clear(self, size);
            store(self + function_at, closure ? resolve("concrt140.dll", "?_Yield@_Context@details@Concurrency@@SAXXZ").value : argument(0));
            return finish(closure ? 0 : self);
        }
        if (name.starts_with("?_SetSpinCount@")) { set_count(static_cast<std::uint32_t>(argument(0))); return finish(0); }
        if (name.starts_with("?_Reset@")) { set_count(spin_count); return finish(0); }
        if (name.starts_with("?_NumberOfSpins@")) return finish(1);
        if (name.starts_with("?_ShouldSpinAgain@")) { const auto left = get(0) - 1; put(0, left); return finish(left > 0); }
        const auto yield = [&] { return yields ? guest(load(self + function_at)) : finish(0); };
        if (name.starts_with("?_DoYield@")) return yield();
        // _SpinOnce：初始狀態先以預設自旋數初始化；自旋耗盡後轉入讓出，讓出耗盡後回 false。
        if (!get(8)) set_count(spin_count);
        if (get(8) == 1) { const auto left = get(0) - 1; put(0, left); if (!left) put(8, get(4) ? 2U : 3U); return finish(1); }
        if (get(8) == 2) { const auto left = get(4) - 1; put(4, left); if (!left) put(8, 3); return yield(); }
        return finish(0);
    }
    // ---- Context 讓出、Alloc／Free、wait 與 _GetConcurrency ----
    if (name.starts_with("?Yield@Context@") || name.starts_with("?_SpinYield@Context@") || name.starts_with("?_Yield@_Context@")) return start("kernel32.dll", "SwitchToThread", {});
    if (name.starts_with("?Alloc@Concurrency@@")) {
        const auto block = allocate_heap(memory, std::max<std::uint64_t>(1, call(0)));
        if (!block) { throw_msvc_exception(state, memory, service, MsvcExceptionKind::BadAlloc); return std::nullopt; }
        return finish(block);
    }
    if (name.starts_with("?Free@Concurrency@@")) { if (call(0) && !free_heap(memory, call(0), state)) throw HostCallFault("Concrt Free 的位址不是 Alloc 配置"); return finish(0); }
    if (name == "?wait@Concurrency@@YAXI@Z") return start("kernel32.dll", "Sleep", {static_cast<std::uint32_t>(call(0))});
    return finish(std::max(1U, runtime_.processor_count));
}

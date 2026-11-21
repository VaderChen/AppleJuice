#define _WIN32_WINNT 0x0601
#define COBJMACROS
#include "d3dx_thread_pump_abi.h"
#include <d3d10.h>
#include <d3d10effect.h>
#include <d3dcompiler.h>
#include <stdint.h>

enum Stage { LoadStage, ProcessStage, DeviceStage, StageCount };
typedef struct Pump Pump;

typedef struct Work {
    struct Work *next;
    D3dxDataLoader *loader;
    D3dxDataProcessor *processor;
    HRESULT *status;
    void **output;
    uint64_t generation;
} Work;

typedef struct Queue {
    Work *first, *last;
    UINT count;
} Queue;

typedef struct Worker {
    Pump *pump;
    HANDLE handle;
    DWORD id;
    enum Stage stage;
} Worker;

struct Pump {
    D3dxThreadPump iface;
    LONG references;
    HANDLE heap;
    SRWLOCK lock;
    CONDITION_VARIABLE ready[2], changed;
    Queue queues[StageCount];
    uint64_t generation;
    UINT pending, cancelled;
    BOOL closing, purging;
    Worker *shutdown_worker;
    UINT worker_count;
    Worker workers[];
};

static void push(Queue *queue, Work *work)
{
    work->next = NULL;
    if (queue->last) queue->last->next = work;
    else queue->first = work;
    queue->last = work;
    ++queue->count;
}

static Work *pop(Queue *queue)
{
    Work *work = queue->first;
    if (work) {
        queue->first = work->next;
        if (!queue->first) queue->last = NULL;
        --queue->count;
    }
    return work;
}

// 必須持鎖摘除，Destroy 在解鎖後執行，客體可在回呼中呼叫其他服務。
static Work *detach_queues(Pump *pump)
{
    Work *first = NULL, *last = NULL;
    for (unsigned stage = 0; stage < StageCount; ++stage) {
        Queue *queue = &pump->queues[stage];
        if (!queue->first) continue;
        if (last) last->next = queue->first;
        else first = queue->first;
        last = queue->last;
        queue->first = queue->last = NULL;
        queue->count = 0;
    }
    return first;
}

static void retire(Pump *pump, Work *work, HRESULT status)
{
    const uint64_t generation = work->generation;
    work->loader->lpVtbl->Destroy(work->loader);
    work->processor->lpVtbl->Destroy(work->processor);
    if (work->status) *work->status = status;
    HeapFree(pump->heap, 0, work);

    AcquireSRWLockExclusive(&pump->lock);
    if (generation == pump->generation) --pump->pending;
    else --pump->cancelled;
    WakeAllConditionVariable(&pump->changed);
    ReleaseSRWLockExclusive(&pump->lock);
}

static void cancel_list(Pump *pump, Work *work)
{
    while (work) {
        Work *next = work->next;
        retire(pump, work, S_FALSE);
        work = next;
    }
}

static DWORD WINAPI run_worker(void *argument)
{
    Worker *worker = argument;
    Pump *pump = worker->pump;
    const enum Stage stage = worker->stage;
    for (;;) {
        AcquireSRWLockExclusive(&pump->lock);
        while (!pump->closing && !pump->queues[stage].first)
            SleepConditionVariableSRW(&pump->ready[stage], &pump->lock, INFINITE, 0);
        if (pump->closing) {
            const BOOL free_here = pump->shutdown_worker == worker;
            ReleaseSRWLockExclusive(&pump->lock);
            if (free_here) HeapFree(pump->heap, 0, pump);
            return 0;
        }
        Work *work = pop(&pump->queues[stage]);
        ReleaseSRWLockExclusive(&pump->lock);

        HRESULT status;
        if (stage == LoadStage) status = work->loader->lpVtbl->Load(work->loader);
        else {
            void *data = NULL;
            SIZE_T bytes = 0;
            status = work->loader->lpVtbl->Decompress(work->loader, &data, &bytes);
            if (SUCCEEDED(status))
                status = work->processor->lpVtbl->Process(work->processor, data, bytes);
        }

        AcquireSRWLockExclusive(&pump->lock);
        const BOOL cancelled = pump->closing || work->generation != pump->generation;
        if (SUCCEEDED(status) && !cancelled) {
            push(&pump->queues[stage + 1], work);
            if (stage == LoadStage) WakeConditionVariable(&pump->ready[ProcessStage]);
            else WakeAllConditionVariable(&pump->changed);
            ReleaseSRWLockExclusive(&pump->lock);
        } else {
            ReleaseSRWLockExclusive(&pump->lock);
            retire(pump, work, cancelled ? S_FALSE : status);
        }
    }
}

static ULONG WINAPI pump_add_ref(D3dxThreadPump *iface)
{
    return (ULONG)InterlockedIncrement(&((Pump *)iface)->references);
}

static Worker *own_worker(Pump *pump)
{
    const DWORD current = GetCurrentThreadId();
    for (UINT i = 0; i < pump->worker_count; ++i)
        if (pump->workers[i].handle && pump->workers[i].id == current) return &pump->workers[i];
    return NULL;
}

static ULONG WINAPI pump_release(D3dxThreadPump *iface)
{
    Pump *pump = (Pump *)iface;
    const LONG references = InterlockedDecrement(&pump->references);
    if (references) return (ULONG)references;

    AcquireSRWLockExclusive(&pump->lock);
    pump->shutdown_worker = own_worker(pump);
    pump->closing = TRUE;
    Work *abandoned = detach_queues(pump);
    WakeAllConditionVariable(&pump->ready[LoadStage]);
    WakeAllConditionVariable(&pump->ready[ProcessStage]);
    ReleaseSRWLockExclusive(&pump->lock);
    for (UINT i = 0; i < pump->worker_count; ++i) {
        Worker *worker = &pump->workers[i];
        if (!worker->handle) continue;
        if (worker != pump->shutdown_worker) WaitForSingleObject(worker->handle, INFINITE);
        CloseHandle(worker->handle);
    }
    cancel_list(pump, abandoned);
    // 最後一次 Release 可由自訂 loader／processor 的客體工作執行緒送出。
    if (!pump->shutdown_worker) HeapFree(pump->heap, 0, pump);
    return 0;
}

static BOOL same_guid(const GUID *a, const GUID *b)
{
    const BYTE *lhs = (const BYTE *)a, *rhs = (const BYTE *)b;
    for (unsigned i = 0; i < sizeof(GUID); ++i) if (lhs[i] != rhs[i]) return FALSE;
    return TRUE;
}

static HRESULT WINAPI pump_query(D3dxThreadPump *iface, const GUID *iid, void **output)
{
    static const GUID unknown = {0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
    static const GUID thread_pump = {0xc93fecfa,0x6967,0x478a,{0xab,0xbc,0x40,0x2d,0x90,0x62,0x1f,0xcb}};
    if (!output) return E_POINTER;
    *output = NULL;
    if (!iid) return E_INVALIDARG;
    if (!same_guid(iid, &unknown) && !same_guid(iid, &thread_pump)) return E_NOINTERFACE;
    pump_add_ref(iface);
    *output = iface;
    return S_OK;
}

static HRESULT WINAPI pump_add_work(D3dxThreadPump *iface, D3dxDataLoader *loader, D3dxDataProcessor *processor,
                                   HRESULT *status, void **output)
{
    Pump *pump = (Pump *)iface;
    if (!loader || !processor) return E_INVALIDARG;
    Work *work = HeapAlloc(pump->heap, 0, sizeof(*work));
    if (!work) return E_OUTOFMEMORY;
    work->loader = loader;
    work->processor = processor;
    work->status = status;
    work->output = output;
    AcquireSRWLockExclusive(&pump->lock);
    if (pump->closing || pump->pending == UINT_MAX - pump->cancelled) {
        ReleaseSRWLockExclusive(&pump->lock);
        HeapFree(pump->heap, 0, work);
        return E_FAIL;
    }
    if (output) *output = NULL;
    work->generation = pump->generation;
    ++pump->pending;
    push(&pump->queues[LoadStage], work);
    WakeConditionVariable(&pump->ready[LoadStage]);
    ReleaseSRWLockExclusive(&pump->lock);
    return S_OK;
}

static UINT WINAPI pump_work_count(D3dxThreadPump *iface)
{
    Pump *pump = (Pump *)iface;
    AcquireSRWLockExclusive(&pump->lock);
    const UINT count = pump->pending + pump->cancelled;
    ReleaseSRWLockExclusive(&pump->lock);
    return count;
}

static void process_ready(Pump *pump, UINT count)
{
    while (count--) {
        AcquireSRWLockExclusive(&pump->lock);
        Work *work = pop(&pump->queues[DeviceStage]);
        ReleaseSRWLockExclusive(&pump->lock);
        if (!work) return;
        HRESULT status = work->processor->lpVtbl->CreateDeviceObject(work->processor, work->output);
        retire(pump, work, status);
    }
}

static HRESULT WINAPI pump_process(D3dxThreadPump *iface, UINT count)
{
    pump_add_ref(iface);
    process_ready((Pump *)iface, count);
    pump_release(iface);
    return S_OK;
}

static HRESULT WINAPI pump_wait(D3dxThreadPump *iface)
{
    Pump *pump = (Pump *)iface;
    pump_add_ref(iface);
    for (;;) {
        AcquireSRWLockExclusive(&pump->lock);
        while (pump->pending + pump->cancelled && !pump->queues[DeviceStage].first)
            SleepConditionVariableSRW(&pump->changed, &pump->lock, INFINITE, 0);
        const BOOL ready = pump->queues[DeviceStage].first != NULL;
        ReleaseSRWLockExclusive(&pump->lock);
        if (!ready) break;
        process_ready(pump, UINT_MAX);
    }
    pump_release(iface);
    return S_OK;
}

static HRESULT WINAPI pump_purge(D3dxThreadPump *iface)
{
    Pump *pump = (Pump *)iface;
    pump_add_ref(iface);
    AcquireSRWLockExclusive(&pump->lock);
    while (pump->purging) SleepConditionVariableSRW(&pump->changed, &pump->lock, INFINITE, 0);
    pump->purging = TRUE;
    ++pump->generation;
    pump->cancelled = pump->pending;
    pump->pending = 0;
    Work *abandoned = detach_queues(pump);
    ReleaseSRWLockExclusive(&pump->lock);
    cancel_list(pump, abandoned);
    AcquireSRWLockExclusive(&pump->lock);
    while (pump->cancelled) SleepConditionVariableSRW(&pump->changed, &pump->lock, INFINITE, 0);
    pump->purging = FALSE;
    WakeAllConditionVariable(&pump->changed);
    ReleaseSRWLockExclusive(&pump->lock);
    pump_release(iface);
    return S_OK;
}

static HRESULT WINAPI pump_status(D3dxThreadPump *iface, UINT *io, UINT *processing, UINT *device)
{
    Pump *pump = (Pump *)iface;
    AcquireSRWLockExclusive(&pump->lock);
    if (io) *io = pump->queues[LoadStage].count;
    if (processing) *processing = pump->queues[ProcessStage].count;
    if (device) *device = pump->queues[DeviceStage].count;
    ReleaseSRWLockExclusive(&pump->lock);
    return S_OK;
}

static const D3dxThreadPumpVtbl pump_vtable = {
    pump_query, pump_add_ref, pump_release, pump_add_work, pump_work_count,
    pump_wait, pump_process, pump_purge, pump_status,
};

static HRESULT create_pump(UINT io_threads, UINT proc_threads, D3dxThreadPump **output)
{
    if (!output) return E_POINTER;
    *output = NULL;
    if (io_threads >= 1024 || proc_threads >= 1024) return E_FAIL;
    if (!io_threads) io_threads = 1;
    if (!proc_threads) {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        proc_threads = info.dwNumberOfProcessors;
        if (!proc_threads) proc_threads = 1;
    }
    const UINT count = io_threads + proc_threads;
    HANDLE heap = GetProcessHeap();
    Pump *pump = HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(*pump) + count * sizeof(Worker));
    if (!pump) return E_OUTOFMEMORY;
    pump->iface.lpVtbl = &pump_vtable;
    pump->heap = heap;
    pump->references = 1;
    pump->worker_count = count;
    for (UINT i = 0; i < count; ++i) {
        Worker *worker = &pump->workers[i];
        worker->pump = pump;
        worker->stage = i < io_threads ? LoadStage : ProcessStage;
        worker->handle = CreateThread(NULL, 0, run_worker, worker, 0, &worker->id);
        if (!worker->handle) {
            pump_release(&pump->iface);
            return E_FAIL;
        }
    }
    *output = &pump->iface;
    return S_OK;
}

__declspec(dllexport) HRESULT WINAPI D3DX10CreateThreadPump(UINT io, UINT processing, D3dxThreadPump **output)
{
    return create_pump(io, processing, output);
}

__declspec(dllexport) HRESULT WINAPI D3DX11CreateThreadPump(UINT io, UINT processing, D3dxThreadPump **output)
{
    return create_pump(io, processing, output);
}

#include "guest_d3dx_shader_processor.inc"
#include "guest_d3dx10_skin.inc"

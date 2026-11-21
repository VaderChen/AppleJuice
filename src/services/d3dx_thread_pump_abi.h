#pragma once
#include <windows.h>

// D3DX10／11 的 loader、processor 與 ThreadPump 共用同一份客體 ABI。
typedef struct D3dxDataLoader D3dxDataLoader;
typedef struct D3dxDataProcessor D3dxDataProcessor;
typedef struct D3dxThreadPump D3dxThreadPump;

typedef struct D3dxDataLoaderVtbl {
    HRESULT (WINAPI *Load)(D3dxDataLoader *);
    HRESULT (WINAPI *Decompress)(D3dxDataLoader *, void **, SIZE_T *);
    HRESULT (WINAPI *Destroy)(D3dxDataLoader *);
} D3dxDataLoaderVtbl;
struct D3dxDataLoader { const D3dxDataLoaderVtbl *lpVtbl; };

typedef struct D3dxDataProcessorVtbl {
    HRESULT (WINAPI *Process)(D3dxDataProcessor *, void *, SIZE_T);
    HRESULT (WINAPI *CreateDeviceObject)(D3dxDataProcessor *, void **);
    HRESULT (WINAPI *Destroy)(D3dxDataProcessor *);
} D3dxDataProcessorVtbl;
struct D3dxDataProcessor { const D3dxDataProcessorVtbl *lpVtbl; };

typedef struct D3dxThreadPumpVtbl {
    HRESULT (WINAPI *QueryInterface)(D3dxThreadPump *, const GUID *, void **);
    ULONG (WINAPI *AddRef)(D3dxThreadPump *);
    ULONG (WINAPI *Release)(D3dxThreadPump *);
    HRESULT (WINAPI *AddWorkItem)(D3dxThreadPump *, D3dxDataLoader *, D3dxDataProcessor *, HRESULT *, void **);
    UINT (WINAPI *GetWorkItemCount)(D3dxThreadPump *);
    HRESULT (WINAPI *WaitForAllItems)(D3dxThreadPump *);
    HRESULT (WINAPI *ProcessDeviceWorkItems)(D3dxThreadPump *, UINT);
    HRESULT (WINAPI *PurgeAllItems)(D3dxThreadPump *);
    HRESULT (WINAPI *GetQueueStatus)(D3dxThreadPump *, UINT *, UINT *, UINT *);
} D3dxThreadPumpVtbl;
struct D3dxThreadPump { const D3dxThreadPumpVtbl *lpVtbl; };

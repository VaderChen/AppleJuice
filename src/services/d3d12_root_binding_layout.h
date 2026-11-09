#pragma once

// 清單 COM 物件的快速錄製緩衝指標。緩衝以 32 位元槽數開頭，資料從位移 16 開始。
#define X64ARM_D3D12_LIST_ROOT_BUFFER 8
#define X64ARM_D3D12_ROOT_BUFFER_HEADER 16
#define X64ARM_D3D12_ROOT_BUFFER_CAPACITY 64
#define X64ARM_D3D12_ROOT_BINDING_SHIFT 4
#define X64ARM_D3D12_ROOT_BINDING_METHODS(APPLY) \
    APPLY(SetGraphicsRootDescriptorTable, 0) \
    APPLY(SetComputeRootDescriptorTable, 1) \
    APPLY(SetGraphicsRootConstantBufferView, 2) \
    APPLY(SetComputeRootConstantBufferView, 3) \
    APPLY(SetGraphicsRootShaderResourceView, 4) \
    APPLY(SetComputeRootShaderResourceView, 5) \
    APPLY(SetGraphicsRootUnorderedAccessView, 6) \
    APPLY(SetComputeRootUnorderedAccessView, 7)

// 繪製占用兩槽。首槽依序存方法、數量、實例數與起始索引。次槽存餘下的堆疊參數。
#define X64ARM_D3D12_DRAW_INSTANCED 8
#define X64ARM_D3D12_DRAW_INDEXED_INSTANCED 9
// 輸入檢視也占用兩槽。首槽存方法、頂點槽及 GPU 位址。次槽存大小及 stride／format。
#define X64ARM_D3D12_IA_VERTEX_BUFFER 10
#define X64ARM_D3D12_IA_INDEX_BUFFER 11

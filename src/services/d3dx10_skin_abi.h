#pragma once
#include <windows.h>

// ID3DX10SkinInfo 的客體 ABI；頂點通道各為三個連續的 float。
typedef struct D3dx10Skin D3dx10Skin;
typedef struct D3dx10SkinChannel {
    UINT source_offset;
    UINT destination_offset;
    BOOL normal;
} D3dx10SkinChannel;
typedef struct D3dx10SkinMatrix { float m[4][4]; } D3dx10SkinMatrix;

typedef struct D3dx10SkinVtbl {
    HRESULT (WINAPI *QueryInterface)(D3dx10Skin *, const GUID *, void **);
    ULONG (WINAPI *AddRef)(D3dx10Skin *);
    ULONG (WINAPI *Release)(D3dx10Skin *);
    UINT (WINAPI *GetNumVertices)(D3dx10Skin *);
    UINT (WINAPI *GetNumBones)(D3dx10Skin *);
    UINT (WINAPI *GetMaxBoneInfluences)(D3dx10Skin *);
    HRESULT (WINAPI *AddVertices)(D3dx10Skin *, UINT);
    HRESULT (WINAPI *RemapVertices)(D3dx10Skin *, UINT, const UINT *);
    HRESULT (WINAPI *AddBones)(D3dx10Skin *, UINT);
    HRESULT (WINAPI *RemoveBone)(D3dx10Skin *, UINT);
    HRESULT (WINAPI *RemapBones)(D3dx10Skin *, UINT, const UINT *);
    HRESULT (WINAPI *AddBoneInfluences)(D3dx10Skin *, UINT, UINT, const UINT *, const float *);
    HRESULT (WINAPI *ClearBoneInfluences)(D3dx10Skin *, UINT);
    UINT (WINAPI *GetBoneInfluenceCount)(D3dx10Skin *, UINT);
    HRESULT (WINAPI *GetBoneInfluences)(D3dx10Skin *, UINT, UINT, UINT, UINT *, float *);
    HRESULT (WINAPI *FindBoneInfluenceIndex)(D3dx10Skin *, UINT, UINT, UINT *);
    HRESULT (WINAPI *SetBoneInfluence)(D3dx10Skin *, UINT, UINT, float);
    HRESULT (WINAPI *GetBoneInfluence)(D3dx10Skin *, UINT, UINT, float *);
    HRESULT (WINAPI *Compact)(D3dx10Skin *, UINT, UINT, float);
    HRESULT (WINAPI *DoSoftwareSkinning)(D3dx10Skin *, UINT, UINT, const void *, UINT, void *, UINT,
            const D3dx10SkinMatrix *, const D3dx10SkinMatrix *, const D3dx10SkinChannel *, UINT);
} D3dx10SkinVtbl;
struct D3dx10Skin { const D3dx10SkinVtbl *lpVtbl; };

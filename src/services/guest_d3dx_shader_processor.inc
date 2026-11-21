// 著色器編譯器與前處理器在客體 worker 中呼叫內建 D3DCompiler，保留原有 Include ABI。
typedef struct EffectProcessor {
    ID3D10Device *device;
    ID3D10EffectPool *pool;
    UINT flags;
    BOOL create_pool;
} EffectProcessor;

typedef struct ShaderProcessor {
    D3dxDataProcessor iface;
    HANDLE heap;
    char *name, *entry, *profile;
    D3D_SHADER_MACRO *macros;
    ID3DInclude *include;
    UINT flags, effect_flags;
    ID3DBlob **output, **errors;
    ID3DBlob *code, *diagnostics;
    EffectProcessor effect;
} ShaderProcessor;

static void clear_shader_results(ShaderProcessor *processor)
{
    if (processor->code) ID3D10Blob_Release(processor->code);
    if (processor->diagnostics) ID3D10Blob_Release(processor->diagnostics);
    processor->code = NULL;
    processor->diagnostics = NULL;
}

static HRESULT WINAPI shader_process(D3dxDataProcessor *iface, void *data, SIZE_T bytes)
{
    ShaderProcessor *processor = (ShaderProcessor *)iface;
    clear_shader_results(processor);
    if (!data || !bytes) return E_INVALIDARG;
    if (processor->effect.device && bytes >= 4) {
        const BYTE *source = data;
        const uint32_t magic = (uint32_t)source[0] | (uint32_t)source[1] << 8 |
                (uint32_t)source[2] << 16 | (uint32_t)source[3] << 24;
        if (magic == 0x43425844 || magic == 0xfeff1001 || magic == 0xfeff1011) {
            if (bytes > 64U * 1024U * 1024U) return E_INVALIDARG;
            const HRESULT result = D3DCreateBlob(bytes, &processor->code);
            if (FAILED(result)) return result;
            BYTE *copy = ID3D10Blob_GetBufferPointer(processor->code);
            for (SIZE_T i = 0; i < bytes; ++i) copy[i] = source[i];
            return S_OK;
        }
    }
    HRESULT status;
    if (processor->profile)
        status = D3DCompile(data, bytes, processor->name, processor->macros, processor->include,
                processor->entry, processor->profile, processor->flags, processor->effect_flags,
                &processor->code, processor->errors ? &processor->diagnostics : NULL);
    else
        status = D3DPreprocess(data, bytes, processor->name, processor->macros, processor->include,
                &processor->code, processor->errors ? &processor->diagnostics : NULL);

    // Process 失敗時 Thread Pump 不呼叫 CreateDeviceObject，仍須交付編譯錯誤。
    if (FAILED(status) && processor->errors) {
        *processor->errors = processor->diagnostics;
        processor->diagnostics = NULL;
    }
    return status;
}

static HRESULT WINAPI shader_publish(D3dxDataProcessor *iface, void **object)
{
    ShaderProcessor *processor = (ShaderProcessor *)iface;
    if (!processor->code) return E_FAIL;
    HRESULT result = S_OK;
    const EffectProcessor *effect = &processor->effect;
    if (effect->device) {
        if (!object) return E_INVALIDARG;
        void *data = ID3D10Blob_GetBufferPointer(processor->code);
        SIZE_T size = ID3D10Blob_GetBufferSize(processor->code);
        if (effect->create_pool) {
            ID3D10EffectPool *pool = NULL;
            result = D3D10CreateEffectPoolFromMemory(data, size, effect->flags, effect->device, &pool);
            *object = pool;
        } else {
            ID3D10Effect *instance = NULL;
            result = D3D10CreateEffectFromMemory(data, size, effect->flags, effect->device, effect->pool, &instance);
            *object = instance;
        }
    } else {
        *processor->output = processor->code;
        processor->code = NULL;
    }
    if (processor->errors) {
        *processor->errors = processor->diagnostics;
        processor->diagnostics = NULL;
    }
    return result;
}

static HRESULT WINAPI shader_destroy(D3dxDataProcessor *iface)
{
    ShaderProcessor *processor = (ShaderProcessor *)iface;
    HANDLE heap = processor->heap;
    clear_shader_results(processor);
    if (processor->effect.pool) ID3D10EffectPool_Release(processor->effect.pool);
    if (processor->effect.device) ID3D10Device_Release(processor->effect.device);
    if (processor->name) HeapFree(heap, 0, processor->name);
    if (processor->entry) HeapFree(heap, 0, processor->entry);
    if (processor->profile) HeapFree(heap, 0, processor->profile);
    if (processor->macros) {
        for (D3D_SHADER_MACRO *item = processor->macros; item->Name; ++item) {
            HeapFree(heap, 0, (char *)item->Name);
            if (item->Definition) HeapFree(heap, 0, (char *)item->Definition);
        }
        HeapFree(heap, 0, processor->macros);
    }
    HeapFree(heap, 0, processor);
    return S_OK;
}

static const D3dxDataProcessorVtbl shader_vtable = { shader_process, shader_publish, shader_destroy };

static char *shader_copy_string(HANDLE heap, const char *source)
{
    SIZE_T size = 0;
    while (source[size]) ++size;
    char *copy = HeapAlloc(heap, 0, size + 1);
    if (copy)
        for (SIZE_T i = 0; i <= size; ++i) copy[i] = source[i];
    return copy;
}

static HRESULT create_shader_processor(const char *name, const D3D_SHADER_MACRO *macros, ID3DInclude *include,
        const char *entry, const char *profile, UINT flags, UINT effect_flags, ID3DBlob **output, ID3DBlob **errors,
        D3dxDataProcessor **result, const EffectProcessor *effect)
{
    if (!result) return E_INVALIDARG;
    *result = NULL;
    if (!output && !effect) return E_INVALIDARG;

    SIZE_T count = 0;
    if (macros) {
        while (count < 255 && macros[count].Name) ++count;
        if (count == 255 && macros[count].Name) return E_INVALIDARG;
    }
    HANDLE heap = GetProcessHeap();
    ShaderProcessor *processor = HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(*processor));
    if (!processor) return E_OUTOFMEMORY;
    processor->iface.lpVtbl = &shader_vtable;
    processor->heap = heap;
    processor->include = include;
    processor->flags = flags;
    if (profile && profile[0] == 'f' && profile[1] == 'x' && profile[2] == '_')
        processor->effect_flags = effect_flags;
    processor->output = output;
    processor->errors = errors;
    if (name && !(processor->name = shader_copy_string(heap, name))) goto oom;
    if (entry && !(processor->entry = shader_copy_string(heap, entry))) goto oom;
    if (profile && !(processor->profile = shader_copy_string(heap, profile))) goto oom;
    if (count) {
        processor->macros = HeapAlloc(heap, HEAP_ZERO_MEMORY, (count + 1) * sizeof(*macros));
        if (!processor->macros) goto oom;
        for (SIZE_T i = 0; i < count; ++i) {
            D3D_SHADER_MACRO *item = &processor->macros[i];
            item->Name = shader_copy_string(heap, macros[i].Name);
            if (!item->Name) goto oom;
            if (macros[i].Definition) {
                item->Definition = shader_copy_string(heap, macros[i].Definition);
                if (!item->Definition) goto oom;
            }
        }
    }
    if (effect) {
        processor->effect = *effect;
        ID3D10Device_AddRef(effect->device);
        if (effect->pool) ID3D10EffectPool_AddRef(effect->pool);
    }
    if (output) *output = NULL;
    if (errors) *errors = NULL;
    *result = &processor->iface;
    return S_OK;

oom:
    shader_destroy(&processor->iface);
    return E_OUTOFMEMORY;
}

#define DEFINE_SHADER_PROCESSORS(version) \
__declspec(dllexport) HRESULT WINAPI version##CreateAsyncCompilerProcessor(const char *name, \
        const D3D_SHADER_MACRO *macros, ID3DInclude *include, const char *entry, const char *profile, \
        UINT flags, UINT effect_flags, ID3DBlob **output, ID3DBlob **errors, D3dxDataProcessor **result) \
{ \
    if (!profile || !*profile) return E_INVALIDARG; \
    return create_shader_processor(name, macros, include, entry, profile, flags, effect_flags, output, errors, result, NULL); \
} \
__declspec(dllexport) HRESULT WINAPI version##CreateAsyncShaderPreprocessProcessor(const char *name, \
        const D3D_SHADER_MACRO *macros, ID3DInclude *include, ID3DBlob **output, ID3DBlob **errors, D3dxDataProcessor **result) \
{ \
    return create_shader_processor(name, macros, include, NULL, NULL, 0, 0, output, errors, result, NULL); \
}

DEFINE_SHADER_PROCESSORS(D3DX10)
DEFINE_SHADER_PROCESSORS(D3DX11)
#undef DEFINE_SHADER_PROCESSORS

static HRESULT create_effect_processor(const char *name, const D3D_SHADER_MACRO *macros, ID3DInclude *include,
        const char *profile, UINT hlsl_flags, const EffectProcessor *effect, ID3DBlob **errors, D3dxDataProcessor **result)
{
    if (!effect->device || (effect->flags & ~0xbU) || (effect->create_pool && (effect->flags & 1)))
        return E_INVALIDARG;
    return create_shader_processor(name, macros, include, NULL, profile ? profile : "fx_4_0",
            hlsl_flags, effect->flags & 3, NULL, errors, result, effect);
}

__declspec(dllexport) HRESULT WINAPI D3DX10CreateAsyncEffectCreateProcessor(const char *name,
        const D3D_SHADER_MACRO *macros, ID3DInclude *include, const char *profile, UINT hlsl_flags, UINT fx_flags,
        ID3D10Device *device, ID3D10EffectPool *pool, ID3DBlob **errors, D3dxDataProcessor **result)
{
    const EffectProcessor effect = { device, pool, fx_flags, FALSE };
    return create_effect_processor(name, macros, include, profile, hlsl_flags, &effect, errors, result);
}

__declspec(dllexport) HRESULT WINAPI D3DX10CreateAsyncEffectPoolCreateProcessor(const char *name,
        const D3D_SHADER_MACRO *macros, ID3DInclude *include, const char *profile, UINT hlsl_flags, UINT fx_flags,
        ID3D10Device *device, ID3DBlob **errors, D3dxDataProcessor **result)
{
    const EffectProcessor effect = { device, NULL, fx_flags, TRUE };
    return create_effect_processor(name, macros, include, profile, hlsl_flags, &effect, errors, result);
}

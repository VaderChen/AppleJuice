            else if(resource_kind(id->kind)){
                supported=guid==IID_ID3D11Resource || guid==IID_ID3D11DeviceChild ||
                    (id->kind==Kind::BufferObject && guid==IID_ID3D11Buffer) ||
                    (id->kind==Kind::Texture1D && guid==IID_ID3D11Texture1D) ||
                    (id->kind==Kind::Texture3D && (guid==IID_ID3D11Texture3D || guid==IID_ID3D11Texture3D1));
            }else if(graphics_view_kind(id->kind)){
                supported=guid==IID_ID3D11View || guid==IID_ID3D11DeviceChild ||
                    (id->kind==Kind::ShaderResource && (guid==IID_ID3D11ShaderResourceView || guid==IID_ID3D11ShaderResourceView1)) ||
                    (id->kind==Kind::RenderTarget && (guid==IID_ID3D11RenderTargetView || guid==IID_ID3D11RenderTargetView1)) ||
                    (id->kind==Kind::Unordered && (guid==IID_ID3D11UnorderedAccessView || guid==IID_ID3D11UnorderedAccessView1)) ||
                    (id->kind==Kind::DepthView && guid==IID_ID3D11DepthStencilView);
            }else if(id->kind==Kind::SwapChain)supported=guid==IID_IDXGISwapChain || guid==IID_IDXGISwapChain1 || guid==IID_IDXGIDeviceSubObject || guid==IID_IDXGIObject;
            else if(id->kind==Kind::Output)supported=guid==IID_IDXGIOutput || guid==IID_IDXGIObject;
            else {
                static const std::map<Kind,Guid> kinds{{Kind::ClassLinkage,IID_ID3D11ClassLinkage},{Kind::ClassInstance,IID_ID3D11ClassInstance},{Kind::VertexShader,IID_ID3D11VertexShader},{Kind::PixelShader,IID_ID3D11PixelShader},{Kind::ComputeShader,IID_ID3D11ComputeShader},{Kind::GeometryShader,IID_ID3D11GeometryShader},{Kind::HullShader,IID_ID3D11HullShader},{Kind::DomainShader,IID_ID3D11DomainShader},{Kind::Layout,IID_ID3D11InputLayout},{Kind::Sampler,IID_ID3D11SamplerState},{Kind::Blend,IID_ID3D11BlendState},{Kind::DepthState,IID_ID3D11DepthStencilState},{Kind::Raster,IID_ID3D11RasterizerState},{Kind::Query,IID_ID3D11Query},{Kind::Predicate,IID_ID3D11Predicate},{Kind::CommandList,IID_ID3D11CommandList}};
                supported=guid==IID_ID3D11DeviceChild || (kinds.contains(id->kind) && guid==kinds.at(id->kind)) || ((id->kind==Kind::Query || id->kind==Kind::Predicate) && guid==IID_ID3D11Asynchronous) ||
                    // D3D11.1～11.3 延伸介面共用同一 vtable（GetDesc1／GetDesc2 接在原方法之後）。
                    (id->kind==Kind::Blend && guid==IID_ID3D11BlendState1) ||
                    (id->kind==Kind::Raster && (guid==IID_ID3D11RasterizerState1 || guid==IID_ID3D11RasterizerState2)) ||
                    (id->kind==Kind::Query && guid==IID_ID3D11Query1);
            }

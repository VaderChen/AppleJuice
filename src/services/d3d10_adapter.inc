        // 只轉換 ABI；資源驗證、hazard、Map 與 Metal 命令仍走共用處理器。
        const auto temporary=[&](std::span<const std::uint8_t> data){
            const auto pointer=allocate_heap(memory,data.size());if(!pointer)throw std::bad_alloc{};
            temporary_arguments.push_back(pointer);memory.write(GuestAddress{pointer},data);return pointer;
        };
        if(service.family=="internal-d3d10"){
            const auto original=object(original_arg(0)).identity;
            constexpr std::string_view context_methods[]{
#include "d3d10_context_methods.inc"
            };
            if(original->kind==Kind::Device && std::find(std::begin(context_methods),std::end(context_methods),symbol)!=std::end(context_methods))adapt(0,original->context10);
            if(symbol=="VSSetShader" || symbol=="GSSetShader" || symbol=="PSSetShader" || symbol=="VSGetShader" || symbol=="GSGetShader" || symbol=="PSGetShader"){
                adapt(2,0);adapt(3,0);
            }else if(symbol=="CreateVertexShader" || symbol=="CreateGeometryShader" || symbol=="CreatePixelShader"){
                adapt(4,original_arg(3));adapt(3,0);
            }else if(symbol=="CreateBuffer"){
                auto bytes=read_bytes(memory,arg(1),20);bytes.resize(24);adapt(1,temporary(bytes));
            }else if(symbol=="CreateDepthStencilView" && arg(2)){
                auto bytes=read_bytes(memory,arg(2),20);bytes.insert(bytes.begin()+8,4,0);adapt(2,temporary(bytes));
            }else if(symbol=="CreateBlendState"){
                const auto old=read_bytes(memory,arg(1),68);std::vector<std::uint8_t> bytes(264);
                put_word(bytes,0,word(old,0));put_word(bytes,4,1);
                for(unsigned i=0;i<8;++i){const auto at=8+i*32;put_word(bytes,at,word(old,4+i*4));for(unsigned j=0;j<6;++j)put_word(bytes,at+4+j*4,word(old,36+j*4));bytes[at+28]=old[60+i];}
                adapt(1,temporary(bytes));
            }else if(symbol=="CreateBlendState1")symbol="CreateBlendState";
            else if(symbol=="CreateShaderResourceView1")symbol="CreateShaderResourceView";
            else if(symbol=="SOGetTargets"){
                const auto context=object(arg(0)).identity;const auto count=u32(1);const auto destination=original_arg(3);if(count>4)throw std::invalid_argument("D3D10 SO 目標數量超限");
                if(destination)finish_adapter=[&,context,count,destination]{for(unsigned i=0;i<count;++i)memory.store(GuestAddress{destination+i*4},32,context->stream_offsets[i]);};
            }else if(symbol=="RSSetViewports"){
                if(u32(1)>16)throw std::invalid_argument("D3D10 viewport 數量超限");
                auto bytes=read_bytes(memory,arg(2),u32(1)*24);
                for(unsigned i=0;i<u32(1);++i)for(unsigned j=0;j<4;++j){const auto v=word(bytes,i*24+j*4);const float f=j<2?static_cast<float>(static_cast<std::int32_t>(v)):static_cast<float>(v);put_word(bytes,i*24+j*4,std::bit_cast<std::uint32_t>(f));}
                if(!bytes.empty())adapt(2,temporary(bytes));
            }else if(symbol=="RSGetViewports" && arg(2)){
                const auto count=static_cast<unsigned>(memory.load(GuestAddress{arg(1)},32));if(count>16)throw std::invalid_argument("D3D10 viewport 數量超限");
                const auto output_address=arg(2),count_address=arg(1);std::vector<std::uint8_t> bytes(std::max(24U,count*24));const auto scratch=temporary(bytes);adapt(2,scratch);
                finish_adapter=[&,count,output_address,count_address,scratch]{const auto actual=std::min<unsigned>(count,static_cast<unsigned>(memory.load(GuestAddress{count_address},32)));auto data=read_bytes(memory,scratch,actual*24);for(unsigned i=0;i<actual;++i)for(unsigned j=0;j<4;++j){const auto f=std::bit_cast<float>(word(data,i*24+j*4));put_word(data,i*24+j*4,j<2?static_cast<std::uint32_t>(static_cast<std::int32_t>(f)):static_cast<std::uint32_t>(f));}memory.write(GuestAddress{output_address},data);};
            }else if(symbol.ends_with("Map") || symbol.ends_with("Unmap")){
                const bool buffer=original->kind==Kind::BufferObject,map=symbol.ends_with("Map");
                const auto device=object(original->device).identity;adapt(0,device->context10);adapt(1,original->root);adapt(2,buffer?0:original_arg(1));
                if(map){adapt(3,original_arg(buffer?1:2));adapt(4,original_arg(buffer?2:3));const auto destination=original_arg(buffer?3:4);std::vector<std::uint8_t> bytes(pointer_size+8);const auto scratch=temporary(bytes);adapt(5,scratch);
                    finish_adapter=[&,scratch,destination,buffer,kind=original->kind]{const auto size=buffer || kind==Kind::Texture1D?pointer_size:kind==Kind::Texture?pointer_size+4:pointer_size+8;memory.write(GuestAddress{destination},read_bytes(memory,scratch,size));};
                }symbol=map?"Map":"Unmap";
            }else if(symbol=="QueryBegin" || symbol=="QueryEnd" || symbol=="QueryGetData"){
                adapt(0,object(original->device).identity->context10);adapt(1,original->root);
                if(symbol=="QueryGetData"){adapt(2,original_arg(1));adapt(3,original_arg(2));adapt(4,original_arg(3));symbol="GetData";}else symbol=symbol=="QueryBegin"?"Begin":"End";
            }else if(symbol.ends_with("GetDesc") || symbol.ends_with("GetDesc1")){
                const auto destination=arg(1);
                if(original->kind==Kind::BufferObject){adapt(1,temporary(std::vector<std::uint8_t>(24)));const auto scratch=arg(1);finish_adapter=[&,destination,scratch]{memory.write(GuestAddress{destination},read_bytes(memory,scratch,20));};symbol="BufferGetDesc";}
                else if(original->kind==Kind::Texture)symbol="TextureGetDesc";
                else if(original->kind==Kind::Texture1D)symbol="Texture1DGetDesc";
                else if(original->kind==Kind::Texture3D)symbol="Texture3DGetDesc";
                else if(original->kind==Kind::DepthView){adapt(1,temporary(std::vector<std::uint8_t>(24)));const auto scratch=arg(1);finish_adapter=[&,destination,scratch]{auto data=read_bytes(memory,scratch,24);data.erase(data.begin()+8,data.begin()+12);memory.write(GuestAddress{destination},data);};symbol="GraphicsGetDesc";}
                else if(original->kind==Kind::Blend && !symbol.ends_with("GetDesc1")){adapt(1,temporary(std::vector<std::uint8_t>(264)));const auto scratch=arg(1);finish_adapter=[&,destination,scratch]{const auto data=read_bytes(memory,scratch,264);std::vector<std::uint8_t> old(68);put_word(old,0,word(data,0));for(unsigned i=0;i<8;++i){put_word(old,4+i*4,word(data,8+i*32));old[60+i]=data[8+i*32+28];}for(unsigned j=0;j<6;++j)put_word(old,36+j*4,word(data,12+j*4));memory.write(GuestAddress{destination},old);};symbol="GraphicsGetDesc";}
                else symbol="GraphicsGetDesc";
            }
        }

        if(service.family=="d3dx10_43.dll" && symbol.starts_with("D3DX10CreateEffectFrom")){
            effect_compile=true;const bool file=symbol.find("FromFile")!=symbol.npos;const unsigned delta=file?0U:2U;
            if(original_arg(7+delta) || original_arg(8+delta))throw std::invalid_argument("FX4 pool 或非同步 pump 需要對應實作");
            memory.store(GuestAddress{original_arg(9+delta)},pointer_bits,0);effect_code_slot=temporary(std::vector<std::uint8_t>(pointer_size));
            adapted_symbol="D3DX11Compile"+std::string(symbol.substr(std::string_view("D3DX10CreateEffect").size()));symbol=adapted_symbol;
            adapt(3+delta,0);adapt(4+delta,original_arg(3+delta));adapt(5+delta,original_arg(4+delta));adapt(6+delta,original_arg(5+delta));adapt(7+delta,0);adapt(8+delta,effect_code_slot);adapt(9+delta,original_arg(10+delta));adapt(10+delta,original_arg(11+delta));
        }else if(service.family=="d3dx10_43.dll" && symbol.starts_with("D3DX10") && !symbol.starts_with("D3DX10CreateFont") && symbol!="D3DX10CreateSprite" && symbol!="D3DX10CreateMesh"){
            // D3DX10/11 的圖片資訊及載入設定具有相同 ABI。
            adapted_symbol="D3DX11"+std::string(symbol.substr(6));symbol=adapted_symbol;
            if(symbol.starts_with("D3DX11SaveTexture") || symbol=="D3DX11LoadTextureFromTexture"){
                for(unsigned i=service.arguments;i>0;--i)adapt(i,original_arg(i-1));
                adapt(0,object(object(original_arg(0)).identity->device).identity->context10);
            }
        }

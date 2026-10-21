        const bool frame_handled=symbol=="D3DXFrameFind" || symbol=="D3DXFrameNumNamedMatrices" || symbol=="D3DXFrameRegisterNamedMatrices" || symbol=="D3DXFrameAppendChild" || symbol=="D3DXFrameDestroy" || symbol=="D3DXFrameCalculateBoundingSphere";
        if(frame_handled){
            mesh_handled=true;const unsigned mesh_offset=64+ps,sibling_offset=64+2*ps,child_offset=64+3*ps;
            struct FrameView {std::uint64_t address{},name{},mesh{};Matrix transform{};};
            std::vector<FrameView> frames;std::vector<std::pair<std::uint64_t,Matrix>> pending;if(arg(0))pending.emplace_back(arg(0),identity());std::set<std::uint64_t> visited;
            while(!pending.empty()){const auto [address,parent]=pending.back();pending.pop_back();require(frames.size()<65536 && visited.insert(address).second);const auto data=read(address,64+4*ps);Matrix local{},combined{};std::memcpy(local.data(),data.data()+ps,64);for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)for(unsigned k=0;k<4;++k)combined[r*4+c]+=local[r*4+k]*parent[k*4+c];
                frames.push_back({address,memory.load(GuestAddress{address},pb),memory.load(GuestAddress{address+mesh_offset},pb),combined});const auto sibling=memory.load(GuestAddress{address+sibling_offset},pb),child=memory.load(GuestAddress{address+child_offset},pb);if(sibling)pending.emplace_back(sibling,parent);if(child)pending.emplace_back(child,combined);
            }
            if(symbol=="D3DXFrameFind"){require(arg(1));const auto name=read_ansi(memory,arg(1));for(const auto& frame:frames)if(frame.name && read_ansi(memory,frame.name)==name){result=frame.address;break;}}
            else if(symbol=="D3DXFrameNumNamedMatrices"){for(const auto& frame:frames)if(frame.name)++result;}
            else if(symbol=="D3DXFrameAppendChild"){
                require(arg(0) && arg(1) && !visited.contains(arg(1)));auto link=arg(0)+child_offset;std::set<std::uint64_t> siblings;
                for(auto next=memory.load(GuestAddress{link},pb);next;next=memory.load(GuestAddress{link},pb)){require(siblings.size()<65536 && siblings.insert(next).second);link=next+sibling_offset;}store_pointer(link,arg(1));
            }else if(symbol=="D3DXFrameRegisterNamedMatrices"){
                require(arg(1));const auto table=memory.load(GuestAddress{arg(1)},pb),callback=memory.load(GuestAddress{table+7*ps},pb);std::vector<GuestCallback> calls;
                for(const auto& frame:frames)if(frame.name)calls.push_back({callback,{arg(1),frame.name,frame.address+ps,0,0,0}});
                if(!calls.empty()){start_callbacks(state,memory,std::move(calls),Continuation::ReturnUntilFailure,0);return std::nullopt;}
            }else if(symbol=="D3DXFrameDestroy"){
                require(arg(1));const auto table=memory.load(GuestAddress{arg(1)},pb),destroy_frame=memory.load(GuestAddress{table+2*ps},pb),destroy_mesh=memory.load(GuestAddress{table+3*ps},pb);std::vector<GuestCallback> calls;std::set<std::uint64_t> containers;
                for(auto frame=frames.rbegin();frame!=frames.rend();++frame){for(auto mesh=frame->mesh;mesh;mesh=memory.load(GuestAddress{mesh+8*ps},pb)){require(containers.size()<65536 && containers.insert(mesh).second);calls.push_back({destroy_mesh,{arg(1),mesh}});}calls.push_back({destroy_frame,{arg(1),frame->address}});}
                if(!calls.empty()){start_callbacks(state,memory,std::move(calls),Continuation::ReturnUntilFailure,0);return std::nullopt;}
            }else{
                require(arg(1) && arg(2));std::vector<Vector> vertices;std::set<std::uint64_t> containers;
                for(const auto& frame:frames)for(auto container=frame.mesh;container;container=memory.load(GuestAddress{container+8*ps},pb)){
                    require(containers.size()<65536 && containers.insert(container).second);const auto mesh_pointer=memory.load(GuestAddress{container+2*ps},pb);if(!mesh_pointer)continue;require(memory.load(GuestAddress{container+ps},32)==1);auto mesh=get(mesh_pointer,Kind::Mesh);for(auto vertex:positions(*mesh)){vertex[3]=1;vertices.push_back(transform(vertex,frame.transform));}
                }
                if(std::getenv("X64ARM_TRACE_D3DX_HIERARCHY"))std::fprintf(stderr,"[d3dx9 sphere] root=%llx frames=%zu containers=%zu vertices=%zu\n",static_cast<unsigned long long>(arg(0)),frames.size(),containers.size(),vertices.size());require(!vertices.empty());std::array<double,3> mean{};for(const auto& vertex:vertices)for(unsigned c=0;c<3;++c)mean[c]+=vertex[c];Vector center{};for(unsigned c=0;c<3;++c)center[c]=static_cast<float>(mean[c]/static_cast<double>(vertices.size()));double radius=0;for(const auto& vertex:vertices){double squared=0;for(unsigned c=0;c<3;++c){const double difference=vertex[c]-center[c];squared+=difference*difference;}radius=std::max(radius,squared);}
                memory.write(GuestAddress{arg(1)},std::span(reinterpret_cast<const std::uint8_t*>(center.data()),12));memory.store(GuestAddress{arg(2)},32,bits(static_cast<float>(std::sqrt(radius))));
            }
        }

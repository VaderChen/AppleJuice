            const auto faces=static_cast<std::uint32_t>(arg(1)),vertices=static_cast<std::uint32_t>(arg(2));
            const std::uint8_t bits=arg(3)?32:16;
            const bool by_face=symbol=="D3DXOptimizeFaces";
            if(!arg(0) || !arg(4) || !faces || !vertices || (bits==16 && (vertices>65536 || (by_face && faces>65535)))){arg.return_to(state,0x8876086cU);return std::nullopt;}
            const std::uint64_t count=std::uint64_t{faces}*3;
            memory.check(GuestAddress{arg(0)},count*(bits/8),Access::Read);
            memory.check(GuestAddress{arg(4)},std::uint64_t{by_face?faces:vertices}*4,Access::Write);
            try{
                std::vector<std::uint32_t> indices(count),remap;
                for(std::uint64_t i=0;i<count;++i){indices[i]=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+i*(bits/8)},bits));if(indices[i]>=vertices){arg.return_to(state,0x8876086cU);return std::nullopt;}}
                if(!by_face){
                    // remap[new] = old；依第一次引用順序緊密排列，未引用頂點置於尾端。
                    std::vector<bool> seen(vertices);remap.reserve(vertices);
                    for(auto index:indices)if(!seen[index]){seen[index]=true;remap.push_back(index);}
                    for(unsigned i=0;i<vertices;++i)if(!seen[i])remap.push_back(i);
                }else{
                    // 以共用頂點形成局部候選，再以保守的 16-entry LRU 模擬驗收順序。
                    // 每個頂點只展開一次相鄰面，避免高價數網格退化成平方掃描。
                    std::vector<std::vector<unsigned>> adjacent(vertices);
                    for(unsigned i=0;i<faces;++i)for(unsigned j=0;j<3;++j)adjacent[indices[std::uint64_t{i}*3+j]].push_back(i);
                    std::vector<bool> expanded(vertices),queued(faces),emitted(faces);
                    std::priority_queue<std::pair<unsigned,unsigned>> candidates;
                    std::vector<unsigned> cache;remap.reserve(faces);unsigned first=0;
                    while(remap.size()<faces){
                        while(!candidates.empty() && emitted[candidates.top().second])candidates.pop();
                        while(first<faces && emitted[first])++first;
                        const auto face=candidates.empty()?first:candidates.top().second;if(!candidates.empty())candidates.pop();
                        emitted[face]=true;remap.push_back(face);
                        for(unsigned j=0;j<3;++j){const auto vertex=indices[std::uint64_t{face}*3+j];auto it=std::find(cache.begin(),cache.end(),vertex);if(it!=cache.end())cache.erase(it);cache.insert(cache.begin(),vertex);if(cache.size()>16)cache.pop_back();}
                        for(unsigned j=0;j<3;++j){const auto vertex=indices[std::uint64_t{face}*3+j];if(expanded[vertex])continue;expanded[vertex]=true;
                            for(auto candidate:adjacent[vertex])if(!queued[candidate] && !emitted[candidate]){queued[candidate]=true;unsigned score=0;for(unsigned k=0;k<3;++k)if(std::find(cache.begin(),cache.end(),indices[std::uint64_t{candidate}*3+k])!=cache.end())++score;candidates.emplace(score,candidate);}
                        }
                    }
                    const auto misses=[&](const std::vector<std::uint32_t>& order){std::vector<unsigned> lru;std::uint64_t n=0;for(auto face:order)for(unsigned j=0;j<3;++j){const auto index=indices[std::uint64_t{face}*3+j];auto it=std::find(lru.begin(),lru.end(),index);if(it==lru.end())++n;else lru.erase(it);lru.insert(lru.begin(),index);if(lru.size()>16)lru.pop_back();}return n;};
                    std::vector<std::uint32_t> original(faces);std::iota(original.begin(),original.end(),0U);
                    const auto before=misses(original),after=misses(remap);if(after>before)remap=std::move(original);
                    if(std::getenv("X64ARM_TRACE_GRAPHICS_ERRORS"))std::fprintf(stderr,"[d3dx_mesh] faces=%u vertices=%u cache_misses=%llu->%llu\n",faces,vertices,static_cast<unsigned long long>(before),static_cast<unsigned long long>(std::min(before,after)));
                }
                for(unsigned i=0;i<remap.size();++i)memory.store(GuestAddress{arg(4)+std::uint64_t{i}*4},32,remap[i]);
                if(const char* folder=std::getenv("X64ARM_DUMP_DXBC");folder && *folder){std::filesystem::create_directories(folder);const auto base=std::filesystem::path(folder)/(std::string(symbol)+"-"+std::to_string(faces)+"-"+std::to_string(vertices));std::ofstream input(base.string()+"-indices.bin",std::ios::binary),output(base.string()+"-remap.bin",std::ios::binary);input.write(reinterpret_cast<const char*>(indices.data()),static_cast<std::streamsize>(indices.size()*4));output.write(reinterpret_cast<const char*>(remap.data()),static_cast<std::streamsize>(remap.size()*4));}
                result=0;
            }catch(const std::bad_alloc&){result=0x8007000eU;}

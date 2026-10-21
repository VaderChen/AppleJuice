        const auto read_views=[&](std::uint64_t address,unsigned count,Kind kind,const std::shared_ptr<Identity>& context){
            std::vector<std::uint64_t> values(count);
            for(unsigned i=0;i<count;++i){values[i]=address?memory.load(GuestAddress{address+i*pointer_size},pointer_bits):0;if(values[i]){auto id=typed(values[i],kind);require(id->device==context->device && !id->offered && (!id->texture || !object(id->texture).identity->offered));}}
            return values;
        };
        const auto get_views=[&](const std::shared_ptr<Identity>& context,const std::string& name,unsigned start,unsigned count,std::uint64_t output_address){
            if(!output_address)return;memory.check(GuestAddress{output_address},count*pointer_size,Access::Write);
            for(unsigned i=0;i<count;++i){const auto pointer=binding(context,name,start+i);if(pointer)hold(pointer);write_pointer(output_address+i*pointer_size,pointer);}
        };
        const auto overlaps=[&](std::uint64_t first,std::uint64_t second){
            if(!first || !second)return false;const auto a=view_of(first),b=view_of(second);if(a.resource!=b.resource)return false;
            if(a.resource->desc.dimension==1)return true; // Buffer 在 D3D11 中只有一個 subresource。
            if(a.level>=b.level+b.levels || b.level>=a.level+a.levels)return false;
            if(a.resource->desc.dimension!=4 && (a.slice>=b.slice+b.slices || b.slice>=a.slice+a.slices))return false;
            const auto readonly=[&](std::uint64_t output_view,const GraphicsView& output,std::uint64_t input_view,const GraphicsView& input){
                if(object(output_view).kind!=Kind::DepthView || object(input_view).kind!=Kind::ShaderResource)return false;
                const bool stencil=input.format==22 || input.format==47;return bool(output.flags&(stencil?2U:1U));
            };
            return !readonly(first,a,second,b) && !readonly(second,b,first,a);
        };
        const auto outputs_conflict=[&](const std::shared_ptr<Identity>& context,std::uint64_t pointer){
            for(const auto* name:{"Targets","Depth","PSUnorderedAccessViews","CSUnorderedAccessViews","StreamOutput"}){
                const auto found=context->bindings.find(name);if(found!=context->bindings.end())for(auto other:found->second)if(overlaps(pointer,other))return true;
            }return false;
        };
        const auto remove_conflicts=[&](const std::shared_ptr<Identity>& context,std::span<const std::uint64_t> outputs,std::string_view preserved){
            for(auto& [name,values]:context->bindings){
                if(name==preserved || !(name.ends_with("ShaderResources") || name.ends_with("UnorderedAccessViews") || name=="StreamOutput" || name=="Vertices" || name=="Index" || name.ends_with("ConstantBuffers") || name=="Targets" || name=="Depth"))continue;
                for(auto& pointer:values)if(pointer && std::any_of(outputs.begin(),outputs.end(),[&](auto output_view){return overlaps(pointer,output_view);})){const auto previous=pointer;pointer=0;if(!context->weak_bindings)release(previous);}
            }
        };
        const auto nonconflicting=[&](std::span<const std::uint64_t> values){for(std::size_t i=0;i<values.size();++i)for(std::size_t j=0;j<i;++j)if(overlaps(values[i],values[j]))return false;return true;};
        const auto counter_values=[&](std::span<const std::uint64_t> values,std::uint64_t initial){
            std::vector<std::pair<std::shared_ptr<GraphicsResource>,std::uint32_t>> counts;
            if(initial)for(std::size_t i=0;i<values.size();++i)if(values[i]){auto counter=view_of(values[i]).counter;if(counter){const auto value=static_cast<std::uint32_t>(memory.load(GuestAddress{initial+i*4},32));if(value!=UINT32_MAX)counts.emplace_back(std::move(counter),value);}}
            return counts;
        };
        const auto reset_counters=[&](const std::shared_ptr<Identity>& context,const auto& counts){for(const auto& [counter,value]:counts){const GraphicsView view{counter};submit(context,{[view,initial_value=value](auto& native){native.graphics_write(view,std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(&initial_value),4},0,0);}});}};
        const auto target_dimensions=[&](std::span<const std::uint64_t> targets,std::uint64_t depth){
            std::optional<std::array<std::uint32_t,5>> reference;
            const auto check=[&](std::uint64_t pointer){if(!pointer)return true;const auto view=view_of(pointer);const auto& d=view.resource->desc;
                const std::array size{d.dimension,std::max(1U,d.width>>view.level),std::max(1U,d.height>>view.level),d.samples,view.slices};if(reference)return size==*reference;reference=size;return true;};
            if(!check(depth))return false;for(auto pointer:targets)if(!check(pointer))return false;return true;
        };

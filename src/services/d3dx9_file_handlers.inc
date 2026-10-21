        const bool file_handled=symbol=="D3DXFileCreate" || symbol.starts_with("XFile.") || symbol.starts_with("XEnum.") || symbol.starts_with("XData.");
        if(file_handled){
            mesh_handled=true;constexpr unsigned bad_value=0x88760385U,not_found=0x88760387U,parse_error=0x88760390U;
            const auto parse_document=[&](const std::vector<std::uint8_t>& bytes,const d3d9_xfile::Templates& templates){try{return std::make_shared<d3d9_xfile::Document>(d3d9_xfile::DocumentParser(bytes,templates).parse());}catch(const std::invalid_argument&){throw Failure{parse_error};}};
            if(symbol=="D3DXFileCreate"){output(0);auto file=create(Kind::XFile);file->x_templates=std::make_shared<d3d9_xfile::Templates>();store_pointer(arg(0),file->root);}
            else{
                const auto kind=symbol.starts_with("XFile.")?Kind::XFile:symbol.starts_with("XEnum.")?Kind::XEnum:Kind::XData;
                auto object=get(arg(0),kind);const auto name=symbol.substr(symbol.find('.')+1);
                if(name=="QueryInterface"){output(2);Guid requested{};memory.read(GuestAddress{arg(1)},requested);if(!supports(kind,requested))result=no_interface;else{hold(object);store_pointer(arg(2),object->root);}}
                else if(name=="AddRef")result=hold(object);else if(name=="Release")result=release(object->root);
                else if(kind==Kind::XFile){
                    if(name=="RegisterTemplates"){
                        require(arg(1) && arg(2)<=512U*1024U*1024U,bad_value);const auto parsed=parse_document(read(arg(1),arg(2)),*object->x_templates);*object->x_templates=parsed->templates;
                    }else if(name=="RegisterEnumTemplates"){auto enumeration=get(arg(1),Kind::XEnum);for(const auto& [label,value]:enumeration->x_document->templates)(*object->x_templates)[label]=value;}
                    else if(name=="CreateEnumObject"){
                        output(3);require(arg(1) && u(2)<=3,bad_value);std::vector<std::uint8_t> bytes;
                        if(u(2)<=1){const auto path=u(2)?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(1)})):read_ansi(memory,arg(1));const auto opened=filesystem_.open_reader(d3dx_path(path));require(!opened.error,0x88760389U);const auto info=opened.file->information();require(!info.error && info.metadata.size<=512U*1024U*1024U,bad_value);bytes.resize(info.metadata.size);const auto read_result=opened.file->read(bytes,0);require(!read_result.error && read_result.bytes==bytes.size(),parse_error);}
                        else if(u(2)==3){const auto pointer=memory.load(GuestAddress{arg(1)},pb),size=memory.load(GuestAddress{arg(1)+ps},pb);require(pointer && size<=512U*1024U*1024U,bad_value);bytes=read(pointer,size);}
                        else{const auto module=memory.load(GuestAddress{arg(1)},pb),resource_name=memory.load(GuestAddress{arg(1)+ps},pb),type=memory.load(GuestAddress{arg(1)+2*ps},pb);const auto found=gui_resource(memory,module,resource_name,type,false);require(found,0x8876038aU);const auto size=memory.load(GuestAddress{found+4},32);require(size<=512U*1024U*1024U,bad_value);bytes=read(resource_image(found,true)->base+memory.load(GuestAddress{found},32),size);}
                        auto document=parse_document(bytes,*object->x_templates);auto enumeration=create(Kind::XEnum,object->root);enumeration->x_document=std::move(document);store_pointer(arg(3),enumeration->root);
                    }else require(false,0x80004001U);
                }else{
                    auto enumeration=kind==Kind::XEnum?object:get(object->parent,Kind::XEnum);auto& document=*enumeration->x_document;
                    const auto child_object=[&](unsigned index){require(index<document.nodes.size(),bad_value);auto& node=document.nodes[index];
                        if(node.guest){const auto found=bridge.objects.find(node.guest);if(found!=bridge.objects.end() && found->second->kind==Kind::XData && found->second->x_document==enumeration->x_document && found->second->x_index==index){hold(found->second);return found->second;}}
                        auto child=create(Kind::XData,enumeration->root);child->x_document=enumeration->x_document;child->x_index=index;node.guest=child->root;return child;};
                    auto* node=kind==Kind::XData?&document.nodes.at(object->x_index):nullptr;auto* payload=node && node->target!=UINT32_MAX?&document.nodes.at(node->target):node;
                    if(name=="GetFile" || name=="GetEnum"){output(1);auto owner=bridge.objects.at(object->parent);hold(owner);store_pointer(arg(1),owner->root);}
                    else if(name=="GetChildren"){require(arg(1),bad_value);memory.store(GuestAddress{arg(1)},pb,node?payload->children.size():document.roots.size());}
                    else if(name=="GetChild"){output(2);const auto& children=node?payload->children:document.roots;require(arg(1)<children.size(),not_found);store_pointer(arg(2),child_object(children[static_cast<std::size_t>(arg(1))])->root);}
                    else if(name=="GetDataObjectByName" || name=="GetDataObjectById"){
                        output(2);require(arg(1),bad_value);Guid requested{};std::string label;if(name.ends_with("Id"))memory.read(GuestAddress{arg(1)},requested);else label=read_ansi(memory,arg(1));unsigned found=UINT32_MAX;
                        for(unsigned i=0;i<document.nodes.size();++i){const auto& candidate=document.nodes[i];if(candidate.reference.empty() && (name.ends_with("Id")?candidate.id==requested:candidate.name==label)){found=i;break;}}
                        require(found!=UINT32_MAX,not_found);store_pointer(arg(2),child_object(found)->root);
                    }else if(name=="GetName"){
                        require(node && (arg(1) || arg(2)),bad_value);const auto required=payload->name.empty()?0:payload->name.size()+1;const auto capacity=arg(2)?memory.load(GuestAddress{arg(2)},pb):required;if(arg(2))memory.store(GuestAddress{arg(2)},pb,required);
                        if(arg(1) && required){require(capacity>=required,bad_value);memory.write(GuestAddress{arg(1)},std::span(reinterpret_cast<const std::uint8_t*>(payload->name.c_str()),required));}
                    }else if(name=="GetType" || name=="GetId"){require(node && arg(1),bad_value);memory.write(GuestAddress{arg(1)},name=="GetType"?payload->type_id:payload->id);}
                    else if(name=="IsReference"){require(node,bad_value);result=node->target!=UINT32_MAX;}
                    else if(name=="Lock"){
                        require(node && arg(1) && arg(2) && object->lock_count<UINT32_MAX,bad_value);
                        if(!object->mapped){const auto address=allocate_heap(memory,std::max<std::size_t>(payload->bytes.size(),8));require(address,out_of_memory);try{if(!payload->bytes.empty())memory.write(GuestAddress{address},payload->bytes);}catch(...){(void)free_heap(memory,address,state);throw;}object->mapped=address;}
                        ++object->lock_count;memory.store(GuestAddress{arg(1)},pb,payload->bytes.size());store_pointer(arg(2),object->mapped);
                    }else if(name=="Unlock"){require(node && object->lock_count,bad_value);if(!--object->lock_count){(void)free_heap(memory,object->mapped,state);object->mapped=0;}}
                    else require(false,0x80004001U);
                }
            }
        }

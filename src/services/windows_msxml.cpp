#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <algorithm>
#include <set>
#include <functional>

namespace x64arm {
namespace {
#include "msxml_vtables.inc"
constexpr std::uint32_t invalid=0x80070057U, pointer_error=0x80004003U, no_interface=0x80004002U;
struct XmlError {std::uint32_t code;};
void require(bool value,std::uint32_t code=invalid){if(!value)throw XmlError{code};}
std::string guid(const GuestMemory& memory,std::uint64_t address){
    if(!address)return {};constexpr char hex[]="0123456789abcdef";std::string result;
    for(unsigned offset:std::array<unsigned,16>{3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15}){
        if(result.size()==8 || result.size()==13 || result.size()==18 || result.size()==23)result+='-';
        const auto byte=memory.load(GuestAddress{address+offset},8);result+=hex[byte>>4];result+=hex[byte&15];
    }return result;
}
std::string node_name(xmlNodePtr node){
    switch(node->type){case XML_DOCUMENT_NODE:return "#document";case XML_TEXT_NODE:return "#text";case XML_COMMENT_NODE:return "#comment";
    case XML_CDATA_SECTION_NODE:return "#cdata-section";case XML_DOCUMENT_FRAG_NODE:return "#document-fragment";default:break;}
    std::string result;if(node->ns && node->ns->prefix){result=reinterpret_cast<const char*>(node->ns->prefix);result+=':';}
    if(node->name)result+=reinterpret_cast<const char*>(node->name);return result;
}
std::string content(xmlNodePtr node){const auto value=xmlNodeGetContent(node);std::string result=value?reinterpret_cast<const char*>(value):"";xmlFree(value);return result;}
struct Tree {
    xmlDocPtr doc{xmlNewDoc(BAD_CAST "1.0")};std::set<xmlNodePtr> detached;
    ~Tree(){for(auto node:detached)xmlFreeNode(node);xmlFreeDoc(doc);}
    Tree()=default;Tree(const Tree&)=delete;
};
}
struct MsxmlState {
    enum Kind {Node,Document,List,Element,Factory};
    struct Object {
        Kind kind{Node};unsigned references{1};std::shared_ptr<Tree> tree;xmlNodePtr node{};
        xmlNodePtr list_root{};std::string tag;bool descendants{};std::vector<xmlNodePtr> selected;bool xpath{};std::size_t cursor{};
        bool preserve{},async{true},validate{true},externals{};std::string url;
    };
    std::map<std::uint64_t,Object> objects;std::map<Kind,std::uint64_t> tables;unsigned server_locks{};
};
std::span<const WindowsServiceDescriptor> msxml_service_descriptors(){
    static const auto descriptors=[](){std::vector<WindowsServiceDescriptor> out;
        for(auto table:{std::span<const WindowsServiceDescriptor>(node_methods),std::span<const WindowsServiceDescriptor>(document_methods),std::span<const WindowsServiceDescriptor>(list_methods),std::span<const WindowsServiceDescriptor>(element_methods)})
            for(const auto& item:table)if(std::none_of(out.begin(),out.end(),[&](auto& current){return current.symbol==item.symbol;}))out.push_back(item);
        out.push_back({"internal-msxml","CreateInstance",4});out.push_back({"internal-msxml","LockServer",2});return out;
    }();return descriptors;
}
bool WindowsServices::begin_msxml_com(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto id=guid(memory,arg(0));
    // 版本無關 MSXML 2.x DOM；不同版本的 schema／安全預設不可任意混用。
    if(id!="2933bf90-7b36-11d2-b20e-00c04f983e60" && id!="2933bf91-7b36-11d2-b20e-00c04f983e60")return false;
    (void)invoke_msxml(state,memory,service);return true;
}
std::optional<std::uint32_t> WindowsServices::invoke_msxml(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    using Kind=MsxmlState::Kind;const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);std::uint32_t result=0;
    const auto output=[&](std::uint64_t at,std::uint64_t value){require(at,pointer_error);memory.store(GuestAddress{at},bits,value);};
    const auto text=[&](std::uint64_t at){return at?utf16_to_utf8(read_utf16(memory,GuestAddress{at},16*1024*1024)):std::string{};};
    const auto bstr=[&](std::uint64_t at,std::string_view value){const auto bytes=utf16_bytes(utf8_to_utf16(value));const auto pointer=allocate_bstr(memory,bytes.size()-2,std::span(bytes).first(bytes.size()-2));require(pointer,0x8007000eU);output(at,pointer);};
    const auto variant_text=[&](std::uint64_t at){require(at);const auto type=memory.load(GuestAddress{at},16);require(type==8,0x80020005U);return text(memory.load(GuestAddress{at+8},bits));};
    try{
        if(!msxml_)msxml_=std::make_shared<MsxmlState>();
        // 狀態複本隔離：每棵樹只複製一次，同一服務內的節點仍保持 COM 身分與別名。
        if(msxml_.use_count()>1){
            auto next=std::make_shared<MsxmlState>(*msxml_);std::map<const Tree*,std::shared_ptr<Tree>> trees;std::map<xmlNodePtr,xmlNodePtr> nodes;
            std::function<void(xmlNodePtr,xmlNodePtr)> pairs=[&](xmlNodePtr a,xmlNodePtr b){for(;a && b;a=a->next,b=b->next){nodes[a]=b;pairs(a->children,b->children);}};
            for(auto& [handle,o]:next->objects){(void)handle;if(!o.tree)continue;const auto old=o.tree.get();if(!trees.contains(old)){
                    auto replacement=std::make_shared<Tree>();xmlFreeDoc(replacement->doc);replacement->doc=xmlCopyDoc(old->doc,1);require(replacement->doc,0x8007000eU);
                    nodes[reinterpret_cast<xmlNodePtr>(old->doc)]=reinterpret_cast<xmlNodePtr>(replacement->doc);pairs(old->doc->children,replacement->doc->children);
                    for(auto detached:old->detached){auto copy=xmlDocCopyNode(detached,replacement->doc,1);require(copy,0x8007000eU);replacement->detached.insert(copy);pairs(detached,copy);}trees[old]=replacement;
                }o.tree=trees.at(old);if(o.node)o.node=nodes.at(o.node);if(o.list_root)o.list_root=nodes.at(o.list_root);for(auto& n:o.selected)n=nodes.at(n);
            }msxml_=std::move(next);
        }
        auto& bridge=*msxml_;
        const auto create=[&](MsxmlState::Object object){
            require(bridge.objects.size()<65536,0x8007000eU);
            if(!bridge.tables.contains(object.kind)){
                static constexpr WindowsServiceDescriptor factory_methods[]={{"internal-msxml","QueryInterface",3},{"internal-msxml","AddRef",1},{"internal-msxml","Release",1},{"internal-msxml","CreateInstance",4},{"internal-msxml","LockServer",2}};
                std::span<const WindowsServiceDescriptor> table=object.kind==Kind::Document?std::span<const WindowsServiceDescriptor>(document_methods):object.kind==Kind::List?std::span<const WindowsServiceDescriptor>(list_methods):object.kind==Kind::Element?std::span<const WindowsServiceDescriptor>(element_methods):object.kind==Kind::Factory?std::span<const WindowsServiceDescriptor>(factory_methods):std::span<const WindowsServiceDescriptor>(node_methods);
                const auto address=runtime_allocate(memory,{},table.size()*unit);for(unsigned i=0;i<table.size();++i)memory.store(GuestAddress{address+i*unit},bits,resolve(table[i].family,table[i].symbol).value);bridge.tables[object.kind]=address;
            }
            const auto pointer=allocate_heap(memory,unit);require(pointer,0x8007000eU);memory.store(GuestAddress{pointer},bits,bridge.tables.at(object.kind));bridge.objects.emplace(pointer,std::move(object));return pointer;
        };
        const auto node_object=[&](const std::shared_ptr<Tree>& tree,xmlNodePtr node)->std::uint64_t{
            if(!node)return 0;for(auto& [pointer,o]:bridge.objects)if(o.kind!=Kind::List && o.kind!=Kind::Factory && o.tree==tree && o.node==node){++o.references;return pointer;}
            MsxmlState::Object o;o.tree=tree;o.node=node;o.kind=node->type==XML_DOCUMENT_NODE?Kind::Document:node->type==XML_ELEMENT_NODE?Kind::Element:Kind::Node;return create(std::move(o));
        };
        const auto supported=[&](const MsxmlState::Object& o,const std::string& iid){
            if(iid=="00000000-0000-0000-c000-000000000046")return true;
            if(o.kind==Kind::Factory)return iid=="00000001-0000-0000-c000-000000000046";
            if(iid=="00020400-0000-0000-c000-000000000046")return true;
            if(o.kind==Kind::List)return iid=="2933bf82-7b36-11d2-b20e-00c04f983e60";
            if(iid=="2933bf80-7b36-11d2-b20e-00c04f983e60")return true;
            return (o.kind==Kind::Document && iid=="2933bf81-7b36-11d2-b20e-00c04f983e60") || (o.kind==Kind::Element && iid=="2933bf86-7b36-11d2-b20e-00c04f983e60");
        };
        if(name=="CoCreateInstance" || name=="CoGetClassObject" || name=="CreateInstance"){
            const bool factory=name=="CoGetClassObject",method=name=="CreateInstance";const auto out=arg(method?3:4);output(out,0);
            require(com_apartment(threads_.current).has_value(),0x800401f0U);
            if(!method)require((arg(factory?1:2)&1U)!=0,0x80040154U);
            if(!factory)require(!arg(1),0x80040110U);else require(!arg(2),0x80004001U);
            if(method){const auto found=bridge.objects.find(arg(0));require(found!=bridge.objects.end() && found->second.kind==Kind::Factory);}
            MsxmlState::Object o;o.kind=factory?Kind::Factory:Kind::Document;if(!factory){o.tree=std::make_shared<Tree>();require(o.tree->doc,0x8007000eU);o.node=reinterpret_cast<xmlNodePtr>(o.tree->doc);}
            require(supported(o,guid(memory,arg(method?2:3))),no_interface);output(out,create(std::move(o)));
        }else{
            const auto found=bridge.objects.find(arg(0));require(found!=bridge.objects.end());auto& o=found->second;const auto node=o.node;
            if(name=="QueryInterface"){output(arg(2),0);require(supported(o,guid(memory,arg(1))),no_interface);++o.references;output(arg(2),arg(0));}
            else if(name=="AddRef")result=++o.references;
            else if(name=="Release"){result=--o.references;if(!result){bridge.objects.erase(found);(void)free_heap(memory,arg(0),state);}}
            else if(name=="GetTypeInfoCount"){require(arg(1),pointer_error);memory.store(GuestAddress{arg(1)},32,0);}
            else if(name=="GetTypeInfo")throw XmlError{0x8002000bU};
            else if(name=="LockServer"){require(o.kind==Kind::Factory);if(arg(1))++bridge.server_locks;else if(bridge.server_locks)--bridge.server_locks;}
            else if(o.kind==Kind::List){
                std::vector<xmlNodePtr> nodes=o.selected;if(!o.xpath){nodes.clear();std::function<void(xmlNodePtr)> scan=[&](xmlNodePtr n){for(;n;n=n->next){if(o.tag.empty() || (n->type==XML_ELEMENT_NODE && (o.tag=="*" || node_name(n)==o.tag)))nodes.push_back(n);if(o.descendants)scan(n->children);}};scan(o.list_root->children);}
                if(name=="get_length"){require(arg(1),pointer_error);memory.store(GuestAddress{arg(1)},32,nodes.size());}
                else if(name=="reset")o.cursor=0;
                else if(name=="get_item" || name=="nextNode"){const auto index=name=="nextNode"?o.cursor++:static_cast<std::uint32_t>(arg(1));const auto at=arg(name=="nextNode"?1:2);output(at,index<nodes.size()?node_object(o.tree,nodes[index]):0);if(index>=nodes.size())result=1;}
                else throw HostCallFault("尚未實作 MSXML NodeList 方法："+std::string(name));
            }else if(name=="load" || name=="loadXML"){
                require(o.kind==Kind::Document);require(arg(2),pointer_error);memory.store(GuestAddress{arg(2)},16,0);
                std::string data;if(name=="loadXML")data=text(arg(1));else{
                    o.url=variant_text(arg(1));const auto file=filesystem_.open_reader(o.url);require(!file.error,0x80070000U|file.error);const auto info=file.file->information();require(!info.error && info.metadata.size<=32U*1024U*1024U);
                    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.metadata.size));const auto read=file.file->read(bytes,0);require(!read.error && read.bytes==bytes.size(),0x80004005U);data.assign(bytes.begin(),bytes.end());
                }
                auto next=std::make_shared<Tree>();const auto context=xmlNewParserCtxt();require(context,0x8007000eU);
                auto parsed=xmlCtxtReadMemory(context,data.data(),static_cast<int>(data.size()),nullptr,nullptr,XML_PARSE_NONET|XML_PARSE_NOERROR|XML_PARSE_NOWARNING|(o.preserve?0:XML_PARSE_NOBLANKS));xmlFreeParserCtxt(context);
                // 舊節點由各 COM 介面的 shared tree 保活；載入不使既有節點失效。
                if(parsed){xmlFreeDoc(next->doc);next->doc=parsed;}o.tree=std::move(next);o.node=reinterpret_cast<xmlNodePtr>(o.tree->doc);
                if(parsed)memory.store(GuestAddress{arg(2)},16,0xffff);else result=1;
            }else if(name=="createNode" || name=="createElement" || name=="createTextNode" || name=="createComment" || name=="createCDATASection" || name=="createDocumentFragment"){
                require(o.kind==Kind::Document);unsigned type=1;std::string value;std::uint64_t out=arg(2);std::string ns;
                if(name=="createNode"){require(memory.load(GuestAddress{arg(1)},16)==3);type=static_cast<unsigned>(memory.load(GuestAddress{arg(1)+8},32));value=text(arg(2));ns=text(arg(3));out=arg(4);}
                else if(name=="createDocumentFragment"){type=11;out=arg(1);}else{value=text(arg(1));type=name=="createElement"?1U:name=="createTextNode"?3U:name=="createComment"?8U:4U;}
                output(out,0);xmlNodePtr made=nullptr;
                if(type==1){require(!value.empty() && !xmlValidateQName(BAD_CAST value.c_str(),0));auto colon=value.find(':');auto local=colon==value.npos?value:value.substr(colon+1);made=xmlNewDocNode(o.tree->doc,nullptr,BAD_CAST local.c_str(),nullptr);
                    if(made && !ns.empty()){auto prefix=colon==value.npos?std::string{}:value.substr(0,colon);auto binding=xmlNewNs(made,BAD_CAST ns.c_str(),prefix.empty()?nullptr:BAD_CAST prefix.c_str());xmlSetNs(made,binding);}}
                else if(type==3)made=xmlNewDocText(o.tree->doc,BAD_CAST (name=="createNode"?"":value.c_str()));
                else if(type==4)made=xmlNewCDataBlock(o.tree->doc,BAD_CAST value.c_str(),static_cast<int>(value.size()));
                else if(type==8)made=xmlNewDocComment(o.tree->doc,BAD_CAST value.c_str());
                else if(type==11)made=xmlNewDocFragment(o.tree->doc);else throw XmlError{0x80004001U};
                require(made,0x8007000eU);o.tree->detached.insert(made);output(out,node_object(o.tree,made));
            }else if(name=="put_text" || name=="put_nodeValue" || name=="put_nodeTypedValue"){
                require(node->type!=XML_DOCUMENT_NODE && (name!="put_nodeValue" || node->type!=XML_ELEMENT_NODE),0x80004005U);
                const auto value=name=="put_text"?text(arg(1)):variant_text(arg(1));
                // libxml 的直接內容替換會釋放子節點；COM 子節點必須可繼續存活。
                while(node->children){auto child=node->children;xmlUnlinkNode(child);o.tree->detached.insert(child);}
                xmlNodeSetContent(node,BAD_CAST value.c_str());
            }else if(name=="appendChild" || name=="removeChild" || name=="replaceChild" || name=="insertBefore" || name=="put_documentElement"){
                require(node->type==XML_ELEMENT_NODE || node->type==XML_DOCUMENT_NODE || node->type==XML_DOCUMENT_FRAG_NODE);
                const auto child_it=bridge.objects.find(arg(1));require(child_it!=bridge.objects.end() && child_it->second.node && child_it->second.tree==o.tree);
                auto child=child_it->second.node;require(child->type!=XML_DOCUMENT_NODE);for(auto ancestor=node;ancestor;ancestor=ancestor->parent)require(ancestor!=child,0x80004005U);
                xmlNodePtr before=nullptr,old=nullptr;std::uint64_t out=0;
                if(name=="removeChild"){require(child->parent==node);old=child;out=arg(2);}
                else if(name=="replaceChild"){const auto other=bridge.objects.find(arg(2));require(other!=bridge.objects.end());old=other->second.node;require(old && old->parent==node);before=old;out=arg(3);}
                else if(name=="insertBefore"){const auto type=memory.load(GuestAddress{arg(2)},16);if(type==9 || type==13){const auto other=bridge.objects.find(memory.load(GuestAddress{arg(2)+8},bits));require(other!=bridge.objects.end());before=other->second.node;require(before && before->parent==node);}else require(type==0 || type==1);out=arg(3);}
                else if(name=="appendChild")out=arg(2);
                else {require(o.kind==Kind::Document && child->type==XML_ELEMENT_NODE);old=xmlDocGetRootElement(o.tree->doc);before=old;}
                if(out)output(out,0);
                if(name!="removeChild" && child!=before){
                    if(node->type==XML_DOCUMENT_NODE && child->type==XML_ELEMENT_NODE){auto root=xmlDocGetRootElement(o.tree->doc);require(!root || root==old || root==child,0x80004005U);}
                    xmlUnlinkNode(child);o.tree->detached.erase(child);
                    // 避免 xmlAddChild 自動合併文字節點而銷毀已暴露的 COM 物件。
                    child->parent=node;child->next=before;child->prev=before?before->prev:node->last;
                    if(child->prev)child->prev->next=child;else node->children=child;
                    if(before)before->prev=child;else node->last=child;
                }
                if(old && old!=child){xmlUnlinkNode(old);o.tree->detached.insert(old);}else if(name=="removeChild"){xmlUnlinkNode(child);o.tree->detached.insert(child);}
                if(out)output(out,node_object(o.tree,old?old:child));
            }else if(name=="cloneNode"){
                auto copy=xmlDocCopyNode(node,o.tree->doc,arg(1)?1:2);require(copy,0x8007000eU);o.tree->detached.insert(copy);output(arg(2),node_object(o.tree,copy));
            }else if(name=="getAttribute" || name=="setAttribute" || name=="removeAttribute"){
                require(node->type==XML_ELEMENT_NODE);const auto key=text(arg(1));require(!xmlValidateQName(BAD_CAST key.c_str(),0));
                if(name=="getAttribute"){require(arg(2),pointer_error);memory.write(GuestAddress{arg(2)},std::vector<std::uint8_t>(x86_?16:24));auto value=xmlGetProp(node,BAD_CAST key.c_str());memory.store(GuestAddress{arg(2)},16,value?8:1);if(value){const std::string copy(reinterpret_cast<const char*>(value));xmlFree(value);bstr(arg(2)+8,copy);}else result=1;}
                else if(name=="setAttribute"){const auto value=variant_text(arg(2));require(xmlSetProp(node,BAD_CAST key.c_str(),BAD_CAST value.c_str()),0x8007000eU);}
                else if(xmlUnsetProp(node,BAD_CAST key.c_str()))result=1;
            }else if(name=="get_nodeName" || name=="get_tagName")bstr(arg(1),node_name(node));
            else if(name=="get_nodeType"){require(arg(1),pointer_error);memory.store(GuestAddress{arg(1)},32,node->type);}
            else if(name=="get_text")bstr(arg(1),content(node));
            else if(name=="get_nodeValue" || name=="get_nodeTypedValue" || name=="get_dataType"){
                require(arg(1),pointer_error);memory.write(GuestAddress{arg(1)},std::vector<std::uint8_t>(x86_?16:24));
                const bool null=name=="get_dataType" || node->type==XML_DOCUMENT_NODE || (name=="get_nodeValue" && node->type==XML_ELEMENT_NODE);
                memory.store(GuestAddress{arg(1)},16,null?1:8);if(!null)bstr(arg(1)+8,content(node));
            }else if(name=="get_firstChild" || name=="get_lastChild" || name=="get_parentNode" || name=="get_previousSibling" || name=="get_nextSibling" || name=="get_ownerDocument" || name=="get_documentElement" || name=="get_doctype"){
                const auto target=name=="get_firstChild"?node->children:name=="get_lastChild"?node->last:name=="get_parentNode"?node->parent:name=="get_previousSibling"?node->prev:name=="get_nextSibling"?node->next:name=="get_ownerDocument"?(o.kind==Kind::Document?nullptr:reinterpret_cast<xmlNodePtr>(o.tree->doc)):name=="get_doctype"?reinterpret_cast<xmlNodePtr>(o.tree->doc->intSubset):xmlDocGetRootElement(o.tree->doc);
                output(arg(1),node_object(o.tree,target));if(!target)result=1;
            }else if(name=="get_childNodes" || name=="getElementsByTagName" || name=="selectNodes" || name=="selectSingleNode"){
                MsxmlState::Object list;list.kind=Kind::List;list.tree=o.tree;list.list_root=node;list.descendants=name=="getElementsByTagName";
                if(list.descendants)list.tag=text(arg(1));
                if(name.starts_with("select")){
                    list.xpath=true;auto context=xmlXPathNewContext(o.tree->doc);require(context,0x8007000eU);context->node=node;const auto query=text(arg(1));auto selected=xmlXPathEvalExpression(BAD_CAST query.c_str(),context);
                    if(selected && selected->type==XPATH_NODESET && selected->nodesetval)for(int i=0;i<selected->nodesetval->nodeNr;++i)list.selected.push_back(selected->nodesetval->nodeTab[i]);
                    const bool valid=selected && selected->type==XPATH_NODESET;xmlXPathFreeObject(selected);xmlXPathFreeContext(context);require(valid,0x80004005U);
                }
                if(name=="selectSingleNode"){output(arg(2),list.selected.empty()?0:node_object(o.tree,list.selected.front()));if(list.selected.empty())result=1;}else output(arg(name=="get_childNodes"?1:2),create(std::move(list)));
            }else if(name=="hasChildNodes" || name=="get_parsed" || name=="get_specified"){
                require(arg(1),pointer_error);memory.store(GuestAddress{arg(1)},16,(name=="hasChildNodes"?!node->children:false)?0:0xffff);
            }else if(name=="get_readyState"){require(arg(1),pointer_error);memory.store(GuestAddress{arg(1)},32,4);}
            else if(name=="get_url")bstr(arg(1),o.url);
            else if(name=="get_namespaceURI" || name=="get_prefix" || name=="get_baseName"){
                const xmlChar* value=name=="get_baseName"?node->name:node->ns?(name=="get_prefix"?node->ns->prefix:node->ns->href):nullptr;bstr(arg(1),value?reinterpret_cast<const char*>(value):"");
            }else if(name=="get_async" || name=="get_validateOnParse" || name=="get_resolveExternals" || name=="get_preserveWhiteSpace" || name=="put_async" || name=="put_validateOnParse" || name=="put_resolveExternals" || name=="put_preserveWhiteSpace"){
                bool& field=name.ends_with("async")?o.async:name.ends_with("validateOnParse")?o.validate:name.ends_with("resolveExternals")?o.externals:o.preserve;
                if(name.starts_with("put_")){if(name=="put_resolveExternals" && arg(1))throw XmlError{0x80004001U};field=static_cast<std::uint16_t>(arg(1))!=0;}else{require(arg(1),pointer_error);memory.store(GuestAddress{arg(1)},16,field?0xffff:0);}
            }else if(name=="get_xml" || name=="save"){
                const auto buffer=xmlBufferCreate();require(buffer,0x8007000eU);xmlNodeDump(buffer,o.tree->doc,node,0,0);std::string xml(reinterpret_cast<const char*>(xmlBufferContent(buffer)),static_cast<std::size_t>(xmlBufferLength(buffer)));xmlBufferFree(buffer);
                if(name=="get_xml")bstr(arg(1),xml);else{const auto path=variant_text(arg(1));const auto file=filesystem_.open_host(path,0x40000000U,2,0x80,1);require(!file.error,0x80070000U|file.error);const auto io=file.file->write(std::span(reinterpret_cast<const std::uint8_t*>(xml.data()),xml.size()),0);require(!io.error && io.bytes==xml.size(),0x80004005U);}
            }else throw HostCallFault("尚未實作 MSXML DOM 方法："+std::string(name));
        }
    }catch(const XmlError& error){result=error.code;}catch(const std::bad_alloc&){result=0x8007000eU;}
    arg.return_to(state,result);return std::nullopt;
}
}

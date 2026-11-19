// 由 windows_url_security.cpp 的 invoke_url_security 引入：urlmon 的 IUri 與 IUriBuilder（urlmon_uri_parse.hpp 的元件模型）。
// CreateUri／CreateUriWithFragment 解析並正規化；IUri 的屬性依 Uri_PROPERTY 編號，沒有的字串屬性回 S_FALSE 與空 BSTR；
// CreateIUriBuilder 複製元件後可逐項修改，Create* 依元件組回字串再解析；CoInternetParseIUri／CombineIUri／CombineUrlEx 沿用同檔的 CoInternetParseUrl 與 shlwapi UrlCombineW。
if(name=="CreateUri" || name=="CreateUriWithFragment" || name=="CreateIUriBuilder" || name=="CoInternetParseIUri" || name=="CoInternetCombineIUri" || name=="CoInternetCombineUrlEx" ||
   service.family=="internal-uri" || service.family=="internal-uri-builder"){
    constexpr std::uint32_t s_false=1,e_fail=0x80004005U,invalid_url=0x800C0002U;
    if(!uri_state_)uri_state_=std::make_shared<UriState>();auto& uris=*uri_state_;
    const auto table_of=[&](std::string_view family)->std::uint64_t{
        auto& table=family=="internal-uri"?uris.table:uris.builder_table;if(table)return table;unsigned count=0;
        for(const auto& row:url_security_service_descriptors())if(row.family==family)++count;
        const auto storage=runtime_allocate(memory,{},(count+1)*unit);table=storage+unit;unsigned i=0;
        for(const auto& row:url_security_service_descriptors())if(row.family==family)memory.store(GuestAddress{table+unit*i++},bits,resolve(row.family,row.symbol).value);
        return table;
    };
    const auto string_at=[&](std::uint64_t at){return read_utf16(memory,GuestAddress{at},32768);};
    const auto make_uri=[&](uri_model::Uri components)->std::uint64_t{
        if(uris.objects.size()>=65536)return 0;const auto object=allocate_heap(memory,unit);if(!object)return 0;
        memory.store(GuestAddress{object},bits,table_of("internal-uri"));uris.objects.emplace(object,UriObject{1,std::move(components)});return object;
    };
    const auto release_uri=[&](std::uint64_t object){
        const auto found=uris.objects.find(object);if(found==uris.objects.end())return 0U;if(--found->second.references)return found->second.references;
        uris.objects.erase(found);(void)free_heap(memory,object,state);return 0U;
    };
    // Uri_CREATE_*：不能同時要求互斥的選項。
    const auto valid_flags=[](std::uint32_t f){
        if(f&~0x3ffffU)return false;
        const std::array<std::pair<std::uint32_t,std::uint32_t>,5> pairs{{{0x10,0x100},{0x40,0x80},{0x200,0x400},{0x800,0x1000},{0x2000,0x4000}}};
        for(const auto& [a,b]:pairs)if((f&a) && (f&b))return false;return !((f&0x1) && (f&0x2));
    };
    const auto create_from=[&](const std::u16string& text,std::uint32_t flags,std::uint64_t output)->std::uint32_t{
        auto parsed=uri_model::parse(text,flags);if(!parsed)return invalid;const auto object=make_uri(std::move(*parsed));if(!object)return no_memory;
        memory.store(GuestAddress{output},bits,object);return 0;
    };
    // 字串屬性（Uri_PROPERTY 0～14）；沒有時為 nullopt。
    const auto text_property=[&](const uri_model::Uri& u,std::uint32_t id,std::uint32_t flags)->std::optional<std::u16string>{
        std::u16string value;
        switch(id){
        case 0:if(u.relative)return std::nullopt;return uri_model::compose(u,true,true);
        case 1:if(!u.authority)return std::nullopt;value=uri_model::authority(u,true);break;
        case 2:if(u.relative)return std::nullopt;return uri_model::compose(u,false,!(flags&1U));
        case 3:value=uri_model::domain(u);break;
        case 4:value=uri_model::extension(u);break;
        case 5:value=u.fragment;break;
        case 6:value=u.host;break;
        case 7:if(!u.has_password)return std::nullopt;value=u.password;break;
        case 8:value=u.path;break;
        case 9:value=u.path+u.query;break;
        case 10:value=u.query;break;
        case 11:return u.raw;
        case 12:value=u.scheme;break;
        case 13:if(!u.has_user && !u.has_password)return std::nullopt;value=uri_model::user_info(u);break;
        case 14:if(!u.has_user)return std::nullopt;value=u.user;break;
        default:return std::nullopt;
        }
        if(value.empty())return std::nullopt;return value;
    };
    const auto dword_property=[&](const uri_model::Uri& u,std::uint32_t id)->std::optional<std::uint32_t>{
        if(id==15)return u.host.empty()?std::nullopt:std::optional<std::uint32_t>{u.host_type};
        if(id==16)return u.has_port?std::optional<std::uint32_t>{u.port}:std::nullopt;
        if(id==17)return u.relative?std::nullopt:std::optional<std::uint32_t>{u.scheme_id};
        if(id==18)return u.relative?std::nullopt:std::optional<std::uint32_t>{url_zone(uri_model::compose(u,true,true))};
        return std::nullopt;
    };
    const auto store_bstr=[&](std::uint64_t output,const std::u16string& value)->std::uint32_t{
        auto bytes=utf16_bytes(value);bytes.resize(bytes.size()-2);const auto bstr=allocate_bstr(memory,bytes.size(),bytes);if(!bstr)return no_memory;
        memory.store(GuestAddress{output},bits,bstr);return 0;
    };
    const auto query_interface=[&](std::uint64_t object,std::array<std::uint8_t,16> wanted,std::uint32_t& references)->std::uint32_t{
        if(!arg(2))return pointer_error;memory.store(GuestAddress{arg(2)},bits,0);if(!arg(1))return pointer_error;
        std::array<std::uint8_t,16> iid{};memory.read(GuestAddress{arg(1)},iid);constexpr std::array<std::uint8_t,16> unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
        if(iid!=unknown && iid!=wanted)return no_interface;++references;memory.store(GuestAddress{arg(2)},bits,object);return 0;
    };
    if(service.family=="internal-uri"){
        const auto found=uris.objects.find(arg(0));if(found==uris.objects.end())return finish(invalid);auto& object=found->second;const auto& u=object.uri;
        if(name=="QueryInterface")return finish(query_interface(arg(0),{0x48,0xe7,0x9e,0xa3,0x27,0x6a,0x17,0x48,0xa6,0xf2,0x13,0x91,0x4b,0xef,0x58,0x90},object.references));
        if(name=="AddRef")return finish(++object.references);
        if(name=="Release")return finish(release_uri(arg(0)));
        if(name=="GetPropertyBSTR" || name=="GetPropertyLength" || name=="GetPropertyDWORD"){
            // (this, Uri_PROPERTY, 輸出, dwFlags)
            const auto id=static_cast<std::uint32_t>(arg(1)),flags=static_cast<std::uint32_t>(arg(3));if(!arg(2))return finish(pointer_error);
            if(name=="GetPropertyDWORD"){if(id<15 || id>18)return finish(invalid);memory.store(GuestAddress{arg(2)},32,0);const auto value=dword_property(u,id);if(!value)return finish(s_false);memory.store(GuestAddress{arg(2)},32,*value);return finish(0);}
            if(id>14)return finish(invalid);if(flags&~7U)return finish(invalid);const auto value=text_property(u,id,flags);
            if(name=="GetPropertyLength"){memory.store(GuestAddress{arg(2)},32,value?value->size():0);return finish(value?0:s_false);}
            memory.store(GuestAddress{arg(2)},bits,0);const auto status=store_bstr(arg(2),value.value_or(std::u16string{}));return finish(status?status:value?0:s_false);
        }
        if(name=="HasProperty"){
            const auto id=static_cast<std::uint32_t>(arg(1));if(!arg(2))return finish(pointer_error);if(id>18)return finish(invalid);
            memory.store(GuestAddress{arg(2)},32,id<15?text_property(u,id,0).has_value():dword_property(u,id).has_value());return finish(0);
        }
        if(name=="GetProperties"){
            if(!arg(1))return finish(pointer_error);std::uint32_t mask=0;
            for(std::uint32_t id=0;id<15;++id)if(text_property(u,id,0))mask|=1U<<id;for(std::uint32_t id=15;id<19;++id)if(dword_property(u,id))mask|=1U<<id;
            memory.store(GuestAddress{arg(1)},32,mask);return finish(0);
        }
        if(name=="IsEqual"){
            if(!arg(2))return finish(pointer_error);memory.store(GuestAddress{arg(2)},32,0);if(!arg(1))return finish(0);
            const auto other=uris.objects.find(arg(1));if(other==uris.objects.end())return finish(0);
            memory.store(GuestAddress{arg(2)},32,uri_model::compose(u,true,true)==uri_model::compose(other->second.uri,true,true) && u.relative==other->second.uri.relative);return finish(0);
        }
        // GetAbsoluteUri…GetUserName（Uri_PROPERTY 0～14）與 GetHostType／GetPort／GetScheme／GetZone（15～18）。
        constexpr std::array<std::string_view,19> methods{"GetAbsoluteUri","GetAuthority","GetDisplayUri","GetDomain","GetExtension","GetFragment","GetHost","GetPassword",
            "GetPath","GetPathAndQuery","GetQuery","GetRawUri","GetSchemeName","GetUserInfo","GetUserName","GetHostType","GetPort","GetScheme","GetZone"};
        const auto at=std::find(methods.begin(),methods.end(),std::string_view(name));if(at==methods.end())throw HostCallFault("未知的 IUri 方法："+std::string(name));
        const auto id=static_cast<std::uint32_t>(at-methods.begin());if(!arg(1))return finish(pointer_error);
        if(id>=15){memory.store(GuestAddress{arg(1)},32,0);const auto value=dword_property(u,id);if(!value)return finish(s_false);memory.store(GuestAddress{arg(1)},32,*value);return finish(0);}
        memory.store(GuestAddress{arg(1)},bits,0);const auto value=text_property(u,id,0);const auto status=store_bstr(arg(1),value.value_or(std::u16string{}));return finish(status?status:value?0:s_false);
    }
    if(service.family=="internal-uri-builder"){
        const auto found=uris.builders.find(arg(0));if(found==uris.builders.end())return finish(invalid);auto& builder=found->second;
        const auto free_strings=[&](){for(const auto& [id,pointer]:builder.strings){(void)id;(void)free_heap(memory,pointer,state);}builder.strings.clear();};
        if(name=="QueryInterface")return finish(query_interface(arg(0),{0xe1,0xb2,0x21,0x42,0x55,0x89,0xc0,0x46,0xbd,0x5b,0xde,0x98,0x97,0x56,0x5d,0xe7},builder.references));
        if(name=="AddRef")return finish(++builder.references);
        if(name=="Release"){
            if(--builder.references)return finish(builder.references);free_strings();const auto base=builder.base;uris.builders.erase(found);(void)free_heap(memory,arg(0),state);
            if(base)(void)release_uri(base);return finish(0);
        }
        auto& u=builder.uri;
        const std::array<std::pair<std::string_view,std::uint32_t>,7> fields{{{"Fragment",5},{"Host",6},{"Password",7},{"Path",8},{"Query",10},{"SchemeName",12},{"UserName",14}}};
        const auto field=[&](std::uint32_t id)->std::u16string*{switch(id){case 5:return &u.fragment;case 6:return &u.host;case 7:return &u.password;case 8:return &u.path;case 10:return &u.query;case 12:return &u.scheme;case 14:return &u.user;default:return nullptr;}};
        if(name=="HasBeenModified"){if(!arg(1))return finish(pointer_error);memory.store(GuestAddress{arg(1)},32,builder.modified);return finish(0);}
        if(name=="GetIUri"){if(!arg(1))return finish(pointer_error);memory.store(GuestAddress{arg(1)},bits,builder.base);if(builder.base)++uris.objects.at(builder.base).references;return finish(0);}
        if(name=="SetIUri"){
            uri_model::Uri copy;if(arg(1)){const auto other=uris.objects.find(arg(1));if(other==uris.objects.end())return finish(invalid);copy=other->second.uri;++other->second.references;}
            const auto previous=builder.base;builder.base=arg(1);builder.uri=std::move(copy);builder.modified=false;free_strings();if(previous)(void)release_uri(previous);return finish(0);
        }
        if(name=="GetPort"){if(!arg(1) || !arg(2))return finish(pointer_error);memory.store(GuestAddress{arg(1)},32,u.explicit_port);memory.store(GuestAddress{arg(2)},32,u.explicit_port?u.port:0);return finish(0);}
        if(name=="SetPort"){u.explicit_port=u.has_port=static_cast<std::uint32_t>(arg(1))!=0;u.port=u.explicit_port?static_cast<std::uint32_t>(arg(2)):0;builder.modified=true;return finish(0);}
        if(name=="RemoveProperties"){
            const auto mask=static_cast<std::uint32_t>(arg(1));constexpr std::uint32_t allowed=(1U<<5)|(1U<<6)|(1U<<7)|(1U<<8)|(1U<<10)|(1U<<14)|(1U<<16);if(mask&~allowed)return finish(invalid);
            for(std::uint32_t id=0;id<15;++id)if((mask&(1U<<id)) && field(id)){field(id)->clear();if(id==7)u.has_password=false;if(id==14)u.has_user=false;}
            if(mask&(1U<<16))u.explicit_port=u.has_port=false;if(mask)builder.modified=true;return finish(0);
        }
        for(const auto& [suffix,id]:fields){
            if(name=="Get"+std::string(suffix)){
                // (this, DWORD* 長度, LPCWSTR* 字串)：字串屬於建構器，修改或釋放前有效；沒有值時為 NULL。
                if(!arg(1) || !arg(2))return finish(pointer_error);const auto& value=*field(id);
                if(const auto old=builder.strings.find(id);old!=builder.strings.end()){(void)free_heap(memory,old->second,state);builder.strings.erase(old);}
                memory.store(GuestAddress{arg(1)},32,value.size());if(value.empty()){memory.store(GuestAddress{arg(2)},bits,0);return finish(0);}
                const auto bytes=utf16_bytes(value);const auto at=allocate_heap(memory,bytes.size());if(!at)return finish(no_memory);memory.write(GuestAddress{at},bytes);builder.strings[id]=at;
                memory.store(GuestAddress{arg(2)},bits,at);return finish(0);
            }
            if(name=="Set"+std::string(suffix)){
                // NULL 移除屬性；scheme 與主機不能移除。片段與查詢補上「#」「?」。
                if(!arg(1)){if(id==12 || id==6)return finish(invalid);field(id)->clear();if(id==7)u.has_password=false;if(id==14)u.has_user=false;builder.modified=true;return finish(0);}
                auto value=string_at(arg(1));if(id==12)value=uri_model::lowered(value);if(id==6)value=uri_model::lowered(value);
                if(id==5 && !value.empty() && value[0]!=u'#')value=u"#"+value;if(id==10 && !value.empty() && value[0]!=u'?')value=u"?"+value;
                *field(id)=value;if(id==7)u.has_password=true;if(id==14)u.has_user=true;if(id==6){u.authority=true;u.host_type=value.empty()?0:uri_model::ipv4(value)?2:1;}
                if(id==12){const auto* known=uri_model::find_scheme(value);u.scheme_id=known?known->id:0;u.relative=false;}
                builder.modified=true;return finish(0);
            }
        }
        if(name=="CreateUriSimple" || name=="CreateUri" || name=="CreateUriWithFlags"){
            const unsigned out=name=="CreateUriSimple"?3:name=="CreateUri"?4:5;if(!arg(out))return finish(pointer_error);memory.store(GuestAddress{arg(out)},bits,0);
            auto flags=name=="CreateUriSimple"?0xffffffffU:static_cast<std::uint32_t>(arg(1));const auto base_flags=builder.base?uris.objects.at(builder.base).uri.flags:0U;
            if(flags==0xffffffffU)flags=base_flags;
            if(!builder.modified && builder.base && flags==base_flags){++uris.objects.at(builder.base).references;memory.store(GuestAddress{arg(out)},bits,builder.base);return finish(0);}
            if(u.scheme.empty() && !(flags&uri_model::allow_relative))return finish(invalid_url);
            auto copy=u;copy.relative=u.scheme.empty();copy.authority=copy.authority || !copy.host.empty();
            if(copy.authority && !copy.path.empty() && copy.path[0]!=u'/')copy.path=u"/"+copy.path;
            if(!valid_flags(flags))return finish(invalid);const auto status=create_from(uri_model::compose(copy,true,true),flags,arg(out));return finish(status==invalid?invalid_url:status);
        }
        throw HostCallFault("未知的 IUriBuilder 方法："+std::string(name));
    }
    if(name=="CreateUri" || name=="CreateUriWithFragment"){
        // CreateUri(pwzURI, dwFlags, dwReserved, ppURI)；CreateUriWithFragment(pwzURI, pwzFragment, dwFlags, dwReserved, ppURI)。
        const bool fragment=name=="CreateUriWithFragment";const unsigned shift=fragment?1:0;const auto output=arg(3+shift);
        if(!output)return finish(invalid);memory.store(GuestAddress{output},bits,0);
        const auto flags=static_cast<std::uint32_t>(arg(1+shift));if(!arg(0) || arg(2+shift) || !valid_flags(flags))return finish(invalid);
        auto text=string_at(arg(0));
        if(fragment && arg(1)){
            auto piece=string_at(arg(1));if(text.find(u'#')!=text.npos)return finish(invalid);if(!piece.empty() && piece[0]!=u'#')piece=u"#"+piece;text+=piece;
        }
        return finish(create_from(text,flags,output));
    }
    if(name=="CreateIUriBuilder"){
        // (pIUri, dwFlags, dwReserved, ppIUriBuilder)
        if(!arg(3))return finish(pointer_error);memory.store(GuestAddress{arg(3)},bits,0);if(static_cast<std::uint32_t>(arg(1)) || arg(2))return finish(invalid);
        UriBuilderObject builder;if(arg(0)){const auto other=uris.objects.find(arg(0));if(other==uris.objects.end())return finish(invalid);builder.uri=other->second.uri;builder.base=arg(0);++other->second.references;}
        else builder.uri.relative=true;
        const auto object=allocate_heap(memory,unit);if(!object){if(builder.base)(void)release_uri(builder.base);return finish(no_memory);}
        memory.store(GuestAddress{object},bits,table_of("internal-uri-builder"));uris.builders.emplace(object,std::move(builder));memory.store(GuestAddress{arg(3)},bits,object);return finish(0);
    }
    // 下面三個以 IUri 的絕對（或原始）字串轉交既有的字串版本。
    GuestScratch temporary(*this,memory,state);
    const auto put_text=[&](const std::u16string& text){const auto bytes=utf16_bytes(text);const auto at=temporary.allocate(bytes.size());if(at)memory.write(GuestAddress{at},bytes);return at;};
    const auto source_of=[&](std::uint64_t object)->std::optional<uri_model::Uri>{const auto found=uris.objects.find(object);if(found==uris.objects.end())return std::nullopt;return found->second.uri;};
    if(name=="CoInternetParseIUri"){
        // (pIUri, ParseAction, dwFlags, pwzResult, cchResult, pcchResult, dwReserved)
        const auto source=source_of(arg(0));if(!source || arg(6))return finish(invalid);
        const auto text=put_text(source->relative?source->raw:uri_model::compose(*source,true,true));if(!text)return finish(no_memory);
        return finish(static_cast<std::uint32_t>(nested_service(state,memory,"urlmon.dll","CoInternetParseUrl",{text,arg(1),arg(2),arg(3),arg(4),arg(5),0})));
    }
    // CoInternetCombineIUri(pBaseUri, pRelativeUri, dwCombineFlags, ppCombinedUri, dwReserved)、CoInternetCombineUrlEx(pBaseUri, pwzRelativeUrl, …)
    if(!arg(3))return finish(invalid);memory.store(GuestAddress{arg(3)},bits,0);if(arg(4))return finish(invalid);
    const auto base=source_of(arg(0));if(!base)return finish(invalid);std::u16string relative;
    if(name=="CoInternetCombineIUri"){const auto other=source_of(arg(1));if(!other)return finish(invalid);relative=other->raw;}else{if(!arg(1))return finish(invalid);relative=string_at(arg(1));}
    const auto base_text=put_text(base->relative?base->raw:uri_model::compose(*base,true,true)),relative_text=put_text(relative),result=temporary.allocate(2*2084),count=temporary.allocate(4);
    if(!base_text || !relative_text || !result || !count)return finish(no_memory);memory.store(GuestAddress{count},32,2084);
    const auto status=static_cast<std::uint32_t>(nested_service(state,memory,"shlwapi.dll","UrlCombineW",{base_text,relative_text,result,count,static_cast<std::uint32_t>(arg(2))&0x7fffffffU}));
    if(status)return finish(status==pointer_error?e_fail:status);
    return finish(create_from(read_utf16(memory,GuestAddress{result},2084),base->flags,arg(3)));
}

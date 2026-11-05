if(service.family=="msvcp90.dll")if(const auto* entry=msvcp90_string::binding(symbol)){
    using namespace msvcp90_string;
    const unsigned p=x86_?4U:8U,e=entry->element;
    const auto bits=static_cast<std::uint8_t>(p*8),cbits=static_cast<std::uint8_t>(e*8);
    const std::uint64_t npos=x86_?UINT32_MAX:UINT64_MAX,limit=npos/e-1,small=16/e-1;
    const auto pointer=[&](std::uint64_t at){return memory.load(GuestAddress{at},bits);};
    const auto put=[&](std::uint64_t at,std::uint64_t value){memory.store(GuestAddress{at},bits,value);};
    const auto size=[&](std::uint64_t o){return pointer(o+p+16);};
    const auto capacity=[&](std::uint64_t o){return pointer(o+2*p+16);};
    const auto data=[&](std::uint64_t o){return capacity(o)>small?pointer(o+p):o+p;};
    const auto self=entry->member?(x86_?state.reg(Gpr::Rcx)&UINT32_MAX:arg(0)):0;
    const auto a=[&](unsigned i){return arg(i+(!x86_ && entry->member?1U:0U));};
    const auto done=[&](std::uint64_t value)->std::optional<std::uint32_t>{arg.return_to(state,value);return {};};
    const auto length_error=[] {throw MsvcExceptionKind::LengthError;};
    const auto range_error=[] {throw MsvcExceptionKind::OutOfRange;};
    using Text=std::vector<std::uint16_t>;
    try {
        const auto read=[&](std::uint64_t at,std::uint64_t n){
            if(n>limit)length_error();memory.check(GuestAddress{at},static_cast<std::size_t>(n*e),Access::Read);
            Text text(static_cast<std::size_t>(n));for(std::size_t i=0;i<text.size();++i)text[i]=static_cast<std::uint16_t>(memory.load(GuestAddress{at+i*e},cbits));return text;
        };
        const auto text=[&](std::uint64_t at){Text value;for(;;){const auto c=memory.load(GuestAddress{at+value.size()*e},cbits);if(!c)break;if(value.size()==limit)length_error();value.push_back(static_cast<std::uint16_t>(c));}return value;};
        const auto contents=[&](std::uint64_t o){const auto n=size(o);if(n>capacity(o))throw HostCallFault("VC9 字串長度超出容量");return read(data(o),n);};
        const auto sub=[&](Text value,std::uint64_t pos,std::uint64_t count){if(pos>value.size())range_error();count=std::min<std::uint64_t>(count,value.size()-pos);return Text(value.begin()+static_cast<std::ptrdiff_t>(pos),value.begin()+static_cast<std::ptrdiff_t>(pos+count));};
        const auto initialize=[&](std::uint64_t o){memory.check(GuestAddress{o},16+3*p,Access::Write);put(o,0);memory.fill(GuestAddress{o+p},0,16);put(o+p+16,0);put(o+2*p+16,small);};
        const auto release=[&](std::uint64_t at){if(at && !free_heap(memory,at,state))throw HostCallFault("VC9 字串不是有效 CRT 配置");};
        const auto store=[&](std::uint64_t o,const Text& value,std::uint64_t requested=0,bool trim=false){
            const auto n=static_cast<std::uint64_t>(value.size());requested=std::max(requested,n);if(requested>limit)length_error();
            memory.check(GuestAddress{o},16+3*p,Access::Write);
            const auto oldcap=capacity(o),oldptr=oldcap>small?pointer(o+p):0;
            auto cap=oldcap,destination=oldptr?oldptr:o+p;
            const bool inline_result=trim && requested<=small;
            if(inline_result){cap=small;destination=o+p;}
            else if(requested>oldcap){
                const auto rounded=requested|small;
                cap=std::max(rounded<=limit?rounded:requested,oldcap<=limit-oldcap/2?oldcap+oldcap/2:requested);
                if(cap>limit)cap=requested;
                destination=allocate_heap(memory,(cap+1)*e);if(!destination)throw MsvcExceptionKind::BadAlloc;
            }
            try {
                std::vector<std::uint8_t> bytes((value.size()+1)*e);
                for(std::size_t i=0;i<value.size();++i){bytes[i*e]=static_cast<std::uint8_t>(value[i]);if(e==2)bytes[i*e+1]=static_cast<std::uint8_t>(value[i]>>8);}
                memory.write(GuestAddress{destination},bytes);
            }catch(...){if(destination!=oldptr && destination!=o+p)release(destination);throw;}
            if(cap>small)put(o+p,destination);
            put(o+p+16,n);put(o+2*p+16,cap);
            if(oldptr && oldptr!=destination)release(oldptr);
        };
        const auto iterator=[&](std::uint64_t value){return x86_?std::array<std::uint64_t,2>{value&UINT32_MAX,value>>32}:std::array<std::uint64_t,2>{pointer(value),pointer(value+p)};};
        const auto position=[&](std::uint64_t value,std::uint64_t object){const auto it=iterator(value);const auto begin=data(object);if(it[0]!=object || it[1]<begin || (it[1]-begin)%e || (it[1]-begin)/e>size(object))range_error();return (it[1]-begin)/e;};
        const auto input=[&](Input kind,unsigned index){
            switch(kind){
            case Input::None:return Text{};
            case Input::Text:return text(a(index));
            case Input::Span:return read(a(index),a(index+1));
            case Input::String:return contents(a(index));
            case Input::Substring:return sub(contents(a(index)),a(index+1),a(index+2));
            case Input::Fill:if(a(index)>limit)length_error();return Text(static_cast<std::size_t>(a(index)),static_cast<std::uint16_t>(a(index+1)&(e==1?255U:65535U)));
            case Input::Character:return Text{static_cast<std::uint16_t>(a(index)&(e==1?255U:65535U))};
            case Input::Range:if(a(index+1)<a(index) || (a(index+1)-a(index))%e)range_error();return read(a(index),(a(index+1)-a(index))/e);
            case Input::Iterators:{const auto first=iterator(a(index)),last=iterator(a(index+1));if(first[0]!=last[0])range_error();const auto from=position(a(index),first[0]),to=position(a(index+1),first[0]);if(to<from)range_error();return read(first[1],to-from);}
            }throw HostCallFault("VC9 字串輸入型態無效");
        };
        const auto write_iterator=[&](std::uint64_t output,std::uint64_t object,std::uint64_t offset){memory.check(GuestAddress{output},2*p,Access::Write);put(output,object);put(output+p,data(object)+offset*e);return output;};
        const auto compare=[](const Text& lhs,const Text& rhs){return std::lexicographical_compare(lhs.begin(),lhs.end(),rhs.begin(),rhs.end())?-1:std::lexicographical_compare(rhs.begin(),rhs.end(),lhs.begin(),lhs.end())?1:0;};
        switch(entry->op){
        case Op::Construct:{auto value=input(entry->input,0);initialize(self);store(self,value);return done(self);}
        case Op::Destroy:{const auto old=capacity(self)>small?data(self):0;initialize(self);release(old);return done(self);}
        case Op::Size:return done(size(self));
        case Op::Capacity:return done(capacity(self));
        case Op::Empty:return done(size(self)==0);
        case Op::Maximum:return done(limit);
        case Op::Data:return done(data(self));
        case Op::At:if(a(0)>=size(self))range_error();return done(data(self)+a(0)*e);
        case Op::Index:if(a(0)>size(self))range_error();return done(data(self)+a(0)*e);
        case Op::Begin:case Op::End:case Op::RBegin:case Op::REnd:return done(write_iterator(a(0),self,entry->op==Op::End || entry->op==Op::RBegin?size(self):0));
        case Op::Inside:return done(a(0)>=data(self) && a(0)-data(self)<size(self)*e);
        case Op::Clear:store(self,{});return done(0);
        case Op::Reserve:if(a(0)>limit)length_error();if(a(0)>=size(self))store(self,contents(self),a(0),a(0)==0);return done(0);
        case Op::Resize:{if(a(0)>limit)length_error();auto value=contents(self);value.resize(static_cast<std::size_t>(a(0)),entry->input==Input::Character?static_cast<std::uint16_t>(a(1)&(e==1?255U:65535U)):0);store(self,value);return done(0);}
        case Op::Tidy:{const auto keep=a(1);if(keep>small)range_error();const auto old=a(0) && capacity(self)>small?data(self):0;auto value=read(old?old:self+p,keep);initialize(self);store(self,value);release(old);return done(0);}
        case Op::Grow:{if(a(0)>limit)length_error();auto value=contents(self);if(a(1) && a(0)<16/e && a(0)<value.size())value.resize(static_cast<std::size_t>(a(0)));if(!a(0))value.clear();store(self,value,a(0),a(1)!=0);return done(a(0)!=0);}
        case Op::Eos:if(a(0)>capacity(self))range_error();memory.store(GuestAddress{data(self)+a(0)*e},cbits,0);put(self+p+16,a(0));return done(0);
        case Op::Swap:{std::vector<std::uint8_t> lhs(16+3*p),rhs(16+3*p);memory.read(GuestAddress{self},lhs);memory.read(GuestAddress{a(0)},rhs);memory.check(GuestAddress{self},lhs.size(),Access::Write);memory.check(GuestAddress{a(0)},rhs.size(),Access::Write);memory.write(GuestAddress{self},rhs);memory.write(GuestAddress{a(0)},lhs);return done(0);}
        case Op::Assign:case Op::Append:case Op::Insert:case Op::Replace:case Op::Erase:{
            auto value=entry->op==Op::Assign?Text{}:contents(self);std::uint64_t at=entry->op==Op::Append?value.size():0,count=0;unsigned index=0;
            if(entry->op==Op::Insert || entry->op==Op::Replace || entry->op==Op::Erase){
                at=entry->iterator_range?position(a(0),self):a(0);index=1;
                if(at>value.size())range_error();
                if(entry->op!=Op::Insert){const auto end=entry->iterator_range?position(a(1),self):0;if(entry->iterator_range && end<at)range_error();count=entry->iterator_range?end-at:std::min<std::uint64_t>(a(1),value.size()-at);index=2;}
            }
            auto replacement=entry->op==Op::Erase?Text{}:input(entry->input,index);
            if(replacement.size()>limit-(value.size()-count))length_error();
            value.erase(value.begin()+static_cast<std::ptrdiff_t>(at),value.begin()+static_cast<std::ptrdiff_t>(at+count));value.insert(value.begin()+static_cast<std::ptrdiff_t>(at),replacement.begin(),replacement.end());store(self,value);return done(self);
        }
        case Op::Compare:{auto lhs=contents(self);if(entry->substring_lhs)lhs=sub(std::move(lhs),a(0),a(1));return done(static_cast<std::uint32_t>(compare(lhs,input(entry->input,entry->substring_lhs?2:0))));}
        case Op::Equal:case Op::NotEqual:case Op::Less:case Op::Greater:case Op::LessEqual:case Op::GreaterEqual:{
            const auto lhs=entry->text_lhs?input(entry->input,0):contents(a(0)),rhs=entry->text_lhs?contents(a(1)):input(entry->input,1);const int c=compare(lhs,rhs);
            return done(entry->op==Op::Equal?c==0:entry->op==Op::NotEqual?c!=0:entry->op==Op::Less?c<0:entry->op==Op::Greater?c>0:entry->op==Op::LessEqual?c<=0:c>=0);
        }
        case Op::Concat:{auto lhs=entry->text_lhs?input(entry->input,1):contents(a(1));auto rhs=entry->text_lhs?contents(a(2)):input(entry->input,2);if(rhs.size()>limit-lhs.size())length_error();lhs.insert(lhs.end(),rhs.begin(),rhs.end());initialize(a(0));store(a(0),lhs);return done(a(0));}
        case Op::Substr:{auto value=sub(contents(self),a(1),a(2));initialize(a(0));store(a(0),value);return done(a(0));}
        case Op::Copy:{auto value=sub(contents(self),a(2),a(1));memory.check(GuestAddress{a(0)},value.size()*e,Access::Write);for(std::size_t i=0;i<value.size();++i)memory.store(GuestAddress{a(0)+i*e},cbits,value[i]);return done(value.size());}
        case Op::CopySafe:{
            auto value=sub(contents(self),a(3),a(2));
            if(!value.empty() && (!a(0) || a(1)<value.size())){
                if(a(0) && a(1))memory.fill(GuestAddress{a(0)},0,static_cast<std::size_t>(a(1)*e));
                memory.store(GuestAddress{runtime_.errno_cell},32,a(0)?34:22);
                if(const auto handler=current_invalid_parameter_handler()){start_callbacks(state,memory,{{handler,{0,0,0,0,0}}},Continuation::Return,value.size());return std::nullopt;}
                return 0xc0000417U;
            }
            memory.check(GuestAddress{a(0)},value.size()*e,Access::Write);for(std::size_t i=0;i<value.size();++i)memory.store(GuestAddress{a(0)+i*e},cbits,value[i]);return done(value.size());
        }
        case Op::Allocator:memory.store(GuestAddress{a(0)},8,0);return done(a(0));
        case Op::CopyCapacity:{if(a(0)>limit || a(1)>std::max(capacity(self),a(0)))length_error();auto value=contents(self);if(!a(0))value.clear();store(self,value,a(0));if(a(0)){memory.store(GuestAddress{data(self)+a(1)*e},cbits,0);put(self+p+16,a(1));}return done(0);}
        case Op::CharacterAssign:if(a(0)>capacity(self) || a(1)>capacity(self)-a(0))range_error();memory.check(GuestAddress{data(self)+a(0)*e},static_cast<std::size_t>(a(1)*e),Access::Write);for(std::uint64_t i=0;i<a(1);++i)memory.store(GuestAddress{data(self)+(a(0)+i)*e},cbits,a(2));return done(0);
        case Op::EraseIterator:case Op::InsertIterator:case Op::InsertRange:{
            const bool returns=entry->op!=Op::InsertRange;const unsigned index=returns?1U:0U;
            const auto pos=position(a(index),self);auto value=contents(self);
            if(entry->op==Op::EraseIterator){const auto end=entry->input==Input::Iterators?position(a(index+1),self):pos+1;if(end<pos || end>value.size())range_error();value.erase(value.begin()+static_cast<std::ptrdiff_t>(pos),value.begin()+static_cast<std::ptrdiff_t>(end));}
            else{const auto extra=entry->op==Op::InsertIterator && entry->input==Input::None?Text{0}:input(entry->input,index+1);if(extra.size()>limit-value.size())length_error();value.insert(value.begin()+static_cast<std::ptrdiff_t>(pos),extra.begin(),extra.end());}
            store(self,value);return done(returns?write_iterator(a(0),self,pos):0);
        }
        case Op::Find:case Op::RFind:case Op::FirstOf:case Op::LastOf:case Op::FirstNotOf:case Op::LastNotOf:{
            const auto value=contents(self),needle=entry->input==Input::Span?read(a(0),a(2)):input(entry->input,0);const auto pos=a(1);
            if(entry->op==Op::Find || entry->op==Op::RFind){
                if(needle.size()>value.size())return done(npos);
                const auto last=value.size()-needle.size();if(entry->op==Op::Find && pos>last)return done(npos);
                if(entry->op==Op::Find){for(std::uint64_t i=pos;i<=last;++i)if(std::equal(needle.begin(),needle.end(),value.begin()+static_cast<std::ptrdiff_t>(i)))return done(i);}
                else{for(std::uint64_t i=std::min<std::uint64_t>(pos,last);;--i){if(std::equal(needle.begin(),needle.end(),value.begin()+static_cast<std::ptrdiff_t>(i)))return done(i);if(!i)break;}}
            }else if(!value.empty()){
                const bool reverse=entry->op==Op::LastOf || entry->op==Op::LastNotOf,negate=entry->op==Op::FirstNotOf || entry->op==Op::LastNotOf;
                if(!reverse && pos>=value.size())return done(npos);
                for(std::uint64_t i=reverse?std::min<std::uint64_t>(pos,value.size()-1):pos;;){const bool found=std::find(needle.begin(),needle.end(),value[static_cast<std::size_t>(i)])!=needle.end();if(found!=negate)return done(i);if(reverse){if(!i)break;--i;}else if(++i==value.size())break;}
            }return done(npos);
        }
        }
    }catch(MsvcExceptionKind kind){throw_msvc_exception(state,memory,service,kind);return std::nullopt;}
    catch(const std::bad_alloc&){throw_msvc_exception(state,memory,service,MsvcExceptionKind::BadAlloc);return std::nullopt;}
    catch(const std::length_error&){throw_msvc_exception(state,memory,service,MsvcExceptionKind::LengthError);return std::nullopt;}
    throw HostCallFault("VC9 字串操作缺少處理");
}

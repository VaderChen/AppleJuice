// ntdll 樹狀資料結構家族（依 WDK 公開的結構與語義，宿主端直接操作客體記憶體中的節點）：
// - splay 連結 RTL_SPLAY_LINKS：RtlSplay、RtlDelete、RtlDeleteNoSplay、RtlSubtreeSuccessor／Predecessor、RtlRealSuccessor／Predecessor；
// - RTL_GENERIC_TABLE（splay 樹＋插入順序串列）：Initialize／Insert／Delete／Lookup／Enumerate（含不 splay 版）／GetElement／Number／IsEmpty；
// - RTL_AVL_TABLE（BalancedRoot 哨兵、右子為根）：同一組操作，插入與刪除後依平衡因子旋轉；
// - RTL_RB_TREE：RtlRbInsertNodeEx、RtlRbRemoveNode（父指標低位元為紅色旗標，Min 指向最小節點）；
// - RtlFirstEntrySList。
// 比較、配置與釋放常式是客體函式，經協程回呼；結構調整本身不需回呼，在宿主端完成。
#include "x64arm/windows_services.hpp"
#include "service_callback_task.hpp"
#include "windows_slist.hpp"
#include <algorithm>

namespace x64arm {
namespace {
// 客體指標欄位的讀寫；p 為指標寬度。
struct Links {
    GuestMemory& m;unsigned p;
    std::uint64_t ld(std::uint64_t at)const{return m.load(GuestAddress{at},static_cast<std::uint8_t>(p*8));}
    void st(std::uint64_t at,std::uint64_t value)const{m.store(GuestAddress{at},static_cast<std::uint8_t>(p*8),value);}
    std::uint64_t parent(std::uint64_t n)const{return ld(n);}
    std::uint64_t left(std::uint64_t n)const{return ld(n+p);}
    std::uint64_t right(std::uint64_t n)const{return ld(n+2*p);}
    void set_parent(std::uint64_t n,std::uint64_t v)const{st(n,v);}
    void set_left(std::uint64_t n,std::uint64_t v)const{st(n+p,v);}
    void set_right(std::uint64_t n,std::uint64_t v)const{st(n+2*p,v);}
    std::uint64_t leftmost(std::uint64_t n)const{while(left(n))n=left(n);return n;}
    std::uint64_t rightmost(std::uint64_t n)const{while(right(n))n=right(n);return n;}
    void replace_child(std::uint64_t at,std::uint64_t old,std::uint64_t value)const{if(left(at)==old)set_left(at,value);else set_right(at,value);}
};
// ---- splay：根節點的 Parent 指向自己 ----
struct Splay:Links {
    bool root(std::uint64_t n)const{return parent(n)==n;}
    bool left_child(std::uint64_t n)const{return !root(n) && left(parent(n))==n;}
    void rotate_up(std::uint64_t x)const{
        const auto q=parent(x);const bool top=root(q);const auto g=top?0:parent(q);
        if(left(q)==x){const auto b=right(x);set_left(q,b);if(b)set_parent(b,q);set_right(x,q);}
        else {const auto b=left(x);set_right(q,b);if(b)set_parent(b,q);set_left(x,q);}
        set_parent(q,x);
        if(top)set_parent(x,x);else {set_parent(x,g);replace_child(g,q,x);}
    }
    std::uint64_t splay(std::uint64_t x)const{
        while(!root(x)){
            const auto q=parent(x);
            if(root(q)){rotate_up(x);break;}
            if(left_child(q)==left_child(x)){rotate_up(q);rotate_up(x);}else {rotate_up(x);rotate_up(x);}
        }
        return x;
    }
    std::uint64_t subtree_successor(std::uint64_t n)const{return right(n)?leftmost(right(n)):0;}
    std::uint64_t subtree_predecessor(std::uint64_t n)const{return left(n)?rightmost(left(n)):0;}
    std::uint64_t real_successor(std::uint64_t n)const{
        if(right(n))return leftmost(right(n));
        while(!root(n) && !left_child(n))n=parent(n);
        return root(n)?0:parent(n);
    }
    std::uint64_t real_predecessor(std::uint64_t n)const{
        if(left(n))return rightmost(left(n));
        while(!root(n) && left_child(n))n=parent(n);
        return root(n)?0:parent(n);
    }
    // 移除節點但不 splay；回傳 {新根, 刪除後應 splay 的節點}。兩個子節點時以左子樹最右節點頂替。
    std::pair<std::uint64_t,std::uint64_t> remove(std::uint64_t n,std::uint64_t tree)const{
        std::uint64_t replacement=0,touched=0;
        if(left(n) && right(n)){
            const auto q=rightmost(left(n));
            if(q!=left(n)){const auto ql=left(q),qp=parent(q);set_right(qp,ql);if(ql)set_parent(ql,qp);set_left(q,left(n));set_parent(left(n),q);}
            set_right(q,right(n));set_parent(right(n),q);replacement=touched=q;
        }else {replacement=left(n)?left(n):right(n);touched=root(n)?replacement:parent(n);}
        if(root(n)){tree=replacement;if(replacement)set_parent(replacement,replacement);}
        else {const auto q=parent(n);replace_child(q,n,replacement);if(replacement)set_parent(replacement,q);}
        return {tree,touched};
    }
};
// ---- AVL：Balance 為 右高 − 左高（-1、0、+1），位於三個指標之後的有號位元組 ----
struct Avl:Links {
    std::uint64_t sentinel;
    std::int8_t balance(std::uint64_t n)const{return static_cast<std::int8_t>(m.load(GuestAddress{n+3*p},8));}
    void set_balance(std::uint64_t n,int v)const{m.store(GuestAddress{n+3*p},8,static_cast<std::uint8_t>(static_cast<std::int8_t>(v)));}
    void rotate_left(std::uint64_t x)const{
        const auto y=right(x),b=left(y),g=parent(x);set_right(x,b);if(b)set_parent(b,x);
        set_parent(y,g);replace_child(g,x,y);set_left(y,x);set_parent(x,y);
    }
    void rotate_right(std::uint64_t x)const{
        const auto y=left(x),b=right(y),g=parent(x);set_left(x,b);if(b)set_parent(b,x);
        set_parent(y,g);replace_child(g,x,y);set_right(y,x);set_parent(x,y);
    }
    // 對失衡節點 q（±2）旋轉；回傳新子樹頂點與高度是否降低（刪除時用）。
    std::pair<std::uint64_t,bool> rebalance(std::uint64_t q)const{
        const int b=balance(q);
        if(b==2){
            const auto s=right(q);const int sb=balance(s);
            if(sb>=0){rotate_left(q);if(sb==0){set_balance(q,1);set_balance(s,-1);return {s,false};}set_balance(q,0);set_balance(s,0);return {s,true};}
            const auto w=left(s);const int wb=balance(w);rotate_right(s);rotate_left(q);
            set_balance(q,wb==1?-1:0);set_balance(s,wb==-1?1:0);set_balance(w,0);return {w,true};
        }
        const auto s=left(q);const int sb=balance(s);
        if(sb<=0){rotate_right(q);if(sb==0){set_balance(q,-1);set_balance(s,1);return {s,false};}set_balance(q,0);set_balance(s,0);return {s,true};}
        const auto w=right(s);const int wb=balance(w);rotate_left(s);rotate_right(q);
        set_balance(q,wb==-1?1:0);set_balance(s,wb==1?-1:0);set_balance(w,0);return {w,true};
    }
    // 插入新葉節點 x 後向上調整；回傳樹高是否增加。
    bool insert_fixup(std::uint64_t x)const{
        while(true){
            const auto q=parent(x);if(q==sentinel)return true;
            set_balance(q,balance(q)+(left(q)==x?-1:1));const int b=balance(q);
            if(!b)return false;if(b==1 || b==-1){x=q;continue;}
            (void)rebalance(q);return false;
        }
    }
    // 移除節點 n（兩個子節點時以右子樹最左節點頂替）並調整；回傳樹高是否降低。
    bool remove(std::uint64_t n)const{
        std::uint64_t q=0;bool from_left=false;
        if(left(n) && right(n)){
            const auto s=leftmost(right(n));
            if(parent(s)!=n){const auto sr=right(s),sp=parent(s);set_left(sp,sr);if(sr)set_parent(sr,sp);set_right(s,right(n));set_parent(right(n),s);q=sp;from_left=true;}
            else {q=s;from_left=false;}
            set_left(s,left(n));set_parent(left(n),s);set_balance(s,balance(n));
            const auto g=parent(n);replace_child(g,n,s);set_parent(s,g);
        }else {
            const auto c=left(n)?left(n):right(n);q=parent(n);from_left=left(q)==n;
            replace_child(q,n,c);if(c)set_parent(c,q);
        }
        while(q!=sentinel){
            set_balance(q,balance(q)+(from_left?1:-1));int b=balance(q);
            if(b==1 || b==-1)return false;
            auto top=q;
            if(b==2 || b==-2){const auto [t,shorter]=rebalance(q);if(!shorter)return false;top=t;}
            const auto g=parent(top);from_left=g!=sentinel && left(g)==top;q=g;
        }
        return true;
    }
};
// ---- 紅黑樹：RTL_BALANCED_NODE{Left, Right, ParentValue（低位元為紅色）} ----
struct RedBlack {
    GuestMemory& m;unsigned p;std::uint64_t tree;
    std::uint64_t ld(std::uint64_t at)const{return m.load(GuestAddress{at},static_cast<std::uint8_t>(p*8));}
    void st(std::uint64_t at,std::uint64_t v)const{m.store(GuestAddress{at},static_cast<std::uint8_t>(p*8),v);}
    std::uint64_t child(std::uint64_t n,unsigned side)const{return ld(n+side*p);}
    void set_child(std::uint64_t n,unsigned side,std::uint64_t v)const{st(n+side*p,v);}
    std::uint64_t parent(std::uint64_t n)const{return ld(n+2*p)&~std::uint64_t{3};}
    bool red(std::uint64_t n)const{return n && (ld(n+2*p)&1);}
    void set(std::uint64_t n,std::uint64_t parent_node,bool is_red)const{st(n+2*p,parent_node|(is_red?1:0));}
    void set_parent(std::uint64_t n,std::uint64_t v)const{set(n,v,red(n));}
    void set_red(std::uint64_t n,bool v)const{set(n,parent(n),v);}
    std::uint64_t root()const{return ld(tree);}
    void replace(std::uint64_t parent_node,std::uint64_t old,std::uint64_t value)const{
        if(!parent_node)st(tree,value);else if(child(parent_node,0)==old)set_child(parent_node,0,value);else set_child(parent_node,1,value);
    }
    void rotate(std::uint64_t x,unsigned side)const{ // side=0 左旋（右子上升），1 右旋
        const unsigned other=1-side;const auto y=child(x,other),b=child(y,side),g=parent(x);
        set_child(x,other,b);if(b)set_parent(b,x);set_parent(y,g);replace(g,x,y);set_child(y,side,x);set_parent(x,y);
    }
    std::uint64_t next(std::uint64_t n)const{
        if(child(n,1)){n=child(n,1);while(child(n,0))n=child(n,0);return n;}
        auto q=parent(n);while(q && child(q,1)==n){n=q;q=parent(q);}return q;
    }
    void insert(std::uint64_t parent_node,bool right_side,std::uint64_t n)const{
        set_child(n,0,0);set_child(n,1,0);
        const auto min=ld(tree+p);
        if(!parent_node){set(n,0,false);st(tree,n);st(tree+p,n);return;}
        set_child(parent_node,right_side?1:0,n);set(n,parent_node,true);
        if(!right_side && parent_node==min)st(tree+p,n);
        auto x=n;
        while(red(parent(x))){
            const auto q=parent(x),g=parent(q);const unsigned side=child(g,0)==q?0:1;const auto uncle=child(g,1-side);
            if(red(uncle)){set_red(q,false);set_red(uncle,false);set_red(g,true);x=g;continue;}
            if(child(q,1-side)==x){rotate(q,side);x=q;}
            const auto q2=parent(x),g2=parent(q2);set_red(q2,false);set_red(g2,true);rotate(g2,1-side);
        }
        set_red(root(),false);
    }
    void remove(std::uint64_t z)const{
        if(ld(tree+p)==z)st(tree+p,next(z));
        std::uint64_t x=0,xp=0;bool removed_red=red(z);
        if(!child(z,0) || !child(z,1)){
            x=child(z,0)?child(z,0):child(z,1);xp=parent(z);replace(xp,z,x);if(x)set_parent(x,xp);
        }else {
            auto y=child(z,1);while(child(y,0))y=child(y,0);removed_red=red(y);x=child(y,1);
            if(parent(y)==z)xp=y;
            else {xp=parent(y);set_child(xp,0,x);if(x)set_parent(x,xp);set_child(y,1,child(z,1));set_parent(child(z,1),y);}
            set_child(y,0,child(z,0));set_parent(child(z,0),y);const auto zp=parent(z);replace(zp,z,y);set(y,zp,red(z));
        }
        if(removed_red)return;
        while(x!=root() && !red(x)){
            if(!xp)break;
            const unsigned side=child(xp,0)==x?0:1;auto w=child(xp,1-side);
            if(red(w)){set_red(w,false);set_red(xp,true);rotate(xp,side);w=child(xp,1-side);}
            if(!red(child(w,0)) && !red(child(w,1))){set_red(w,true);x=xp;xp=parent(x);continue;}
            if(!red(child(w,1-side))){set_red(child(w,side),false);set_red(w,true);rotate(w,1-side);w=child(xp,1-side);}
            set_red(w,red(xp));set_red(xp,false);set_red(child(w,1-side),false);rotate(xp,side);x=root();xp=0;break;
        }
        if(x)set_red(x,false);
    }
};
// RTL_GENERIC_TABLE 與 RTL_AVL_TABLE 的欄位位移。
struct GenericLayout {
    unsigned root,list,ordered,which,count,compare,allocate,free,context,header;
    explicit GenericLayout(bool x86):root(0),list(x86?4:8),ordered(x86?12:24),which(x86?16:32),count(x86?20:36),compare(x86?24:40),allocate(x86?28:48),free(x86?32:56),context(x86?36:64),header(x86?24:40){}
};
struct AvlLayout {
    unsigned ordered,which,count,depth,restart,deletes,compare,allocate,free,context,header;
    explicit AvlLayout(bool x86):ordered(x86?16:32),which(x86?20:40),count(x86?24:44),depth(x86?28:48),restart(x86?32:56),deletes(x86?36:64),compare(x86?40:72),allocate(x86?44:80),free(x86?48:88),context(x86?52:96),header(x86?16:32){}
};
}
std::span<const WindowsServiceDescriptor> ntdll_tree_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"ntdll.dll","RtlSplay",1},{"ntdll.dll","RtlDelete",1},{"ntdll.dll","RtlDeleteNoSplay",2},{"ntdll.dll","RtlSubtreeSuccessor",1},{"ntdll.dll","RtlSubtreePredecessor",1},
        {"ntdll.dll","RtlRealSuccessor",1},{"ntdll.dll","RtlRealPredecessor",1},
        {"ntdll.dll","RtlInitializeGenericTable",5},{"ntdll.dll","RtlInsertElementGenericTable",4,0,0,0x4},{"ntdll.dll","RtlDeleteElementGenericTable",2},
        {"ntdll.dll","RtlLookupElementGenericTable",2},{"ntdll.dll","RtlEnumerateGenericTable",2,0,0,0x2},{"ntdll.dll","RtlEnumerateGenericTableWithoutSplaying",2},
        {"ntdll.dll","RtlGetElementGenericTable",2,0,0,0x2},{"ntdll.dll","RtlNumberGenericTableElements",1},{"ntdll.dll","RtlIsGenericTableEmpty",1},
        {"ntdll.dll","RtlInitializeGenericTableAvl",5},{"ntdll.dll","RtlInsertElementGenericTableAvl",4,0,0,0x4},{"ntdll.dll","RtlDeleteElementGenericTableAvl",2},
        {"ntdll.dll","RtlLookupElementGenericTableAvl",2},{"ntdll.dll","RtlEnumerateGenericTableAvl",2,0,0,0x2},{"ntdll.dll","RtlEnumerateGenericTableWithoutSplayingAvl",2},
        {"ntdll.dll","RtlGetElementGenericTableAvl",2,0,0,0x2},{"ntdll.dll","RtlNumberGenericTableElementsAvl",1},{"ntdll.dll","RtlIsGenericTableEmptyAvl",1},
        {"ntdll.dll","RtlRbInsertNodeEx",4,0,0,0x4},{"ntdll.dll","RtlRbRemoveNode",2},{"ntdll.dll","RtlFirstEntrySList",1}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_ntdll_trees(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const auto name=service.symbol;const unsigned unit=x86_?4U:8U;
    const bool avl=name.ends_with("Avl");
    const bool generic=name.find("GenericTable")!=name.npos;
    // 需要客體比較或配置常式的入口走協程；其餘直接完成。
    if(generic && (name.starts_with("RtlInsert") || name.starts_with("RtlDelete") || name.starts_with("RtlLookup")))
        return nested_continuation(state,memory,"Rtl 泛用表",[this,service,unit,avl](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
            const auto table=call(0),buffer=call(1);if(!table)co_return finish(0);
            const auto symbol=service.symbol;const Links links{m,unit};
            const GenericLayout g(x86_);const AvlLayout a(x86_);
            const auto compare=links.ld(table+(avl?a.compare:g.compare));const std::uint64_t header=avl?a.header:g.header;
            // 依比較結果（0 小於、1 大於、2 相等）由根往下尋找；回傳 {找到的節點或 0, 最後的父節點, 是否接在右側}。
            std::uint64_t node=avl?links.right(table):links.ld(table+g.root),parent=0;bool right_side=false,found=false;
            while(node){
                const auto result=static_cast<std::uint32_t>(co_await service_callback::call(compare,{table,buffer,node+header}));
                if(result==2){found=true;break;}
                parent=node;right_side=result==1;node=right_side?links.right(node):links.left(node);
            }
            if(symbol.starts_with("RtlLookup")){
                if(!found)co_return finish(0);
                if(!avl){const Splay splay{m,unit};links.st(table+g.root,splay.splay(node));}
                co_return finish(node+header);
            }
            if(symbol.starts_with("RtlDelete")){
                if(!found)co_return finish(0);
                if(avl){
                    const Avl tree{{m,unit},table};if(tree.remove(node)){const auto depth=static_cast<std::uint32_t>(m.load(GuestAddress{table+a.depth},32));if(depth)m.store(GuestAddress{table+a.depth},32,depth-1);}
                    m.store(GuestAddress{table+a.count},32,m.load(GuestAddress{table+a.count},32)-1);m.store(GuestAddress{table+a.deletes},32,m.load(GuestAddress{table+a.deletes},32)+1);
                    links.st(table+a.ordered,0);m.store(GuestAddress{table+a.which},32,0);links.st(table+a.restart,0);
                }else {
                    const Splay splay{m,unit};auto [root,touched]=splay.remove(node,links.ld(table+g.root));if(touched)root=splay.splay(touched);links.st(table+g.root,root);
                    // 自插入順序串列移除，並使 GetElement 的位置快取失效。
                    const auto entry=node+3*unit,flink=links.ld(entry),blink=links.ld(entry+unit);links.st(blink,flink);links.st(flink+unit,blink);
                    m.store(GuestAddress{table+g.count},32,m.load(GuestAddress{table+g.count},32)-1);links.st(table+g.ordered,table+g.list);m.store(GuestAddress{table+g.which},32,0);
                }
                (void)co_await service_callback::call(links.ld(table+(avl?a.free:g.free)),{table,node});
                co_return finish(1);
            }
            // Insert(Table, Buffer, BufferSize, NewElement*)：已存在時回傳既有元素；否則配置 header＋BufferSize 並複製內容。
            const auto size=static_cast<std::uint32_t>(call(2));const std::uint64_t created=call(3);
            if(found){
                if(created)m.store(GuestAddress{created},8,0);
                if(!avl){const Splay splay{m,unit};links.st(table+g.root,splay.splay(node));}
                co_return finish(node+header);
            }
            const std::uint64_t fresh=co_await service_callback::call_value(links.ld(table+(avl?a.allocate:g.allocate)),{table,header+std::uint64_t{size}}); // 回傳指標，取完整 RAX。
            const std::uint64_t allocation=x86_?fresh&0xffffffffU:fresh;if(!allocation)co_return finish(0);
            if(size){std::vector<std::uint8_t> bytes(size);m.read(GuestAddress{buffer},bytes);m.write(GuestAddress{allocation+header},bytes);}
            links.set_left(allocation,0);links.set_right(allocation,0);
            if(avl){
                const Avl tree{{m,unit},table};tree.set_balance(allocation,0);
                if(!parent){links.set_parent(allocation,table);links.set_right(table,allocation);}
                else {links.set_parent(allocation,parent);if(right_side)links.set_right(parent,allocation);else links.set_left(parent,allocation);}
                if(tree.insert_fixup(allocation))m.store(GuestAddress{table+a.depth},32,m.load(GuestAddress{table+a.depth},32)+1);
                m.store(GuestAddress{table+a.count},32,m.load(GuestAddress{table+a.count},32)+1);links.st(table+a.ordered,0);m.store(GuestAddress{table+a.which},32,0);
            }else {
                const Splay splay{m,unit};
                if(!parent)links.set_parent(allocation,allocation);
                else {links.set_parent(allocation,parent);if(right_side)links.set_right(parent,allocation);else links.set_left(parent,allocation);}
                // 接在插入順序串列尾端。
                const auto head=table+g.list,entry=allocation+3*unit,tail=links.ld(head+unit);
                links.st(entry,head);links.st(entry+unit,tail);links.st(tail,entry);links.st(head+unit,entry);
                m.store(GuestAddress{table+g.count},32,m.load(GuestAddress{table+g.count},32)+1);
                links.st(table+g.root,splay.splay(allocation));
            }
            if(created)m.store(GuestAddress{created},8,1);
            co_return finish(allocation+header);
        });
    const WindowsCall call(state,memory,service);const auto finish=[&](std::uint64_t value){call.return_to(state,value);return std::optional<std::uint32_t>{};};
    const Links links{memory,unit};const Splay splay{memory,unit};
    if(name=="RtlFirstEntrySList")return finish(slist::first(memory,call(0),x86_));
    if(name=="RtlRbInsertNodeEx" || name=="RtlRbRemoveNode"){
        // RTL_RB_TREE{Root, Min}；Min 的最低位元為編碼樹旗標，未編碼的樹才可由宿主端直接調整。
        const auto tree=call(0);if(!tree)return finish(0);
        if(links.ld(tree+unit)&1)throw HostCallFault("RtlRbTree 編碼指標樹尚未支援");
        const RedBlack rb{memory,unit,tree};
        if(name=="RtlRbInsertNodeEx")rb.insert(call(1),static_cast<std::uint8_t>(call(2))!=0,call(3));else rb.remove(call(1));
        return finish(0);
    }
    if(name=="RtlSplay")return finish(call(0)?splay.splay(call(0)):0);
    if(name=="RtlSubtreeSuccessor")return finish(splay.subtree_successor(call(0)));
    if(name=="RtlSubtreePredecessor")return finish(splay.subtree_predecessor(call(0)));
    if(name=="RtlRealSuccessor")return finish(splay.real_successor(call(0)));
    if(name=="RtlRealPredecessor")return finish(splay.real_predecessor(call(0)));
    if(name=="RtlDelete" || name=="RtlDeleteNoSplay"){
        // RtlDelete 回傳刪除並 splay 後的新根；RtlDeleteNoSplay 只更新 *Root。
        const auto n=call(0);auto top=n;while(!splay.root(top))top=splay.parent(top);
        auto [root,touched]=splay.remove(n,top);
        if(name=="RtlDelete")return finish(touched?splay.splay(touched):root);
        links.st(call(1),root);return finish(0);
    }
    const auto table=call(0);
    if(name=="RtlInitializeGenericTable"){
        const GenericLayout g(x86_);links.st(table+g.root,0);links.st(table+g.list,table+g.list);links.st(table+g.list+unit,table+g.list);
        links.st(table+g.ordered,table+g.list);memory.store(GuestAddress{table+g.which},32,0);memory.store(GuestAddress{table+g.count},32,0);
        links.st(table+g.compare,call(1));links.st(table+g.allocate,call(2));links.st(table+g.free,call(3));links.st(table+g.context,call(4));return finish(0);
    }
    if(name=="RtlInitializeGenericTableAvl"){
        // BalancedRoot 為哨兵：Parent 指向自己、右子為樹根。
        const AvlLayout a(x86_);memory.fill(GuestAddress{table},0,a.context+unit);links.set_parent(table,table);
        links.st(table+a.compare,call(1));links.st(table+a.allocate,call(2));links.st(table+a.free,call(3));links.st(table+a.context,call(4));return finish(0);
    }
    const GenericLayout g(x86_);const AvlLayout a(x86_);
    const auto count=static_cast<std::uint32_t>(memory.load(GuestAddress{table+(avl?a.count:g.count)},32));
    if(name.starts_with("RtlNumberGenericTableElements"))return finish(count);
    if(name.starts_with("RtlIsGenericTableEmpty"))return finish(count==0);
    if(name=="RtlEnumerateGenericTable"){
        // 以根為目前位置：Restart 時 splay 最左節點，否則 splay 根的後繼。
        const auto root=links.ld(table+g.root);if(!root)return finish(0);
        const auto node=static_cast<std::uint8_t>(call(1))?links.leftmost(root):splay.real_successor(root);if(!node)return finish(0);
        links.st(table+g.root,splay.splay(node));return finish(node+g.header);
    }
    const Avl tree{{memory,unit},table};
    const auto avl_next=[&](std::uint64_t n)->std::uint64_t{
        if(links.right(n))return links.leftmost(links.right(n));
        auto q=links.parent(n);while(q!=table && links.right(q)==n){n=q;q=links.parent(q);}return q==table?0:q;
    };
    if(name=="RtlEnumerateGenericTableAvl"){
        // AVL 不 splay：以 RestartKey 記住上次位置。
        const auto root=links.right(table);if(!root)return finish(0);
        const auto previous=links.ld(table+a.restart);const auto node=static_cast<std::uint8_t>(call(1)) || !previous?links.leftmost(root):avl_next(previous);
        links.st(table+a.restart,node);return finish(node?node+a.header:0);
    }
    if(name.starts_with("RtlEnumerateGenericTableWithoutSplaying")){
        const auto key=call(1);if(!key)return finish(0);const auto previous=links.ld(key);
        std::uint64_t node=0;
        if(avl){const auto root=links.right(table);if(root)node=previous?avl_next(previous):links.leftmost(root);}
        else {const auto root=links.ld(table+g.root);if(root)node=previous?splay.real_successor(previous):links.leftmost(root);}
        links.st(key,node);return finish(node?node+(avl?a.header:g.header):0);
    }
    if(name.starts_with("RtlGetElementGenericTable")){
        // 第 I 個元素（0 起算）：泛用表依插入順序，AVL 表依排序順序；以 OrderedPointer／WhichOrderedElement（1 起算）快取位置。
        const auto index=static_cast<std::uint32_t>(call(1));if(index>=count)return finish(0);const auto target=index+1;
        auto which=static_cast<std::uint32_t>(memory.load(GuestAddress{table+(avl?a.which:g.which)},32));auto at=links.ld(table+(avl?a.ordered:g.ordered));
        if(avl){
            if(!which || !at || which>target){at=links.leftmost(links.right(table));which=1;}
            while(which<target){at=avl_next(at);++which;}
            links.st(table+a.ordered,at);memory.store(GuestAddress{table+a.which},32,which);return finish(at+a.header);
        }
        const auto head=table+g.list;if(!which)at=head;
        // 從快取位置、串列頭或串列尾中距離最近者走過去。
        if(target>which){if(target-which>count-target+1){at=head;which=count+1;while(which>target){at=links.ld(at+unit);--which;}}else while(which<target){at=links.ld(at);++which;}}
        else if(which-target>target){at=head;which=0;while(which<target){at=links.ld(at);++which;}}
        else while(which>target){at=links.ld(at+unit);--which;}
        links.st(table+g.ordered,at);memory.store(GuestAddress{table+g.which},32,which);return finish(at-3*unit+g.header);
    }
    (void)tree;return std::nullopt;
}
}

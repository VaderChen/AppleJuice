#include "x64arm/windows_region.hpp"
#include <map>

namespace x64arm {
namespace {
GuiRegion combine_bands(const GuiRegion& a,const GuiRegion& b,unsigned mode,std::vector<std::int32_t> ys={}){
    if(mode==5)return a;
    if(ys.empty()){
        for(const auto* source:{&a,&b})for(const auto& r:*source)if(!gui_rect_empty(r)){ys.push_back(r[1]);ys.push_back(r[3]);}
        std::sort(ys.begin(),ys.end());ys.erase(std::unique(ys.begin(),ys.end()),ys.end());
    }
    struct Event{std::int32_t x;int a,b;};GuiRegion output;std::vector<std::array<std::int32_t,2>> previous;std::size_t previous_start=0;
    for(std::size_t band=1;band<ys.size();++band){
        const auto top=ys[band-1],bottom=ys[band];std::vector<Event> events;
        for(const auto* source:{&a,&b})for(const auto& r:*source)if(r[1]<=top && r[3]>=bottom && !gui_rect_empty(r)){
            events.push_back({r[0],source==&a?1:0,source==&b?1:0});events.push_back({r[2],source==&a?-1:0,source==&b?-1:0});
        }
        std::sort(events.begin(),events.end(),[](const auto& l,const auto& r){return l.x<r.x;});
        std::vector<std::array<std::int32_t,2>> intervals;int inside_a=0,inside_b=0;std::int32_t last=0;
        for(std::size_t i=0;i<events.size();){
            const auto x=events[i].x;const bool include=mode==1?(inside_a && inside_b):mode==2?(inside_a || inside_b):mode==3?((inside_a!=0)!=(inside_b!=0)):(inside_a && !inside_b);
            if(include && last<x){if(!intervals.empty() && intervals.back()[1]==last)intervals.back()[1]=x;else intervals.push_back({last,x});}
            do{inside_a+=events[i].a;inside_b+=events[i].b;++i;}while(i<events.size() && events[i].x==x);last=x;
        }
        if(!intervals.empty() && intervals==previous){for(std::size_t i=previous_start;i<output.size();++i)output[i][3]=bottom;}
        else {previous_start=output.size();for(const auto& pair:intervals)output.push_back({pair[0],top,pair[1],bottom});}
        previous=std::move(intervals);if(output.size()>4096)throw std::length_error("GDI region 超出矩形數量上限");
    }
    return output;
}

GuiRegion combine_sweep(const GuiRegion& a,const GuiRegion& b,unsigned mode){
    struct Edge {std::int32_t y,left,right;int a,b;};
    std::vector<Edge> edges;edges.reserve((a.size()+b.size())*2);
    for(const auto& r:a)if(!gui_rect_empty(r)){
        edges.push_back({r[1],r[0],r[2],1,0});edges.push_back({r[3],r[0],r[2],-1,0});
    }
    for(const auto& r:b)if(!gui_rect_empty(r)){
        edges.push_back({r[1],r[0],r[2],0,1});edges.push_back({r[3],r[0],r[2],0,-1});
    }
    std::sort(edges.begin(),edges.end(),[](const auto& l,const auto& r){return l.y<r.y;});
    // 僅保留目前水平帶的 x 端點；同一高度的加入／移除全部完成才產生輸出。
    std::map<std::int32_t,std::array<std::int64_t,2>> active;
    const auto change=[&](std::int32_t x,int da,int db){
        auto [it,inserted]=active.try_emplace(x,std::array<std::int64_t,2>{});(void)inserted;
        it->second[0]+=da;it->second[1]+=db;
        if(!it->second[0] && !it->second[1])active.erase(it);
    };
    GuiRegion output;std::vector<std::array<std::int32_t,2>> previous,intervals;std::size_t previous_start=0;
    for(std::size_t at=0;at<edges.size();){
        const auto top=edges[at].y;
        do{const auto& edge=edges[at++];change(edge.left,edge.a,edge.b);change(edge.right,-edge.a,-edge.b);}
        while(at<edges.size() && edges[at].y==top);
        if(at==edges.size())break;
        const auto bottom=edges[at].y;std::int64_t inside_a=0,inside_b=0;std::int32_t last=0;
        intervals.clear();
        for(const auto& [x,counts]:active){
            const bool include=mode==1?(inside_a && inside_b):mode==2?(inside_a || inside_b):mode==3?((inside_a!=0)!=(inside_b!=0)):(inside_a && !inside_b);
            if(include && last<x){if(!intervals.empty() && intervals.back()[1]==last)intervals.back()[1]=x;else intervals.push_back({last,x});}
            inside_a+=counts[0];inside_b+=counts[1];last=x;
        }
        if(!intervals.empty() && intervals==previous){for(std::size_t i=previous_start;i<output.size();++i)output[i][3]=bottom;}
        else {previous_start=output.size();for(const auto& pair:intervals)output.push_back({pair[0],top,pair[1],bottom});}
        previous.swap(intervals);if(output.size()>4096)throw std::length_error("GDI region 超出矩形數量上限");
    }
    return output;
}
}

GuiRegion gui_region_combine(const GuiRegion& a,const GuiRegion& b,unsigned mode){
    if(mode==5)return a;
    // 小區域維持無樹狀索引的流程；大型區域避免每帶重新掃描及排序所有矩形。
    if(a.size()+b.size()<=16)return combine_bands(a,b,mode);
    // 只有一帶時直接排序 x 交點，無須建立及維護跨帶的活躍集合。
    std::array<std::int32_t,2> band{};bool present=false,uniform=true;
    for(const auto* source:{&a,&b}){
        for(const auto& r:*source)if(!gui_rect_empty(r)){
            if(!present){band={r[1],r[3]};present=true;}
            else if(band[0]!=r[1] || band[1]!=r[3]){uniform=false;break;}
        }
        if(!uniform)break;
    }
    if(!present)return {};
    return uniform?combine_bands(a,b,mode,{band[0],band[1]}):combine_sweep(a,b,mode);
}
}

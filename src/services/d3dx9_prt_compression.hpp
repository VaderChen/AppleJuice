#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numeric>
#include <span>
#include <stdexcept>
#include <vector>
namespace x64arm::d3dx_prt {
struct Compressed {
    unsigned samples{},coefficients{},channels{},width{},height{},clusters{},components{};
    std::vector<unsigned> ids;
    std::vector<float> basis,weights;
    void normalize(){
        const unsigned dimension=coefficients*channels;
        std::vector<float> maximum(std::size_t(clusters)*components);
        for(unsigned s=0;s<samples;++s)for(unsigned p=0;p<components;++p)maximum[std::size_t(ids[s])*components+p]=std::max(maximum[std::size_t(ids[s])*components+p],std::abs(weights[std::size_t(s)*components+p]));
        for(unsigned c=0;c<clusters;++c)for(unsigned p=0;p<components;++p){const auto scale=maximum[std::size_t(c)*components+p];if(scale==0)continue;for(unsigned d=0;d<dimension;++d)basis[(std::size_t(c)*(components+1)+p+1)*dimension+d]*=scale;}
        for(unsigned s=0;s<samples;++s)for(unsigned p=0;p<components;++p){const auto scale=maximum[std::size_t(ids[s])*components+p];if(scale!=0)weights[std::size_t(s)*components+p]/=scale;}
    }
};
// 決定性分群後，以對稱 Jacobi 分解求各群的主成分；不以常數或原始向量冒充壓縮。
inline std::uint32_t compress(std::span<const float> input,unsigned samples,unsigned coefficients,unsigned channels,
        unsigned clusters,unsigned components,unsigned quality,const std::function<std::uint32_t(float)>& progress,Compressed& output){
    const auto check=[](bool value){if(!value)throw std::invalid_argument("PRT 壓縮參數不合法");};
    const unsigned dimension=coefficients*channels;
    check(samples && coefficients && coefficients<=4096 && dimension<=4096 && (channels==1 || channels==3) && clusters && clusters<=samples && components<=dimension && (quality==1 || quality==2));
    check(input.size()==std::size_t(samples)*dimension && std::uint64_t(clusters)*(components+1)*dimension<=128U*1024U*1024U);
    for(float value:input)check(std::isfinite(value));
    Compressed result;result.samples=samples;result.coefficients=coefficients;result.channels=channels;result.clusters=clusters;result.components=components;result.ids.resize(samples);
    std::vector<double> centers(std::size_t(clusters)*dimension),distances(samples,INFINITY);
    unsigned seed=0;
    for(unsigned c=0;c<clusters;++c){
        for(unsigned d=0;d<dimension;++d)centers[std::size_t(c)*dimension+d]=input[std::size_t(seed)*dimension+d];
        double farthest=-1;
        for(unsigned s=0;s<samples;++s){double distance=0;for(unsigned d=0;d<dimension;++d){const double delta=input[std::size_t(s)*dimension+d]-centers[std::size_t(c)*dimension+d];distance+=delta*delta;}distances[s]=std::min(distances[s],distance);if(distances[s]>farthest){farthest=distances[s];seed=s;}}
        if(auto status=progress(.1F*static_cast<float>(c+1)/static_cast<float>(clusters)))return status;
    }
    const unsigned iterations=quality==1?16U:64U;
    for(unsigned iteration=0;iteration<iterations;++iteration){
        std::vector<double> sums(centers.size());std::vector<unsigned> counts(clusters);bool changed=false;
        for(unsigned s=0;s<samples;++s){unsigned chosen=0;double best=INFINITY;
            for(unsigned c=0;c<clusters;++c){double distance=0;for(unsigned d=0;d<dimension;++d){const double delta=input[std::size_t(s)*dimension+d]-centers[std::size_t(c)*dimension+d];distance+=delta*delta;}if(distance<best){best=distance;chosen=c;}}
            changed|=result.ids[s]!=chosen;result.ids[s]=chosen;++counts[chosen];for(unsigned d=0;d<dimension;++d)sums[std::size_t(chosen)*dimension+d]+=input[std::size_t(s)*dimension+d];
        }
        for(unsigned c=0;c<clusters;++c)if(counts[c])for(unsigned d=0;d<dimension;++d)centers[std::size_t(c)*dimension+d]=sums[std::size_t(c)*dimension+d]/counts[c];
        if(auto status=progress(.1F+.2F*static_cast<float>(iteration+1)/static_cast<float>(iterations)))return status;
        if(!changed && iteration)break;
    }
    result.basis.resize(std::size_t(clusters)*(components+1)*dimension);result.weights.resize(std::size_t(samples)*components);
    for(unsigned c=0;c<clusters;++c){
        std::vector<double> covariance(std::size_t(dimension)*dimension),vectors(covariance.size());
        for(unsigned d=0;d<dimension;++d)vectors[std::size_t(d)*dimension+d]=1;
        unsigned members=0;
        for(unsigned s=0;s<samples;++s)if(result.ids[s]==c){++members;for(unsigned i=0;i<dimension;++i){const double x=input[std::size_t(s)*dimension+i]-centers[std::size_t(c)*dimension+i];for(unsigned j=0;j<=i;++j)covariance[std::size_t(i)*dimension+j]+=x*(input[std::size_t(s)*dimension+j]-centers[std::size_t(c)*dimension+j]);}}
        for(unsigned i=0;i<dimension;++i)for(unsigned j=0;j<=i;++j){const auto value=covariance[std::size_t(i)*dimension+j]/std::max(1U,members);covariance[std::size_t(i)*dimension+j]=covariance[std::size_t(j)*dimension+i]=value;}
        for(unsigned sweep=0;sweep<64;++sweep){double off=0,diagonal=0;for(unsigned i=0;i<dimension;++i){diagonal+=std::abs(covariance[std::size_t(i)*dimension+i]);for(unsigned j=0;j<i;++j)off+=std::abs(covariance[std::size_t(i)*dimension+j]);}if(off<=std::max(1e-30,diagonal*1e-12))break;
            for(unsigned p=0;p<dimension;++p)for(unsigned q=p+1;q<dimension;++q){const auto pq=std::size_t(p)*dimension+q,pp=std::size_t(p)*dimension+p,qq=std::size_t(q)*dimension+q;const double a=covariance[pq];if(std::abs(a)<=std::max(1e-30,diagonal*1e-16))continue;
                const double tau=(covariance[qq]-covariance[pp])/(2*a),t=std::copysign(1.0,tau)/(std::abs(tau)+std::hypot(1.0,tau)),cosine=1/std::sqrt(1+t*t),sine=t*cosine;
                covariance[pp]-=t*a;covariance[qq]+=t*a;covariance[pq]=covariance[std::size_t(q)*dimension+p]=0;
                for(unsigned k=0;k<dimension;++k){if(k!=p && k!=q){const double kp=covariance[std::size_t(k)*dimension+p],kq=covariance[std::size_t(k)*dimension+q];covariance[std::size_t(k)*dimension+p]=covariance[std::size_t(p)*dimension+k]=cosine*kp-sine*kq;covariance[std::size_t(k)*dimension+q]=covariance[std::size_t(q)*dimension+k]=sine*kp+cosine*kq;}
                    const double vp=vectors[std::size_t(k)*dimension+p],vq=vectors[std::size_t(k)*dimension+q];vectors[std::size_t(k)*dimension+p]=cosine*vp-sine*vq;vectors[std::size_t(k)*dimension+q]=sine*vp+cosine*vq;}
            }
        }
        std::vector<unsigned> order(dimension);std::iota(order.begin(),order.end(),0);std::stable_sort(order.begin(),order.end(),[&](unsigned a,unsigned b){return covariance[std::size_t(a)*dimension+a]>covariance[std::size_t(b)*dimension+b];});
        const auto base=std::size_t(c)*(components+1)*dimension;
        for(unsigned d=0;d<dimension;++d)result.basis[base+d]=static_cast<float>(centers[std::size_t(c)*dimension+d]);
        for(unsigned p=0;p<components;++p){unsigned dominant=0;for(unsigned d=1;d<dimension;++d)if(std::abs(vectors[std::size_t(d)*dimension+order[p]])>std::abs(vectors[std::size_t(dominant)*dimension+order[p]]))dominant=d;const double sign=vectors[std::size_t(dominant)*dimension+order[p]]<0?-1:1;
            for(unsigned d=0;d<dimension;++d)result.basis[base+std::size_t(p+1)*dimension+d]=static_cast<float>(sign*vectors[std::size_t(d)*dimension+order[p]]);
        }
        for(unsigned s=0;s<samples;++s)if(result.ids[s]==c)for(unsigned p=0;p<components;++p){double dot=0;for(unsigned d=0;d<dimension;++d)dot+=(input[std::size_t(s)*dimension+d]-centers[std::size_t(c)*dimension+d])*result.basis[base+std::size_t(p+1)*dimension+d];result.weights[std::size_t(s)*components+p]=static_cast<float>(dot);}
        if(auto status=progress(.3F+((quality==2 && clusters>1)?.5F:.7F)*static_cast<float>(c+1)/static_cast<float>(clusters)))return status;
    }
    // 高品質模式依各群仿射子空間的重建誤差重新分群，而非只增加 Voronoi 迭代次數。
    if(quality==2 && clusters>1)for(unsigned iteration=0;iteration<32;++iteration){
        std::vector<unsigned> assigned(samples);bool changed=false;
        for(unsigned sample=0;sample<samples;++sample){double best=INFINITY;unsigned chosen=0;
            for(unsigned cluster=0;cluster<clusters;++cluster){const auto base=std::size_t(cluster)*(components+1)*dimension;double error=0;
                for(unsigned d=0;d<dimension;++d){const double delta=input[std::size_t(sample)*dimension+d]-result.basis[base+d];error+=delta*delta;}
                for(unsigned component=0;component<components;++component){double projection=0;for(unsigned d=0;d<dimension;++d)projection+=(input[std::size_t(sample)*dimension+d]-result.basis[base+d])*result.basis[base+std::size_t(component+1)*dimension+d];error-=projection*projection;}
                error=std::max(0.0,error);if(error<best){best=error;chosen=cluster;}
            }
            assigned[sample]=chosen;changed|=chosen!=result.ids[sample];
        }
        if(!changed)break;
        result.ids=std::move(assigned);
        for(unsigned cluster=0;cluster<clusters;++cluster){std::vector<unsigned> members;std::vector<float> data;
            for(unsigned sample=0;sample<samples;++sample)if(result.ids[sample]==cluster){members.push_back(sample);data.insert(data.end(),input.data()+std::size_t(sample)*dimension,input.data()+std::size_t(sample+1)*dimension);}
            if(!members.empty()){Compressed local;const auto code=compress(data,static_cast<unsigned>(members.size()),coefficients,channels,1,components,1,[](float){return 0U;},local);if(code)return code;
                std::copy(local.basis.begin(),local.basis.end(),result.basis.data()+std::size_t(cluster)*(components+1)*dimension);
                for(unsigned sample=0;sample<members.size();++sample)std::copy_n(local.weights.data()+std::size_t(sample)*components,components,result.weights.data()+std::size_t(members[sample])*components);
            }
            if(auto status=progress(.8F+.19F*(static_cast<float>(iteration)+(static_cast<float>(cluster)+1)/static_cast<float>(clusters))/32))return status;
        }
    }
    if(auto status=progress(1))return status;
    output=std::move(result);return 0;
}
}

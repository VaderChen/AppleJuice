#pragma once
namespace x64arm::d3dx_gutter {
struct Map {
    unsigned width{},height{},face_count{};float gutter{};
    std::vector<unsigned> faces;std::vector<std::array<float,2>> bary,texels;std::vector<std::uint8_t> kinds;
    Map(unsigned w,unsigned h,float distance,const std::vector<std::array<float,2>>& uv,const std::vector<unsigned>& indices):width(w),height(h),face_count(static_cast<unsigned>(indices.size()/3)),gutter(distance){
        const auto size=static_cast<std::size_t>(w)*h;faces.resize(size,UINT32_MAX);bary.resize(size);texels.resize(size);kinds.resize(size);std::vector<double> distances(size,std::numeric_limits<double>::infinity());
        for(unsigned face=0;face<face_count;++face){
            std::array<std::array<double,2>,3> p{};for(unsigned c=0;c<3;++c){const auto& t=uv.at(indices[face*3+c]);p[c]={double(t[0])*w,double(t[1])*h};}
            const auto determinant=(p[1][0]-p[0][0])*(p[2][1]-p[0][1])-(p[1][1]-p[0][1])*(p[2][0]-p[0][0]);if(std::abs(determinant)<1e-15)continue;
            const auto left=std::max(0.,std::floor(std::min({p[0][0],p[1][0],p[2][0]})-distance)),right=std::min(double(w-1),std::ceil(std::max({p[0][0],p[1][0],p[2][0]})+distance));
            const auto top=std::max(0.,std::floor(std::min({p[0][1],p[1][1],p[2][1]})-distance)),bottom=std::min(double(h-1),std::ceil(std::max({p[0][1],p[1][1],p[2][1]})+distance));
            if(left>right || top>bottom)continue;
            for(unsigned y=static_cast<unsigned>(top);y<=static_cast<unsigned>(bottom);++y)for(unsigned x=static_cast<unsigned>(left);x<=static_cast<unsigned>(right);++x){
                const double px=x+.5,py=y+.5;std::array<double,3> weights{};weights[1]=((px-p[0][0])*(p[2][1]-p[0][1])-(py-p[0][1])*(p[2][0]-p[0][0]))/determinant;weights[2]=((p[1][0]-p[0][0])*(py-p[0][1])-(p[1][1]-p[0][1])*(px-p[0][0]))/determinant;weights[0]=1-weights[1]-weights[2];
                const bool inside=weights[0]>=0 && weights[1]>=0 && weights[2]>=0;double d=0,qx=px,qy=py;
                if(!inside){d=std::numeric_limits<double>::infinity();for(unsigned a=0;a<3;++a){const unsigned b=(a+1)%3;const auto dx=p[b][0]-p[a][0],dy=p[b][1]-p[a][1],length=dx*dx+dy*dy;if(length==0)continue;const auto t=std::clamp(((px-p[a][0])*dx+(py-p[a][1])*dy)/length,0.,1.);const auto ex=p[a][0]+t*dx,ey=p[a][1]+t*dy,dd=(px-ex)*(px-ex)+(py-ey)*(py-ey);if(dd<d){d=dd;weights={};weights[a]=1-t;weights[b]=t;qx=ex;qy=ey;}}}
                const auto i=static_cast<std::size_t>(y)*w+x;if(d>double(distance)*distance || d>=distances[i])continue;distances[i]=d;faces[i]=face;kinds[i]=inside?1:2;bary[i]={static_cast<float>(weights[0]),static_cast<float>(weights[1])};texels[i]={static_cast<float>(qx),static_cast<float>(qy)};
            }
        }
    }
    void apply(std::vector<float>& values,unsigned coefficients)const{
        const auto original=values;
        for(std::size_t i=0;i<kinds.size();++i)if(kinds[i]==2){
            const double x=texels[i][0]-.5,y=texels[i][1]-.5;const auto ix=static_cast<int>(std::floor(x)),iy=static_cast<int>(std::floor(y));double total=0;std::vector<double> sum(coefficients);
            for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx){const int px=ix+dx,py=iy+dy;if(px<0 || py<0 || px>=static_cast<int>(width) || py>=static_cast<int>(height))continue;const auto j=static_cast<std::size_t>(py)*width+static_cast<unsigned>(px);if(kinds[j]!=1 && kinds[j]!=4)continue;const auto weight=(dx?x-ix:1-(x-ix))*(dy?y-iy:1-(y-iy));total+=weight;for(unsigned c=0;c<coefficients;++c)sum[c]+=weight*original[j*coefficients+c];}
            if(total>0){for(unsigned c=0;c<coefficients;++c)values[i*coefficients+c]=static_cast<float>(sum[c]/total);}
            else {double closest=std::numeric_limits<double>::infinity();std::size_t nearest=SIZE_MAX;const int radius=static_cast<int>(std::ceil(gutter))+2;
                for(int py=std::max(0,iy-radius);py<=std::min(static_cast<int>(height)-1,iy+radius);++py)for(int px=std::max(0,ix-radius);px<=std::min(static_cast<int>(width)-1,ix+radius);++px){const auto j=static_cast<std::size_t>(py)*width+static_cast<unsigned>(px);if(kinds[j]!=1 && kinds[j]!=4)continue;const auto d=(px-x)*(px-x)+(py-y)*(py-y);if(d<closest){closest=d;nearest=j;}}
                if(nearest!=SIZE_MAX)std::copy_n(original.data()+nearest*coefficients,coefficients,values.data()+i*coefficients);
            }
        }
    }
};
}

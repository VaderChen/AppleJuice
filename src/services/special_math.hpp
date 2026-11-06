#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>

// C++17 特殊數學函式（msvcp140_2.dll 的 __std_smf_*）。宿主 libc++ 未完整提供，這裡以標準定義與穩定遞迴實作，
// 全部以 double 計算；float 版本由呼叫端窄化。定義域外回 NaN，與 MSVC STL 的 domain error 回傳一致。
namespace x64arm::special_math {
inline constexpr double nan_value=std::numeric_limits<double>::quiet_NaN();
inline constexpr double infinity=std::numeric_limits<double>::infinity();
inline constexpr double euler_gamma=0.57721566490153286061;

inline double legendre(unsigned n,double x){
    if(std::isnan(x))return x;
    if(std::abs(x)>1)return nan_value;
    if(x==1)return 1;
    if(x==-1)return n&1U?-1:1;
    double previous=1,current=x;
    if(!n)return previous;
    for(unsigned k=1;k<n;++k){const double next=((2.0*k+1)*x*current-k*previous)/(k+1);previous=current;current=next;}
    return current;
}
// C++ 定義不含 Condon-Shortley 相位。
inline double assoc_legendre(unsigned l,unsigned m,double x){
    if(std::isnan(x))return x;
    if(std::abs(x)>1)return nan_value;
    if(m>l)return 0;
    double value=1;
    if(m){const double root=std::sqrt((1-x)*(1+x));double factor=1;for(unsigned i=0;i<m;++i){value*=factor*root;factor+=2;}}
    if(l==m)return value;
    double next=x*(2.0*m+1)*value;
    for(unsigned k=m+2;k<=l;++k){const double later=((2.0*k-1)*x*next-(k+m-1.0)*value)/(k-m);value=next;next=later;}
    return next;
}
inline double sph_legendre(unsigned l,unsigned m,double theta){
    if(std::isnan(theta))return theta;
    if(m>l)return 0;
    const double x=std::cos(theta);
    const double pi=std::numbers::pi;
    if(!m)return std::sqrt((2.0*l+1)/(4*pi))*legendre(l,x);
    if(std::abs(x)==1)return 0;
    const double sign=m&1U?-1:1;
    const double log_circle=std::log1p(-x*x);
    const double log_pochhammer=std::lgamma(m+0.5)-std::lgamma(static_cast<double>(m));
    const double scale=std::sqrt((2+1.0/m)/(4*pi));
    double current=sign*scale*std::exp(-0.25*std::log(pi)+0.5*(log_pochhammer+m*log_circle));
    double next=x*std::sqrt(2.0*m+3)*current;
    if(l==m)return current;
    if(l==m+1)return next;
    for(unsigned k=m+2;k<=l;++k){
        const double ratio_a=static_cast<double>(k-m)/(k+m),ratio_b=static_cast<double>(k-m-1)/(k+m-1.0);
        const double factor_a=std::sqrt(ratio_a*(2.0*k+1)*(2.0*k-1));
        const double factor_b=std::sqrt(ratio_a*ratio_b*(2.0*k+1)/(2.0*k-3));
        const double later=(x*next*factor_a-(k+m-1.0)*current*factor_b)/(k-m);
        current=next;next=later;
    }
    return next;
}
inline double hermite(unsigned n,double x){
    if(std::isnan(x))return x;
    double previous=1,current=2*x;
    if(!n)return previous;
    for(unsigned k=1;k<n;++k){const double next=2*x*current-2.0*k*previous;previous=current;current=next;}
    return current;
}
inline double assoc_laguerre(unsigned n,unsigned m,double x){
    if(std::isnan(x))return x;
    if(x<0)return nan_value;
    double previous=1;
    if(!n)return previous;
    double current=1.0+m-x;
    for(unsigned k=1;k<n;++k){const double next=((2.0*k+m+1-x)*current-(k+static_cast<double>(m))*previous)/(k+1);previous=current;current=next;}
    return current;
}
inline double laguerre(unsigned n,double x){return assoc_laguerre(n,0,x);}
inline double beta(double x,double y){
    if(std::isnan(x) || std::isnan(y))return nan_value;
    if(x<=0 || y<=0)return nan_value;
    const double sum=x+y;
    if(sum<150)return std::tgamma(x)*std::tgamma(y)/std::tgamma(sum);
    return std::exp(std::lgamma(x)+std::lgamma(y)-std::lgamma(sum));
}
// Borwein 交錯級數；Re(s)>0、s≠1 收斂快，負數以反射公式求值。
inline double zeta_positive(double s){
    if(s>=64)return 1+std::pow(2.0,-s);
    constexpr int terms=48;
    std::array<double,terms+1> d{};
    double term=1,sum=term;
    d[0]=sum;
    for(int i=1;i<=terms;++i){
        term*=4.0*(terms+i-1)*(terms-i+1)/((2.0*i-1)*(2.0*i));
        sum+=term;d[static_cast<std::size_t>(i)]=sum;
    }
    const double total=d[terms];
    double series=0;
    for(int k=0;k<terms;++k){
        const double sign=k&1?-1:1;
        series+=sign*(d[static_cast<std::size_t>(k)]-total)*std::pow(k+1.0,-s);
    }
    return -series/(total*-std::expm1((1-s)*std::numbers::ln2));
}
inline double riemann_zeta(double s){
    if(std::isnan(s))return s;
    if(s==1)return infinity;
    if(s==0)return -0.5;
    if(s>0)return zeta_positive(s);
    if(s==std::floor(s) && std::fmod(-s,2.0)==0)return 0; // 負偶數是平凡零點。
    const double pi=std::numbers::pi;
    if(s<-170)return std::fmod(std::floor(-s/2),2.0)==0?-infinity:infinity;
    return 2*std::pow(2*pi,s-1)*std::sin(pi*s/2)*std::tgamma(1-s)*zeta_positive(1-s);
}
inline double exponential_integral_e1(double x){
    if(x<=1){
        double sum=-euler_gamma-std::log(x),term=1;
        for(int k=1;k<200;++k){term*=-x/k;const double add=-term/k;sum+=add;if(std::abs(add)<std::abs(sum)*1e-17)break;}
        return sum;
    }
    // Lentz 連分數。
    double b=x+1,c=1e300,d=1/b,h=d;
    for(int k=1;k<500;++k){
        const double a=-static_cast<double>(k)*k;b+=2;d=1/(a*d+b);c=b+a/c;const double delta=c*d;h*=delta;
        if(std::abs(delta-1)<1e-16)break;
    }
    return h*std::exp(-x);
}
inline double expint(double x){
    if(std::isnan(x))return x;
    if(x==0)return -infinity;
    if(x<0)return -exponential_integral_e1(-x);
    if(x>700)return infinity;
    if(x<=40){
        double sum=euler_gamma+std::log(x),term=1;
        for(int k=1;k<500;++k){term*=x/k;const double add=term/k;sum+=add;if(add<std::abs(sum)*1e-17)break;}
        return sum;
    }
    double sum=1,term=1;
    for(int k=1;k<60;++k){const double next=term*k/x;if(next>term)break;term=next;sum+=term;if(term<1e-17)break;}
    return std::exp(x)/x*sum;
}
// Carlson 對稱積分（duplication）。
inline double carlson_rf(double x,double y,double z){
    for(int i=0;i<64;++i){
        const double mean=(x+y+z)/3,a=(mean-x)/mean,b=(mean-y)/mean,c=(mean-z)/mean;
        if(std::max({std::abs(a),std::abs(b),std::abs(c)})<0.0012){
            const double e2=a*b-c*c,e3=a*b*c;
            return (1+(e2/24-0.1-3*e3/44)*e2+e3/14)/std::sqrt(mean);
        }
        const double rx=std::sqrt(x),ry=std::sqrt(y),rz=std::sqrt(z),lambda=rx*(ry+rz)+ry*rz;
        x=0.25*(x+lambda);y=0.25*(y+lambda);z=0.25*(z+lambda);
    }
    return 1/std::sqrt((x+y+z)/3);
}
inline double carlson_rd(double x,double y,double z){
    double sum=0,factor=1;
    for(int i=0;i<64;++i){
        const double mean=0.2*(x+y+3*z),a=(mean-x)/mean,b=(mean-y)/mean,c=(mean-z)/mean;
        if(std::max({std::abs(a),std::abs(b),std::abs(c)})<0.0012){
            const double ea=a*b,eb=c*c,ec=ea-eb,ed=ea-6*eb,ee=ed+ec+ec;
            return 3*sum+factor*(1+ed*(-3.0/14+9.0/88*ed-9.0/52*c*ee)+c*(ee/6+c*(-9.0/22*ec+3.0/26*c*ea)))/(mean*std::sqrt(mean));
        }
        const double rx=std::sqrt(x),ry=std::sqrt(y),rz=std::sqrt(z),lambda=rx*(ry+rz)+ry*rz;
        sum+=factor/(rz*(z+lambda));factor*=0.25;
        x=0.25*(x+lambda);y=0.25*(y+lambda);z=0.25*(z+lambda);
    }
    return 3*sum+factor/(((x+y+3*z)/5)*std::sqrt((x+y+3*z)/5));
}
inline double carlson_rc(double x,double y){
    double weight=1;
    if(y<0){const double shifted=x-y;weight=std::sqrt(x)/std::sqrt(shifted);x=shifted;y=-y;}
    for(int i=0;i<64;++i){
        const double mean=(x+2*y)/3,s=(y-mean)/mean;
        if(std::abs(s)<0.0012)return weight*(1+s*s*(0.3+s*(1.0/7+s*(0.375+s*9.0/22))))/std::sqrt(mean);
        const double lambda=2*std::sqrt(x)*std::sqrt(y)+y;
        x=0.25*(x+lambda);y=0.25*(y+lambda);
    }
    return weight/std::sqrt((x+2*y)/3);
}
inline double carlson_rj(double x,double y,double z,double p){
    double sum=0,factor=1,scale=0,shift=0,rc_value=0;
    const bool principal_value=p<=0;
    if(principal_value){
        // 柯西主值：Carlson (1979) 對 p<=0 的轉換。
        const double low=std::min({x,y,z}),high=std::max({x,y,z}),middle=x+y+z-low-high;
        scale=1/(middle-p);shift=scale*(high-middle)*(middle-low);
        const double transformed=middle+shift;
        rc_value=carlson_rc(low*high/middle,p*transformed/middle);
        x=low;y=middle;z=high;p=transformed;
    }
    double mean=0,da=0,db=0,dc=0,dd=0;
    for(int i=0;i<64;++i){
        const double rx=std::sqrt(x),ry=std::sqrt(y),rz=std::sqrt(z),lambda=rx*(ry+rz)+ry*rz;
        const double alpha=p*(rx+ry+rz)+rx*ry*rz;
        sum+=factor*carlson_rc(alpha*alpha,p*(p+lambda)*(p+lambda));factor*=0.25;
        x=0.25*(x+lambda);y=0.25*(y+lambda);z=0.25*(z+lambda);p=0.25*(p+lambda);
        mean=0.2*(x+y+z+2*p);da=(mean-x)/mean;db=(mean-y)/mean;dc=(mean-z)/mean;dd=(mean-p)/mean;
        if(std::max({std::abs(da),std::abs(db),std::abs(dc),std::abs(dd)})<0.0012)break;
    }
    const double ea=da*(db+dc)+db*dc,eb=da*db*dc,ec=dd*dd,ed=ea-3*ec,ee=eb+2*dd*(ea-ec);
    const double value=3*sum+factor*(1+ed*(-3.0/14+9.0/88*ed-9.0/52*ee)+eb*(1.0/6+dd*(-3.0/11+dd*3.0/26))+dd*ea*(1.0/3-dd*3.0/22)-dd*ec/3)/(mean*std::sqrt(mean));
    return principal_value?scale*(shift*value+3*(rc_value-carlson_rf(x,y,z))):value;
}
inline double comp_ellint_1(double k){
    if(std::isnan(k))return k;
    if(std::abs(k)>1)return nan_value;
    if(std::abs(k)==1)return infinity;
    return carlson_rf(0,1-k*k,1);
}
inline double comp_ellint_2(double k){
    if(std::isnan(k))return k;
    if(std::abs(k)>1)return nan_value;
    if(std::abs(k)==1)return 1;
    const double square=k*k;
    return carlson_rf(0,1-square,1)-square/3*carlson_rd(0,1-square,1);
}
inline double comp_ellint_3(double k,double n){
    if(std::isnan(k) || std::isnan(n))return nan_value;
    if(std::abs(k)>1)return nan_value;
    if(n==1 || std::abs(k)==1)return infinity;
    const double square=k*k;
    return carlson_rf(0,1-square,1)+n/3*carlson_rj(0,1-square,1,1-n);
}
// 振幅落在 (-π/2, π/2) 外時以完全積分展開。
inline double reduce_amplitude(double phi,double& periods){
    const double pi=std::numbers::pi;
    periods=std::floor(phi/pi+0.5);
    return phi-periods*pi;
}
inline double ellint_1(double k,double phi){
    if(std::isnan(k) || std::isnan(phi))return nan_value;
    if(std::abs(k)>1)return nan_value;
    double periods=0;const double reduced=reduce_amplitude(phi,periods);
    const double s=std::sin(reduced),c=std::cos(reduced);
    const double partial=s*carlson_rf(c*c,1-k*k*s*s,1);
    return periods!=0?partial+2*periods*comp_ellint_1(k):partial;
}
inline double ellint_2(double k,double phi){
    if(std::isnan(k) || std::isnan(phi))return nan_value;
    if(std::abs(k)>1)return nan_value;
    double periods=0;const double reduced=reduce_amplitude(phi,periods);
    const double s=std::sin(reduced),c=std::cos(reduced),square=k*k,delta=1-square*s*s;
    const double partial=s*carlson_rf(c*c,delta,1)-square/3*s*s*s*carlson_rd(c*c,delta,1);
    return periods!=0?partial+2*periods*comp_ellint_2(k):partial;
}
inline double ellint_3(double k,double n,double phi){
    if(std::isnan(k) || std::isnan(n) || std::isnan(phi))return nan_value;
    if(std::abs(k)>1)return nan_value;
    double periods=0;const double reduced=reduce_amplitude(phi,periods);
    const double s=std::sin(reduced),c=std::cos(reduced),square=k*k,delta=1-square*s*s;
    const double partial=s*carlson_rf(c*c,delta,1)+n/3*s*s*s*carlson_rj(c*c,delta,1,1-n*s*s);
    return periods!=0?partial+2*periods*comp_ellint_3(k,n):partial;
}
// 實數階貝索函式：|x|<2 用 Temme 級數，x≥2 用 Steed 連分數，再向上遞迴。
struct BesselJY {double j{},y{};};
struct BesselIK {double i{},k{};};
inline void temme_gammas(double mu,double& gamma1,double& gamma2,double& plus,double& minus){
    plus=1/std::tgamma(1+mu);minus=1/std::tgamma(1-mu);
    gamma2=(minus+plus)/2;
    gamma1=std::abs(mu)<1e-3?-euler_gamma+0.0420026350340952*mu*mu:(minus-plus)/(2*mu);
}
inline BesselJY bessel_jy(double nu,double x){
    const double pi=std::numbers::pi,small=1e-300,tolerance=1e-16;
    const int order=x<2?static_cast<int>(nu+0.5):std::max(0,static_cast<int>(nu-x+1.5));
    const double mu=nu-order,mu2=mu*mu,xi=1/x,xi2=2*xi,w=xi2/pi;
    int sign=1;double h=nu*xi;if(h<small)h=small;
    double b=xi2*nu,d=0,c=h;
    for(int i=0;i<100000;++i){
        b+=xi2;d=b-d;if(std::abs(d)<small)d=small;
        c=b-1/c;if(std::abs(c)<small)c=small;
        d=1/d;const double delta=c*d;h*=delta;if(d<0)sign=-sign;
        if(std::abs(delta-1)<tolerance)break;
    }
    double j_low=sign*small,j_prime_low=h*j_low,j_saved=j_low,j_prime_saved=j_prime_low,factor=nu*xi;
    for(int l=order;l>=1;--l){const double next=factor*j_low+j_prime_low;factor-=xi;j_prime_low=factor*next-j_low;j_low=next;}
    if(j_low==0)j_low=tolerance;
    const double f=j_prime_low/j_low;
    double j_mu=0,y_mu=0,y_mu1=0;
    if(x<2){
        const double half=0.5*x,pimu=pi*mu,scale=std::abs(pimu)<tolerance?1:pimu/std::sin(pimu);
        d=-std::log(half);double e=mu*d;
        const double scale2=std::abs(e)<tolerance?1:std::sinh(e)/e;
        double gamma1,gamma2,plus,minus;temme_gammas(mu,gamma1,gamma2,plus,minus);
        double ff=2/pi*scale*(gamma1*std::cosh(e)+gamma2*scale2*d);
        e=std::exp(e);
        double p=e/(pi*plus),q=1/(e*pi*minus);
        const double half_pimu=0.5*pimu,scale3=std::abs(half_pimu)<tolerance?1:std::sin(half_pimu)/half_pimu;
        const double r=pi*half_pimu*scale3*scale3;
        c=1;d=-half*half;
        double sum=ff+r*q,sum1=p;
        for(int i=1;i<100000;++i){
            ff=(i*ff+p+q)/(i*i-mu2);c*=d/i;p/=i-mu;q/=i+mu;
            const double del=c*(ff+r*q);sum+=del;sum1+=c*p-i*del;
            if(std::abs(del)<(1+std::abs(sum))*tolerance)break;
        }
        y_mu=-sum;y_mu1=-sum1*xi2;
        const double y_prime=mu*xi*y_mu-y_mu1;
        j_mu=w/(y_prime-f*y_mu);
    }else{
        double a=0.25-mu2,p=-0.5*xi,q=1;
        const double br=2*x;double bi=2;
        double scale=a*xi/(p*p+q*q);
        double cr=br+q*scale,ci=bi+p*scale,den=br*br+bi*bi;
        double dr=br/den,di=-bi/den,dlr=cr*dr-ci*di,dli=cr*di+ci*dr;
        double temp=p*dlr-q*dli;q=p*dli+q*dlr;p=temp;
        double br2=br,bi2=bi;
        for(int i=2;i<100000;++i){
            a+=2*(i-1);bi2+=2;
            dr=a*dr+br2;di=a*di+bi2;
            if(std::abs(dr)+std::abs(di)<small)dr=small;
            scale=a/(cr*cr+ci*ci);cr=br2+cr*scale;ci=bi2-ci*scale;
            if(std::abs(cr)+std::abs(ci)<small)cr=small;
            den=dr*dr+di*di;dr/=den;di/=-den;
            dlr=cr*dr-ci*di;dli=cr*di+ci*dr;
            temp=p*dlr-q*dli;q=p*dli+q*dlr;p=temp;
            if(std::abs(dlr-1)+std::abs(dli)<tolerance)break;
        }
        const double gamma=(p-f)/q;
        j_mu=std::sqrt(w/((p-f)*gamma+q));
        j_mu=std::copysign(j_mu,j_low);
        y_mu=j_mu*gamma;
        const double y_prime=y_mu*(p+q/gamma);
        y_mu1=mu*xi*y_mu-y_prime;
    }
    const double ratio=j_mu/j_low;
    const double j=j_saved*ratio;(void)j_prime_saved;
    for(int i=1;i<=order;++i){const double next=(mu+i)*xi2*y_mu1-y_mu;y_mu=y_mu1;y_mu1=next;}
    return {j,y_mu};
}
inline BesselIK bessel_ik(double nu,double x){
    const double pi=std::numbers::pi,small=1e-300,tolerance=1e-16;
    const int order=static_cast<int>(nu+0.5);
    const double mu=nu-order,mu2=mu*mu,xi=1/x,xi2=2*xi;
    double h=nu*xi;if(h<small)h=small;
    double b=xi2*nu,d=0,c=h;
    for(int i=0;i<100000;++i){
        b+=xi2;d=1/(b+d);c=b+1/c;const double delta=c*d;h*=delta;
        if(std::abs(delta-1)<tolerance)break;
    }
    double i_low=small,i_prime_low=h*i_low,i_saved=i_low,factor=nu*xi;
    for(int l=order;l>=1;--l){const double next=factor*i_low+i_prime_low;factor-=xi;i_prime_low=factor*next+i_low;i_low=next;}
    const double f=i_prime_low/i_low;
    double k_mu=0,k_mu1=0;
    if(x<2){
        const double half=0.5*x,pimu=pi*mu,scale=std::abs(pimu)<tolerance?1:pimu/std::sin(pimu);
        d=-std::log(half);double e=mu*d;
        const double scale2=std::abs(e)<tolerance?1:std::sinh(e)/e;
        double gamma1,gamma2,plus,minus;temme_gammas(mu,gamma1,gamma2,plus,minus);
        double ff=scale*(gamma1*std::cosh(e)+gamma2*scale2*d);
        double sum=ff;
        e=std::exp(e);
        double p=0.5*e/plus,q=0.5/(e*minus);
        c=1;d=half*half;
        double sum1=p;
        for(int i=1;i<100000;++i){
            ff=(i*ff+p+q)/(i*i-mu2);c*=d/i;p/=i-mu;q/=i+mu;
            const double del=c*ff;sum+=del;sum1+=c*(p-i*ff);
            if(std::abs(del)<std::abs(sum)*tolerance)break;
        }
        k_mu=sum;k_mu1=sum1*xi2;
    }else{
        b=2*(1+x);d=1/b;double delta=d;h=d;
        double q1=0,q2=1;
        const double a1=0.25-mu2;double q=a1,a=-a1,s=1+q*delta;
        c=a1;
        for(int i=2;i<100000;++i){
            a-=2*(i-1);
            c=-a*c/i;
            const double q_next=(q1-b*q2)/a;q1=q2;q2=q_next;q+=c*q_next;
            b+=2;d=1/(b+a*d);delta=(b*d-1)*delta;h+=delta;
            const double ds=q*delta;s+=ds;
            if(std::abs(ds/s)<tolerance)break;
        }
        h=a1*h;
        k_mu=std::sqrt(pi/(2*x))*std::exp(-x)/s;
        k_mu1=k_mu*(mu+x+0.5-h)*xi;
    }
    const double k_prime=mu*xi*k_mu-k_mu1;
    const double i_mu=xi/(f*k_mu-k_prime);
    const double i=(i_mu*i_saved)/i_low;
    for(int l=1;l<=order;++l){const double next=(mu+l)*xi2*k_mu1+k_mu;k_mu=k_mu1;k_mu1=next;}
    return {i,k_mu};
}
inline double cyl_bessel_j(double nu,double x){
    if(std::isnan(nu) || std::isnan(x))return nan_value;
    if(nu<0 || x<0)return nan_value;
    if(x==0)return nu==0?1:0;
    return bessel_jy(nu,x).j;
}
inline double cyl_neumann(double nu,double x){
    if(std::isnan(nu) || std::isnan(x))return nan_value;
    if(nu<0 || x<0)return nan_value;
    if(x==0)return -infinity;
    return bessel_jy(nu,x).y;
}
inline double cyl_bessel_i(double nu,double x){
    if(std::isnan(nu) || std::isnan(x))return nan_value;
    if(nu<0 || x<0)return nan_value;
    if(x==0)return nu==0?1:0;
    return bessel_ik(nu,x).i;
}
inline double cyl_bessel_k(double nu,double x){
    if(std::isnan(nu) || std::isnan(x))return nan_value;
    if(nu<0 || x<0)return nan_value;
    if(x==0)return infinity;
    return bessel_ik(nu,x).k;
}
inline double sph_bessel(unsigned n,double x){
    if(std::isnan(x))return x;
    if(x<0)return nan_value;
    if(x==0)return n?0:1;
    return std::sqrt(std::numbers::pi/(2*x))*bessel_jy(n+0.5,x).j;
}
inline double sph_neumann(unsigned n,double x){
    if(std::isnan(x))return x;
    if(x<0)return nan_value;
    if(x==0)return -infinity;
    return std::sqrt(std::numbers::pi/(2*x))*bessel_jy(n+0.5,x).y;
}
inline double hypot3(double x,double y,double z){return std::hypot(x,y,z);}
}

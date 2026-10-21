    if(symbol.starts_with("D3DX")){
        using Matrix=std::array<float,16>;using Vector=std::array<float,4>;
        const auto load=[&](std::uint64_t p,unsigned count){Vector v{};for(unsigned i=0;i<count;++i)v[i]=std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{p+4*i},32)));return v;};
        const auto matrix=[&](std::uint64_t p){Matrix m{};for(unsigned i=0;i<16;++i)m[i]=std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{p+4*i},32)));return m;};
        const auto store=[&](std::uint64_t p,const auto& v,unsigned count){for(unsigned i=0;i<count;++i)memory.store(GuestAddress{p+4*i},32,std::bit_cast<std::uint32_t>(v[i]));};
        const auto f=[&](unsigned i){return !x86_ && i<4?scalar_float(state,i):std::bit_cast<float>(static_cast<std::uint32_t>(arg(i)));};
        const auto dot=[](const Vector& a,const Vector& b,unsigned n=3){float r=0;for(unsigned i=0;i<n;++i)r+=a[i]*b[i];return r;};
        const auto norm=[&](Vector a,unsigned n=3){const auto len=std::sqrt(dot(a,a,n));if(len!=0)for(unsigned i=0;i<n;++i)a[i]/=len;return a;};
        const auto cross=[](const Vector& a,const Vector& b){return Vector{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0],0};};
        const auto identity=[](){Matrix m{};for(unsigned i=0;i<4;++i)m[i*5]=1;return m;};
        const auto multiply=[](const Matrix& a,const Matrix& b){Matrix m{};for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)for(unsigned k=0;k<4;++k)m[i*4+j]+=a[i*4+k]*b[k*4+j];return m;};
        const auto rotation=[](const Vector& q){const auto x=q[0],y=q[1],z=q[2],w=q[3];return Matrix{1-2*(y*y+z*z),2*(x*y+z*w),2*(x*z-y*w),0,2*(x*y-z*w),1-2*(x*x+z*z),2*(y*z+x*w),0,2*(x*z+y*w),2*(y*z-x*w),1-2*(x*x+y*y),0,0,0,0,1};};
        auto result=arg(0);
        if(symbol=="D3DXSHDot" || symbol=="D3DXSHEvalConeLight"){
            const auto order=static_cast<unsigned>(arg(0));if(order<2 || order>6){if(symbol=="D3DXSHDot")conversion_result(state,x86_,0,true);arg.return_to(state,symbol=="D3DXSHDot"?0:0x8876086cU);return std::nullopt;}
            std::array<float,36> a{},b{},c{};
            if(symbol=="D3DXSHDot"){
                for(unsigned i=0;i<order*order;++i){a[i]=load(arg(1)+4*i,1)[0];b[i]=load(arg(2)+4*i,1)[0];}
                conversion_result(state,x86_,sh_dot(order,a.data(),b.data()),true);result=0;
            }else{
                if(!arg(1) || !arg(6)){arg.return_to(state,0x8876086cU);return std::nullopt;}
                const auto direction=load(arg(1),3);const Vector intensity{f(3),f(4),f(5),0};
                if(!sh_cone(order,direction.data(),f(2),intensity.data(),a.data(),arg(7)?b.data():nullptr,arg(8)?c.data():nullptr)){arg.return_to(state,0x8876086cU);return std::nullopt;}
                store(arg(6),a,order*order);if(arg(7))store(arg(7),b,order*order);if(arg(8))store(arg(8),c,order*order);result=0;
            }
        }else if(symbol.starts_with("D3DXSH")){
            const auto order=static_cast<unsigned>(arg(1));
            if(!arg(0) || order<2 || order>6){arg.return_to(state,0);return std::nullopt;}
            const auto count=order*order;std::array<float,36> out{},a{},b{};
            const auto coefficients=[&](std::uint64_t p,auto& v){for(unsigned i=0;i<count;++i)v[i]=std::bit_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{p+4*i},32)));};
            if(symbol=="D3DXSHEvalDirection"){
                const auto v=load(arg(2),3);sh_evaluate(out.data(),order,v.data());
            }else if(symbol=="D3DXSHRotate"){
                const auto m=matrix(arg(2));coefficients(arg(3),a);sh_rotate(out.data(),order,m.data(),a.data());
            }else{
                coefficients(arg(2),a);
                if(symbol=="D3DXSHScale")sh_scale(out.data(),order,a.data(),f(3));
                else if(symbol=="D3DXSHAdd"){coefficients(arg(3),b);sh_add(out.data(),order,a.data(),b.data());}
                else throw HostCallFault("尚未實作的球諧數學入口");
            }
            store(arg(0),out,count);
        }else if(symbol=="D3DXOptimizeFaces" || symbol=="D3DXOptimizeVertices"){
#include "d3dx_mesh_remap.inc"
        }else if(symbol=="D3DXFloat32To16Array"){
            const auto count=static_cast<std::uint32_t>(arg(2));memory.check(GuestAddress{arg(1)},std::uint64_t{count}*4,Access::Read);memory.check(GuestAddress{arg(0)},std::uint64_t{count}*2,Access::Write);
            std::vector<std::uint16_t> values;values.reserve(count);const auto rounding=softfloat_roundingMode;softfloat_roundingMode=softfloat_round_near_even;
            for(std::uint64_t i=0;i<count;++i)values.push_back(f32_to_f16({static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)+4*i},32))}).v);
            softfloat_roundingMode=rounding;for(std::uint64_t i=0;i<count;++i)memory.store(GuestAddress{arg(0)+2*i},16,values[i]);
        }else if(symbol=="D3DXFloat16To32Array"){
            const auto count=arg(2);memory.check(GuestAddress{arg(1)},count*2,Access::Read);memory.check(GuestAddress{arg(0)},count*4,Access::Write);
            std::vector<std::uint32_t> values;values.reserve(count);for(std::uint64_t i=0;i<count;++i)values.push_back(f16_to_f32({static_cast<std::uint16_t>(memory.load(GuestAddress{arg(1)+2*i},16))}).v);
            for(std::uint64_t i=0;i<count;++i)memory.store(GuestAddress{arg(0)+4*i},32,values[i]);
        }else if(symbol=="D3DXVec2Normalize" || symbol=="D3DXVec3Normalize" || symbol=="D3DXVec4Normalize" || symbol=="D3DXQuaternionNormalize" || symbol=="D3DXQuaternionInverse"){
            const unsigned n=symbol=="D3DXVec2Normalize"?2:symbol=="D3DXVec3Normalize"?3:4;auto v=load(arg(1),n);
            if(symbol=="D3DXQuaternionInverse"){const auto length=dot(v,v,4);for(unsigned i=0;i<4;++i)v[i]=length!=0?(i<3?-v[i]:v[i])/length:0;}else v=norm(v,n);store(arg(0),v,n);
        }else if(symbol=="D3DXQuaternionMultiply"){
            const auto a=load(arg(1),4),b=load(arg(2),4);const auto c=cross(b,a);
            Vector q{b[3]*a[0]+a[3]*b[0]+c[0],b[3]*a[1]+a[3]*b[1]+c[1],b[3]*a[2]+a[3]*b[2]+c[2],b[3]*a[3]-dot(a,b)};store(arg(0),q,4);
        }else if(symbol=="D3DXQuaternionRotationMatrix"){
            const auto m=matrix(arg(1));Vector q{};const auto trace=m[0]+m[5]+m[10];
            if(trace>0){const auto s=2*std::sqrt(trace+1);q={(m[6]-m[9])/s,(m[8]-m[2])/s,(m[1]-m[4])/s,s/4};}
            else{unsigned i=0;if(m[5]>m[0])i=1;if(m[10]>m[i*5])i=2;const auto j=(i+1)%3,k=(j+1)%3;const auto s=2*std::sqrt(1+m[i*5]-m[j*5]-m[k*5]);q[i]=s/4;q[j]=(m[i*4+j]+m[j*4+i])/s;q[k]=(m[i*4+k]+m[k*4+i])/s;q[3]=(m[j*4+k]-m[k*4+j])/s;}store(arg(0),q,4);
        }else if(symbol=="D3DXPlaneFromPointNormal"){
            const auto p=load(arg(1),3);auto n=load(arg(2),3);n[3]=-dot(p,n);store(arg(0),n,4);
        }else if(symbol=="D3DXQuaternionRotationYawPitchRoll"){
            const auto cy=std::cos(f(1)/2),sy=std::sin(f(1)/2),cp=std::cos(f(2)/2),sp=std::sin(f(2)/2),cr=std::cos(f(3)/2),sr=std::sin(f(3)/2);
            store(arg(0),Vector{cy*sp*cr+sy*cp*sr,sy*cp*cr-cy*sp*sr,cy*cp*sr-sy*sp*cr,cy*cp*cr+sy*sp*sr},4);
        }else if(symbol=="D3DXVec3Unproject"){
            const auto input=load(arg(1),3);std::array<float,6> viewport{};for(unsigned i=0;i<6;++i)viewport[i]=i<4?static_cast<float>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)+4*i},32))):load(arg(2)+4*i,1)[0];
            const auto p=matrix(arg(3)),v=matrix(arg(4)),w=matrix(arg(5));Vector out{};d3dx_unproject(out.data(),input.data(),viewport.data(),p.data(),v.data(),w.data());store(arg(0),out,3);
        }else if(symbol.starts_with("D3DXVec")){
            const bool v4=symbol=="D3DXVec4Transform",normal=symbol=="D3DXVec3TransformNormal",coord=symbol=="D3DXVec3TransformCoord";auto v=load(arg(1),v4?4:3);if(!v4)v[3]=normal?0:1;const auto m=matrix(arg(2));Vector out{};
            for(unsigned j=0;j<4;++j)for(unsigned i=0;i<4;++i)out[j]+=v[i]*m[i*4+j];if(coord)for(unsigned i=0;i<3;++i)out[i]/=out[3];store(arg(0),out,(normal || coord)?3:4);
        }else if(symbol=="D3DXComputeBoundingBox"){
            const auto count=static_cast<std::uint32_t>(arg(1)),stride=static_cast<std::uint32_t>(arg(2));if(!count || stride<12 || !arg(0) || !arg(3) || !arg(4)){arg.return_to(state,0x8876086cU);return std::nullopt;}
            auto lo=load(arg(0),3),hi=lo;for(unsigned i=1;i<count;++i){const auto point=load(arg(0)+std::uint64_t{i}*stride,3);for(unsigned j=0;j<3;++j){lo[j]=std::min(lo[j],point[j]);hi[j]=std::max(hi[j],point[j]);}}store(arg(3),lo,3);store(arg(4),hi,3);result=0;
        }else if(symbol=="D3DXComputeBoundingSphere"){
            const auto count=static_cast<std::uint32_t>(arg(1)),stride=static_cast<std::uint32_t>(arg(2));if(!count || stride<12){arg.return_to(state,0x8876086cU);return std::nullopt;}Vector center{};
            for(unsigned i=0;i<count;++i){const auto v=load(arg(0)+std::uint64_t{i}*stride,3);for(unsigned j=0;j<3;++j)center[j]+=v[j]/static_cast<float>(count);}
            float radius=0;for(unsigned i=0;i<count;++i){auto v=load(arg(0)+std::uint64_t{i}*stride,3);for(unsigned j=0;j<3;++j)v[j]-=center[j];radius=std::max(radius,std::sqrt(dot(v,v)));}store(arg(3),center,3);memory.store(GuestAddress{arg(4)},32,std::bit_cast<std::uint32_t>(radius));result=0;
        }else{
            auto m=identity();
            if(symbol=="D3DXMatrixScaling"){m[0]=f(1);m[5]=f(2);m[10]=f(3);}
            else if(symbol=="D3DXMatrixTranslation"){m[12]=f(1);m[13]=f(2);m[14]=f(3);}
            else if(symbol=="D3DXMatrixMultiply")m=multiply(matrix(arg(1)),matrix(arg(2)));
            else if(symbol=="D3DXMatrixMultiplyTranspose"){const auto product=multiply(matrix(arg(1)),matrix(arg(2)));for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)m[i*4+j]=product[j*4+i];}
            else if(symbol=="D3DXMatrixRotationAxis"){auto axis=norm(load(arg(1),3));const auto sine=std::sin(f(2)/2);for(unsigned i=0;i<3;++i)axis[i]*=sine;axis[3]=std::cos(f(2)/2);m=rotation(axis);}
            else if(symbol=="D3DXMatrixTranspose"){const auto a=matrix(arg(1));for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)m[i*4+j]=a[j*4+i];}
            else if(symbol=="D3DXMatrixRotationQuaternion")m=rotation(load(arg(1),4));
            else if(symbol=="D3DXMatrixRotationYawPitchRoll"){
                const auto cy=std::cos(f(1)/2),sy=std::sin(f(1)/2),cp=std::cos(f(2)/2),sp=std::sin(f(2)/2),cr=std::cos(f(3)/2),sr=std::sin(f(3)/2);
                m=rotation({cy*sp*cr+sy*cp*sr,sy*cp*cr-cy*sp*sr,cy*cp*sr-sy*sp*cr,cy*cp*cr+sy*sp*sr});
            }else if(symbol=="D3DXMatrixRotationX" || symbol=="D3DXMatrixRotationY" || symbol=="D3DXMatrixRotationZ"){
                Vector q{};q[symbol.ends_with('X')?0:symbol.ends_with('Y')?1:2]=std::sin(f(1)/2);q[3]=std::cos(f(1)/2);m=rotation(q);
            }else if(symbol=="D3DXMatrixPerspectiveFovLH"){
                m={};const auto ys=1/std::tan(f(1)/2),zn=f(3),zf=f(4);m[0]=ys/f(2);m[5]=ys;m[10]=zf/(zf-zn);m[11]=1;m[14]=-zn*zf/(zf-zn);
            }else if(symbol=="D3DXMatrixOrthoLH" || symbol=="D3DXMatrixOrthoRH"){
                const auto zn=f(3),zf=f(4);m[0]=2/f(1);m[5]=2/f(2);m[10]=1/(symbol.ends_with("LH")?zf-zn:zn-zf);m[14]=zn/(zn-zf);
            }else if(symbol=="D3DXMatrixOrthoOffCenterLH"){
                const auto l=f(1),r=f(2),b=f(3),t=f(4),zn=f(5),zf=f(6);m[0]=2/(r-l);m[5]=2/(t-b);m[10]=1/(zf-zn);m[12]=-(l+r)/(r-l);m[13]=-(t+b)/(t-b);m[14]=-zn/(zf-zn);
            }else if(symbol=="D3DXMatrixLookAtLH"){
                const auto eye=load(arg(1),3),at=load(arg(2),3),up=load(arg(3),3);Vector z{};for(unsigned i=0;i<3;++i)z[i]=at[i]-eye[i];z=norm(z);const auto x=norm(cross(up,z)),y=cross(z,x);
                for(unsigned i=0;i<3;++i){m[i*4]=x[i];m[i*4+1]=y[i];m[i*4+2]=z[i];}m[12]=-dot(x,eye);m[13]=-dot(y,eye);m[14]=-dot(z,eye);
            }else if(symbol=="D3DXMatrixReflect"){
                auto p=load(arg(1),4);const auto len=std::sqrt(dot(p,p));if(len!=0)for(auto& v:p)v/=len;for(unsigned i=0;i<4;++i)for(unsigned j=0;j<3;++j)m[i*4+j]-=2*p[i]*p[j];
            }else if(symbol=="D3DXMatrixInverse"){
                const auto a=matrix(arg(2));double work[4][8]{};double determinant=1;bool singular=false;
                for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j){work[i][j]=a[i*4+j];work[i][j+4]=i==j?1:0;}
                for(unsigned i=0;i<4;++i){unsigned pivot=i;for(unsigned j=i+1;j<4;++j)if(std::abs(work[j][i])>std::abs(work[pivot][i]))pivot=j;
                    if(work[pivot][i]==0){singular=true;determinant=0;break;}if(pivot!=i){for(unsigned j=0;j<8;++j)std::swap(work[i][j],work[pivot][j]);determinant=-determinant;}
                    const auto value=work[i][i];determinant*=value;for(auto& v:work[i])v/=value;for(unsigned k=0;k<4;++k)if(k!=i){const auto factor=work[k][i];for(unsigned j=0;j<8;++j)work[k][j]-=factor*work[i][j];}}
                if(arg(1))memory.store(GuestAddress{arg(1)},32,std::bit_cast<std::uint32_t>(static_cast<float>(determinant)));if(singular){arg.return_to(state,0);return std::nullopt;}
                for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)m[i*4+j]=static_cast<float>(work[i][j+4]);
            }else throw HostCallFault("尚未實作的 D3DX 數學入口");
            store(arg(0),m,16);
        }
        arg.return_to(state,result);return std::nullopt;
    }
